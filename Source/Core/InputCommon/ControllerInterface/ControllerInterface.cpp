// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include <algorithm>
#include <chrono>

#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"

#ifdef CIFACE_USE_WIN32
#include "InputCommon/ControllerInterface/Win32/Win32.h"
#endif
#ifdef CIFACE_USE_XLIB
#include "InputCommon/ControllerInterface/Xlib/XInput2.h"
#endif
#ifdef CIFACE_USE_OSX
#include "InputCommon/ControllerInterface/OSX/OSX.h"
#include "InputCommon/ControllerInterface/Quartz/Quartz.h"
#endif
#ifdef CIFACE_USE_SDL
#include "InputCommon/ControllerInterface/SDL/SDL.h"
#endif
#ifdef CIFACE_USE_ANDROID
#include "InputCommon/ControllerInterface/Android/Android.h"
#endif
#ifdef CIFACE_USE_EVDEV
#include "InputCommon/ControllerInterface/evdev/evdev.h"
#endif
#ifdef CIFACE_USE_PIPES
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"
#endif
#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#endif

ControllerInterface g_controller_interface;

using Clock = std::chrono::steady_clock;

// We need to save which input channel we are in by thread, so we can access the correct input update values
// in different threads by input channel.
// We start from InputChannel::Host on all threads as hotkeys are updated from a worker thread,
// but UI can read from the main thread. This will never interfere with game threads.
static thread_local ciface::InputChannel tls_input_channel = ciface::InputChannel::Host;
static double s_input_channels_delta_seconds[u8(ciface::InputChannel::Max)];
static double s_input_channels_target_delta_seconds[u8(ciface::InputChannel::Max)];
static double s_input_channels_real_delta_seconds[u8(ciface::InputChannel::Max)];
static Clock::time_point s_input_channels_last_update[u8(ciface::InputChannel::Max)];

void ControllerInterface::Initialize(const WindowSystemInfo& wsi)
{
  if (m_is_init)
    return;

  m_wsi = wsi;

  m_is_populating_devices = 1;

  // Allow backends to add devices as soon as they are initialized.
  // This is likely useless as their thread would lock and devices are cleaned just below here.
  m_is_init = true;

  m_devices_mutex.lock();

#ifdef CIFACE_USE_WIN32
  ciface::Win32::Init(wsi.render_window);
#endif
#ifdef CIFACE_USE_XLIB
// nothing needed
#endif
#ifdef CIFACE_USE_OSX
  if (m_wsi.type == WindowSystemType::MacOS)
    ciface::OSX::Init(wsi.render_window);
// nothing needed for Quartz
#endif
#ifdef CIFACE_USE_SDL
  ciface::SDL::Init();
#endif
#ifdef CIFACE_USE_ANDROID
// nothing needed
#endif
#ifdef CIFACE_USE_EVDEV
  ciface::evdev::Init();
#endif
#ifdef CIFACE_USE_PIPES
// nothing needed
#endif
#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
  ciface::DualShockUDPClient::Init();
#endif

  RefreshDevices();

  const bool devices_empty = m_devices.empty();

  m_devices_mutex.unlock();

  if (m_is_populating_devices.fetch_sub(1) == 1 && !devices_empty)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::ChangeWindow(void* hwnd, bool is_exit)
{
  if (!m_is_init)
    return;

  // This shouldn't use render_surface so no need to update it.
  m_wsi.render_window = hwnd;
  
  if (is_exit)  // No need to re-add devices
    ClearDevices();
  else  // We could only repupulate devices that depend on the window but it's not that simple
    RefreshDevices();
}

void ControllerInterface::RefreshDevices()
{
  if (!m_is_init)
    return;

  m_is_populating_devices.fetch_add(1);

  // We lock m_devices_mutex here to make everything simpler.
  // Multiple devices classes have their own "hotplug" thread, and can add/remove devices at any time,
  // while actual writes to "m_devices" are safe, the order in which they happen is not.
  // That means a thread could be adding devices while we are removing them, or removing them as we are populating them.
  // The best way of approaching this (for performance) would be to individually implement this in every devices class,
  // but it's fairly complicated and this should never hang the emulation thread anyway.
  m_devices_mutex.lock();

  // Make sure shared_ptr<Device> objects are released before repopulating.
  ClearDevices();

  // Some of these calls won't immediately populate devices, but will do it async
  // with their own PlatformPopulateDevices().

  // TODO: some devices groups, specifically OSX, SDL and evdev, can still
  // add and remove devices from multiple threads in "unsafe" ways.

#ifdef CIFACE_USE_WIN32
  ciface::Win32::PopulateDevices(m_wsi.render_window);
#endif
#ifdef CIFACE_USE_XLIB
  if (m_wsi.type == WindowSystemType::X11)
    ciface::XInput2::PopulateDevices(m_wsi.render_window);
#endif
#ifdef CIFACE_USE_OSX
  if (m_wsi.type == WindowSystemType::MacOS)
  {
    ciface::OSX::PopulateDevices(m_wsi.render_window);
    ciface::Quartz::PopulateDevices(m_wsi.render_window);
  }
#endif
#ifdef CIFACE_USE_SDL
  ciface::SDL::PopulateDevices();
#endif
#ifdef CIFACE_USE_ANDROID
  ciface::Android::PopulateDevices();
#endif
#ifdef CIFACE_USE_EVDEV
  ciface::evdev::PopulateDevices();
#endif
#ifdef CIFACE_USE_PIPES
  ciface::Pipes::PopulateDevices();
#endif
#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
  ciface::DualShockUDPClient::PopulateDevices();
#endif

  WiimoteReal::ProcessWiimotePool();

  m_devices_mutex.unlock();

  if (m_is_populating_devices.fetch_sub(1) == 1)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::PlatformPopulateDevices(std::function<void()> callback)
{
  if (!m_is_init)
    return;

  m_is_populating_devices.fetch_add(1);

  {
    std::lock_guard lk(m_devices_mutex);

    callback();
  }

  if (m_is_populating_devices.fetch_sub(1) == 1)
    InvokeDevicesChangedCallbacks();
}

// Remove all devices and call library cleanup functions
void ControllerInterface::Shutdown()
{
  if (!m_is_init)
    return;

  // Prevent additional devices from being added during shutdown.
  m_is_init = false;
  // Additional safety measure to avoid InvokeDevicesChangedCallbacks()
  m_is_populating_devices = 1;

  // Update control references so shared_ptr<Device>s are freed up BEFORE we shutdown the backends.
  ClearDevices();

#ifdef CIFACE_USE_WIN32
  ciface::Win32::DeInit();
#endif
#ifdef CIFACE_USE_XLIB
// nothing needed
#endif
#ifdef CIFACE_USE_OSX
  ciface::OSX::DeInit();
  ciface::Quartz::DeInit();
#endif
#ifdef CIFACE_USE_SDL
  ciface::SDL::DeInit();
#endif
#ifdef CIFACE_USE_ANDROID
// nothing needed
#endif
#ifdef CIFACE_USE_EVDEV
  ciface::evdev::Shutdown();
#endif
#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
  ciface::DualShockUDPClient::DeInit();
#endif

  // Make sure no devices had been added within Shutdown() in the time
  // between checking they checked atomic m_is_init bool and we changed it.
  // We couldn't have locked m_devices_mutex for the whole Shutdown() as it could cause deadlocks.
  // Note that this is still not 100% safe as some backends are shutdown in other places, possibly
  // adding devices after we have shut down, but the chances of that happening are basically zero.
  ClearDevices();
}

void ControllerInterface::ClearDevices()
{
  {
    std::lock_guard lk(m_devices_mutex);

    if (m_devices.empty())
      return;

    // Set outputs to ZERO before destroying device.
    // This isn't desirable if the same devices are re-connected.
    // The only solution would be to cache the states of the output and
    // reapply them to the devices of the same name.
    for (const auto& d : m_devices)
      d->ResetOutput();

    m_devices.clear();
  }

  InvokeDevicesChangedCallbacks();
}

void ControllerInterface::AddDevice(std::shared_ptr<ciface::Core::Device> device)
{
  // If we are shutdown (or in process of shutting down) ignore this request:
  if (!m_is_init)
    return;

  {
    std::lock_guard lk(m_devices_mutex);

    const auto is_id_in_use = [&device, this](int id) {
      return std::any_of(m_devices.begin(), m_devices.end(), [&device, &id](const auto& d) {
        return d->GetSource() == device->GetSource() && d->GetName() == device->GetName() &&
               d->GetId() == id;
      });
    };

    const auto preferred_id = device->GetPreferredId();
    if (preferred_id.has_value() && !is_id_in_use(*preferred_id))
    {
      // Use the device's preferred ID if available.
      device->SetId(*preferred_id);
    }
    else
    {
      // Find the first available ID to use.
      int id = 0;
      while (is_id_in_use(id))
        ++id;

      device->SetId(id);
    }

    NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Added device: {}", device->GetQualifiedName());
    m_devices.emplace_back(std::move(device));

    // We can't (and don't want) to control the order in which devices are added, but we
    // need their order to be consistent, and we need the same one to always be the first, where
    // present (the keyboard and mouse device usually). This is because when defaulting a
    // controller profile, it will automatically select the first device in the list as its default.
    std::sort(m_devices.begin(), m_devices.end(),
              [](const std::shared_ptr<ciface::Core::Device>& a,
                 const std::shared_ptr<ciface::Core::Device>& b) {
                // It would be nice to sort devices by Source then Name then ID but it's better
                // to leave them sorted by the add order, which also avoids breaking the order
                // on other platforms that are less tested.
                return a->GetSortPriority() > b->GetSortPriority();
              });
  }

  if (!m_is_populating_devices)
    InvokeDevicesChangedCallbacks();
}

void ControllerInterface::RemoveDevice(std::function<bool(const ciface::Core::Device*)> callback)
{
  // If we are shutdown (or in process of shutting down) ignore this request:
  if (!m_is_init)
    return;

  bool any_removed;
  {
    std::lock_guard lk(m_devices_mutex);
    auto it = std::remove_if(
        m_devices.begin(), m_devices.end(), [&callback](const auto& dev) {
          if (callback(dev.get()))
          {
            dev->ResetOutput();
            NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Removed device: {}", dev->GetQualifiedName());
            return true;
          }
          return false;
        });
    size_t prev_size = m_devices.size();
    m_devices.erase(it, m_devices.end());
    any_removed = m_devices.size() != prev_size;
  }

  if (!m_is_populating_devices && any_removed)
    InvokeDevicesChangedCallbacks();
}

// Update input for all devices if lock can be acquired without waiting.
void ControllerInterface::UpdateInput(ciface::InputChannel input_channel, double delta_seconds,
                                      double target_delta_seconds)
{
  // We set the read/write input channel here, see Device::RelativeInput for more info.
  // The other information is used by FunctionExpression(s) to determine their timings.
  // Inputs for this channel will be read immediately after this call.
  // Make sure to not read them after the input channel has changed again (on the same thread).
  tls_input_channel = input_channel;
  // This is not the actual world elapsed time, it's the emulation elapsed time
  s_input_channels_delta_seconds[u8(tls_input_channel)] = delta_seconds;
  // Delta seconds can be bigger or smaller than the target one, but they should average out
  s_input_channels_target_delta_seconds[u8(tls_input_channel)] =
      target_delta_seconds > 0.f ? target_delta_seconds : delta_seconds;

  // Calculate the real/world elapsed time.
  // Useful to turn relative axes into "rate of change"/speed values usable by games
  const auto now = Clock::now();
  Clock::time_point& input_channel_last_update =
      s_input_channels_last_update[u8(tls_input_channel)];
  s_input_channels_real_delta_seconds[u8(tls_input_channel)] =
      std::chrono::duration_cast<std::chrono::duration<double>>(now - input_channel_last_update)
          .count();
  input_channel_last_update = now;

  // Prefer outdated values over blocking UI or CPU thread (avoids short but noticeable frame drop)
  if (m_devices_mutex.try_lock())
  {
    std::lock_guard lk(m_devices_mutex, std::adopt_lock);

    // Device::UpdateInput() would modify values read by ControlReference(s)
    const auto lock = ControllerEmu::EmulatedController::GetDevicesInputLock();

    for (auto& d : m_devices)
      d->UpdateInput();
  }
}

// Call this when you are "closing" (stopping to update) an input channel
void ControllerInterface::Reset(ciface::InputChannel input_channel)
{
  if (!m_is_init)
    return;

  // No need to clean s_input_channels_delta_seconds and the others
  tls_input_channel = input_channel;
  //To review: also call this on Init? Or maybe put Clock::now() in the constructor and shutdown?
  s_input_channels_last_update[u8(tls_input_channel)] = Clock::now();

  std::lock_guard lk(m_devices_mutex);

  for (auto& d : m_devices)
  {
    {
      const auto lock = ControllerEmu::EmulatedController::GetDevicesInputLock();
      d->ResetInput();
    }
    //To review: is still necessary/correct? Check when this is called. It would only be necessary if outputs aren't already resetting themselves.
    // This isn't 100% right as other input channels could still be changing the outputs but
    // as of now that could never happen and even so, it's still better than stuck output values
    d->ResetOutput();
  }
}

void ControllerInterface::SetAspectRatioAdjustment(float value)
{
  m_aspect_ratio_adjustment = value;
}

Common::Vec2 ControllerInterface::GetWindowInputScale() const
{
  const auto ar = m_aspect_ratio_adjustment.load();

  if (ar > 1)
    return {1.f, ar};
  else
    return {1 / ar, 1.f};
}

// Register a callback to be called when a device is added or removed (as from the input backends'
// hotplug thread), or when devices are refreshed. Can be called from "any" thread.
// Returns a handle for later removing the callback.
ControllerInterface::HotplugCallbackHandle
ControllerInterface::RegisterDevicesChangedCallback(std::function<void()> callback)
{
  std::lock_guard<std::mutex> lk(m_callbacks_mutex);
  m_devices_changed_callbacks.emplace_back(std::move(callback));
  return std::prev(m_devices_changed_callbacks.end());
}

// Unregister a device callback.
void ControllerInterface::UnregisterDevicesChangedCallback(const HotplugCallbackHandle& handle)
{
  std::lock_guard<std::mutex> lk(m_callbacks_mutex);
  m_devices_changed_callbacks.erase(handle);
}

// Invoke all callbacks that were registered
void ControllerInterface::InvokeDevicesChangedCallbacks() const
{
  std::lock_guard<std::mutex> lk(m_callbacks_mutex);
  for (const auto& callback : m_devices_changed_callbacks)
    callback();
}

ciface::InputChannel ControllerInterface::GetCurrentInputChannel()
{
  return tls_input_channel;
}

double ControllerInterface::GetCurrentInputDeltaSeconds()
{
  return s_input_channels_delta_seconds[u8(tls_input_channel)];
}

double ControllerInterface::GetTargetInputDeltaSeconds()
{
  return s_input_channels_target_delta_seconds[u8(tls_input_channel)];
}

double ControllerInterface::GetCurrentRealInputDeltaSeconds()
{
  return s_input_channels_real_delta_seconds[u8(tls_input_channel)];
}
