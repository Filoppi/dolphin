// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// idk in case I wanted to change it from double or something, idk what's best
typedef double ControlState;

namespace ciface
{
// 100Hz which homebrew docs very roughly imply is within WiiMote normal
// range, used for periodic haptic effects though often ignored by devices
// TODO: Make this configurable.
constexpr int RUMBLE_PERIOD_MS = 10;

// This needs to be at least as long as the longest rumble that might ever be played.
// Too short and it's going to stop in the middle of a long effect.
// Infinite values are invalid for ramp effects and probably not sensible.
constexpr int RUMBLE_LENGTH_MS = 1000 * 10;

// All inputs (other than accel/gyro) return 1.0 as their maximum value.
// Battery inputs will almost always be mapped to the "Battery" setting which is a percentage.
// If someone actually wants to map a battery input to a regular control they can divide by 100.
// I think this is better than requiring multiplication by 100 for the most common usage.
constexpr ControlState BATTERY_INPUT_MAX_VALUE = 100.0;

namespace Core
{
class Device
{
public:
  class Input;
  class Output;

  //
  // Control
  //
  // Control includes inputs and outputs
  //
  class Control  // input or output
  {
  public:
    virtual ~Control() = default;
    virtual std::string GetName() const = 0;
    virtual Input* ToInput() { return nullptr; }
    virtual Output* ToOutput() { return nullptr; }

    // May be overridden to allow multiple valid names.
    // Useful for backwards-compatible configurations when names change.
    virtual bool IsMatchingName(std::string_view name) const;
  };

  // A set of flags we can define to determine whether this control should be
  // read (or written), or ignored/blocked, based on our current game app/window focus.
  // As of now they are only used by inputs but nothing prevents us from implementing them
  // on outputs, and they are not limited to focus, if we came up with new blocking conditions.
  //
  // These flags are per Control, but they will get summed up with other expressions
  // from the same ControlReference. This is because checking them per input/output
  // would have been too complicated, expensive, and ultimately, useless.
  // So they are in order of priority, and some of them are mutually exclusive.
  // Users can use function expressions to customize the focus requirements of a
  // ControlReference, but we manually hardcode them in some Inputs (e.g. mouse) so
  // users don't have to bother in most cases, as it's easy to understand as a concept.
  enum class FocusFlags : u8
  {
    // The input is only passed if we have focus (or the user accepts
    // background input)
    RequireFocus = 0x01,
    // The input is only passed if we have "full" focus, which means the mouse
    // has been locked into the game window. Ignored if mouse locking is off
    RequireFullFocus = 0x02,
    // Some inputs are able to make you lose or gain focus or full focus,
    // for example a mouse click, or the Windows key. When these are pressed
    // and there is a window focus change, ignore them for the time being,
    // as the user didn't want the application to react
    IgnoreOnFocusChanged = 0x04,
    // Forces the input to be passed even if we have no focus,
    // useful for things like battery level. This is not 0 because it needs
    // higher priority over other flags
    IgnoreFocus = 0x80,

    // Even expressions that are fixed should return this
    Default = RequireFocus
  };

  // This should be in ControllerInterface.h but it's here to avoid recursive includes
  enum class InputChannel : u8
  {
    SerialInterface,  // GC Controllers
    Bluetooth,        // WiiMote and other BT devices
    Host,             // Hotkeys (worker thread) and UI (game input config panels, main thread)
    Max
  };

  //
  // Input
  //
  // An input on a device
  //
  class Input : public Control
  {
  public:
    // Things like absolute axes/ absolute mouse position should override this to prevent
    // undesirable behavior in our mapping logic.
    virtual bool IsDetectable() const { return true; }

    virtual FocusFlags GetFocusFlags() const { return FocusFlags::Default; }

    // Implementations should return a value from 0.0 to 1.0 across their normal range.
    // One input should be provided for each "direction". (e.g. 2 for each axis)
    // If possible, negative values may be returned in situations where an opposing input
    // is activated. (e.g. When an underlying axis, X, is currently negative, "Axis X-",
    // will return a positive value and "Axis X+" may return a negative value.)
    // Doing so is solely to allow our input detection logic to better detect false positives.
    // This is necessary when making use of "FullAnalogSurface" as multiple inputs will be seen
    // increasing from 0.0 to 1.0 as a user tries to map just one. The negative values provide
    // a view of the underlying axis. (Negative values are clamped off before they reach
    // expression-parser or controller-emu)
    virtual ControlState GetState() const = 0;

    Input* ToInput() override { return this; }

protected:
    InputChannel GetCurrentInputChannel() const;
  };

  //
  // RelativeInput
  //
  // Helper to generate a relative input from an absolute one.
  // Keeps the last 2 absolute states and returns their difference.
  // It has one state per input channel, as otherwise one SetState() would break
  // GetState() from the other channels. You don't have to use this implementation
  //
  template <typename T = ControlState>
  class RelativeInput : public Input
  {
  public:
    RelativeInput(ControlState scale = 1.0) : m_scale(scale) {}
    void UpdateState(T absolute_state)
    {
      u8 i = u8(GetCurrentInputChannel());
      m_prev_relative_state[i] = m_relative_state[i];
      m_relative_state[i] = m_initialized[i] ? (absolute_state - m_last_absolute_state[i]) : 0.0;
      m_last_absolute_state[i] = absolute_state;
      m_initialized[i] = true;
    }
    void ResetState()
    {
      u8 i = u8(GetCurrentInputChannel());
      m_initialized[i] = false;
      m_relative_state[i] = 0.0;
    }
    ControlState GetState() const override
    {
      InputChannel input_channel = GetCurrentInputChannel();
      u8 i = u8(input_channel);
      // SI updates at twice the video refresh rate of the game, it's very unlikely
      // that games will read both inputs so we average the last two, trade a bit
      // of latency for accuracy and smoothness. Theoretically this should be a per game setting
      if (input_channel == InputChannel::SerialInterface)
        return (m_relative_state[i] + m_prev_relative_state[i]) * 0.5 * m_scale;
      return m_relative_state[i] * m_scale;
    }

  protected:
    T m_last_absolute_state[u8(InputChannel::Max)];
    ControlState m_relative_state[u8(InputChannel::Max)]{};
    ControlState m_prev_relative_state[u8(InputChannel::Max)]{};
    // Not really necessary but it helps to add transparency to the final user,
    // we need a multiplier to have the relative values usable. Can also be used as range
    const ControlState m_scale;
    bool m_initialized[u8(InputChannel::Max)]{};
  };

  //
  // Output
  //
  // An output on a device
  //
  class Output : public Control
  {
  public:
    virtual ~Output() = default;
    virtual void SetState(ControlState state) = 0;
    Output* ToOutput() override { return this; }
  };

  virtual ~Device();

  int GetId() const { return m_id; }
  void SetId(int id) { m_id = id; }
  virtual std::string GetName() const = 0;
  virtual std::string GetSource() const = 0;
  std::string GetQualifiedName() const;
  virtual void UpdateInput() {}

  // May be overridden to implement hotplug removal.
  // Currently handled on a per-backend basis but this could change.
  virtual bool IsValid() const { return true; }

  // (e.g. Xbox 360 controllers have controller number LEDs which should match the ID we use.)
  virtual std::optional<int> GetPreferredId() const;

  const std::vector<Input*>& Inputs() const { return m_inputs; }
  const std::vector<Output*>& Outputs() const { return m_outputs; }

  Input* FindInput(std::string_view name) const;
  Output* FindOutput(std::string_view name) const;

protected:
  void AddInput(Input* const i);
  void AddOutput(Output* const o);

  class FullAnalogSurface final : public Input
  {
  public:
    FullAnalogSurface(Input* low, Input* high) : m_low(*low), m_high(*high) {}
    ControlState GetState() const override;
    std::string GetName() const override;
    bool IsMatchingName(std::string_view name) const override;

  private:
    Input& m_low;
    Input& m_high;
  };

  void AddAnalogInputs(Input* low, Input* high)
  {
    AddInput(low);
    AddInput(high);
    AddInput(new FullAnalogSurface(low, high));
    AddInput(new FullAnalogSurface(high, low));
  }

private:
  int m_id;
  std::vector<Input*> m_inputs;
  std::vector<Output*> m_outputs;
};

//
// DeviceQualifier
//
// Device qualifier used to match devices.
// Currently has ( source, id, name ) properties which match a device
//
class DeviceQualifier
{
public:
  DeviceQualifier() : cid(-1) {}
  DeviceQualifier(std::string source_, const int id_, std::string name_)
      : source(std::move(source_)), cid(id_), name(std::move(name_))
  {
  }
  void FromDevice(const Device* const dev);
  void FromString(const std::string& str);
  std::string ToString() const;

  bool operator==(const DeviceQualifier& devq) const;
  bool operator!=(const DeviceQualifier& devq) const;

  bool operator==(const Device* dev) const;
  bool operator!=(const Device* dev) const;

  std::string source;
  int cid;
  std::string name;
};

class DeviceContainer
{
public:
  Device::Input* FindInput(std::string_view name, const Device* def_dev) const;
  Device::Output* FindOutput(std::string_view name, const Device* def_dev) const;

  std::vector<std::string> GetAllDeviceStrings() const;
  std::string GetDefaultDeviceString() const;
  std::shared_ptr<Device> FindDevice(const DeviceQualifier& devq) const;

  bool HasConnectedDevice(const DeviceQualifier& qualifier) const;

  std::pair<std::shared_ptr<Device>, Device::Input*>
  DetectInput(u32 wait_ms, const std::vector<std::string>& device_strings) const;

protected:
  mutable std::recursive_mutex m_devices_mutex;
  std::vector<std::shared_ptr<Device>> m_devices;
};
}  // namespace Core
}  // namespace ciface
