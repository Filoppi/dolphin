// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <windows.h>
#include <vector>

#include "Common/Matrix.h"
#include "InputCommon/ControllerInterface/DInput/DInput8.h"
#include "InputCommon/ControllerInterface/Device.h"

namespace ciface::DInput
{
void InitKeyboardMouse(IDirectInput8* const idi8, HWND hwnd);

class KeyboardMouse : public Core::Device
{
private:
  struct State
  {
    BYTE keyboard[256];
    DIMOUSESTATE2 mouse;
    Common::TVec2<ControlState> cursor;
  };

  // Keyboard key
  class Key : public Input
  {
  public:
    Key(u8 index, const BYTE& key) : m_key(key), m_index(index) {}
    std::string GetName() const override;
    ControlState GetState() const override;

  private:
    const BYTE& m_key;
    const u8 m_index;
  };

  // Mouse button
  class Button : public Input
  {
  public:
    Button(u8 index, const BYTE& button) : m_button(button), m_index(index) {}
    std::string GetName() const override;
    ControlState GetState() const override;
    FocusFlags GetFocusFlags() const override
    {
      return FocusFlags(u8(FocusFlags::RequireFocus) | u8(FocusFlags::RequireFullFocus) |
                        u8(FocusFlags::IgnoreOnFocusChanged));
    }

  private:
    const BYTE& m_button;
    const u8 m_index;
  };

  // Mouse movement offset axis. Includes mouse wheel
  class Axis : public RelativeInput<LONG>
  {
  public:
    Axis(ControlState range, u8 index) : RelativeInput(range), m_index(index) {}
    std::string GetName() const override;
    FocusFlags GetFocusFlags() const override
    {
      return FocusFlags(u8(FocusFlags::RequireFocus) | u8(FocusFlags::RequireFullFocus));
    }

  private:
    const u8 m_index;
  };

  // Mouse from window center
  class Cursor : public Input
  {
  public:
    Cursor(u8 index, const ControlState& axis, const bool positive)
        : m_axis(axis), m_index(index), m_positive(positive)
    {
    }
    std::string GetName() const override;
    ControlState GetState() const override;
    bool IsDetectable() const override { return false; }
    FocusFlags GetFocusFlags() const override
    {
      return FocusFlags((u8(FocusFlags::RequireFocus) | u8(FocusFlags::RequireFullFocus)));
    }

  private:
    const ControlState& m_axis;
    const u8 m_index;
    const bool m_positive;
  };

public:
  void UpdateInput() override;

  KeyboardMouse(const LPDIRECTINPUTDEVICE8 kb_device, const LPDIRECTINPUTDEVICE8 mo_device,
                HWND hwnd);
  ~KeyboardMouse();

  std::string GetName() const override;
  std::string GetSource() const override;

private:
  void UpdateCursorInput();

  const LPDIRECTINPUTDEVICE8 m_kb_device;
  const LPDIRECTINPUTDEVICE8 m_mo_device;

  const HWND m_hwnd;

  std::vector<Axis*> m_mouse_axes;

  DWORD m_last_update;
  State m_state_in;
};
}  // namespace ciface::DInput
