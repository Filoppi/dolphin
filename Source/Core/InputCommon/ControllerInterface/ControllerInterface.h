// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>

#include "Common/Matrix.h"
#include "Common/WindowSystemInfo.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/InputChannel.h"

// enable/disable sources
#ifdef _WIN32
#define CIFACE_USE_WIN32
#endif
#ifdef HAVE_X11
#define CIFACE_USE_XLIB
#endif
#if defined(__APPLE__)
#define CIFACE_USE_OSX
#endif
#if defined(HAVE_LIBEVDEV) && defined(HAVE_LIBUDEV)
#define CIFACE_USE_EVDEV
#endif
#if defined(USE_PIPES)
#define CIFACE_USE_PIPES
#endif
#define CIFACE_USE_DUALSHOCKUDPCLIENT

//
// ControllerInterface
//
// Some crazy shit I made to control different device inputs and outputs
// from lots of different sources, hopefully more easily.
//
class ControllerInterface : public ciface::Core::DeviceContainer
{
public:
  using HotplugCallbackHandle = std::list<std::function<void()>>::iterator;

  ControllerInterface() : m_is_init(false) {}
  void Initialize(const WindowSystemInfo& wsi);
  // Not thread safe
  void ChangeWindow(void* hwnd, bool is_exit = false);
  // Can be called by any thread at any time (when initialized).
  // The window can change even if because_of_window_change is false.
  void RefreshDevices(bool because_of_window_change = false);
  void Shutdown();
  bool AddDevice(std::shared_ptr<ciface::Core::Device> device);
  // Removes all the devices the function returns true to.
  // If we need all the shared ptrs to them to be destroyed immediately,
  // you can set force_devices_release to true.
  void RemoveDevice(std::function<bool(const ciface::Core::Device*)> callback,
                    bool force_devices_release = false);
  // This is mandatory to use on device populations functions that can be called concurrently by
  // more than one thread, or that are called by a single other thread.
  // Without this, our devices list might end up in a mixed state.
  void PlatformPopulateDevices(std::function<void()> callback);
  bool IsInit() const { return m_is_init; }

  void UpdateInput(ciface::InputChannel input_channel, double delta_seconds,
                   double target_delta_seconds = 0.0, u32 updates_per_target = 1);
  void SetChannelRunning(ciface::InputChannel input_channel, bool running);

  // Set adjustment from the full render window aspect-ratio to the drawn aspect-ratio.
  // Used to fit mouse cursor inputs to the relevant region of the render window.
  void SetAspectRatioAdjustment(float);

  // Calculated from the aspect-ratio adjustment.
  // Inputs based on window coordinates should be multiplied by this.
  Common::Vec2 GetWindowInputScale() const;

  HotplugCallbackHandle RegisterDevicesChangedCallback(std::function<void(void)> callback);
  void UnregisterDevicesChangedCallback(const HotplugCallbackHandle& handle);
  void InvokeDevicesChangedCallbacks() const;

  static ciface::InputChannel GetCurrentInputChannel();
  static bool HasInputChannelJustStarted();
  static double GetCurrentInputDeltaSeconds();
  static double GetTargetInputDeltaSeconds();
  static s32 GetInputUpdatesPerTarget();
  static double GetCurrentRealInputDeltaSeconds();

private:
  void ClearDevices();

  std::list<std::function<void()>> m_devices_changed_callbacks;
  mutable std::recursive_mutex m_devices_pupulation_mutex;
  mutable std::mutex m_pre_pupulation_mutex;
  mutable std::mutex m_callbacks_mutex;
  std::atomic<bool> m_is_init;
  // Doesn't need to be atomic anymore, not even counter, but we keep it so
  // in case we changed the design back to what it was
  std::atomic<int> m_is_populating_devices;
  WindowSystemInfo m_wsi;
  std::atomic<float> m_aspect_ratio_adjustment = 1.f;
};

extern ControllerInterface g_controller_interface;
