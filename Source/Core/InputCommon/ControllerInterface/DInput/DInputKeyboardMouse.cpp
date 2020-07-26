// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>

#include "Core/Core.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DInput/DInput.h"
#include "InputCommon/ControllerInterface/DInput/DInputKeyboardMouse.h"

// Just a default value which works well at default at 800dpi.
// Users can multiply it anyway (lower is more sensitive)
#define MOUSE_AXIS_SENSITIVITY 17

// If input hasn't been received for this many ms, mouse input will be skipped
// otherwise it is just some crazy value
#define DROP_INPUT_TIME 250

namespace ciface::DInput
{
static const struct
{
  const BYTE code;
  const char* const name;
} named_keys[] = {
#include "InputCommon/ControllerInterface/DInput/NamedKeys.h"  // NOLINT
};

// Prevent duplicate keyboard/mouse devices.
static bool s_keyboard_mouse_exists = false;

void InitKeyboardMouse(IDirectInput8* const idi8, HWND hwnd)
{
  if (s_keyboard_mouse_exists)
    return;

  // Mouse and keyboard are a combined device, to allow shift+click and stuff
  // if that's dumb, I will make a VirtualDevice class that just uses ranges of inputs/outputs from
  // other devices
  // so there can be a separated Keyboard and Mouse, as well as combined KeyboardMouse.

  LPDIRECTINPUTDEVICE8 kb_device = nullptr;
  LPDIRECTINPUTDEVICE8 mo_device = nullptr;

  // TODO: Make sure the SetCooperativeLevel hwnd is valid when we release the device
  // These are "virtual" system devices, so they are always there even if we have no physical
  // mouse and keyboard plugged into the computer
  if (SUCCEEDED(idi8->CreateDevice(GUID_SysKeyboard, &kb_device, nullptr)) &&
      SUCCEEDED(kb_device->SetDataFormat(&c_dfDIKeyboard)) &&
      SUCCEEDED(kb_device->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)) &&
      SUCCEEDED(idi8->CreateDevice(GUID_SysMouse, &mo_device, nullptr)) &&
      SUCCEEDED(mo_device->SetDataFormat(&c_dfDIMouse2)) &&
      SUCCEEDED(mo_device->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)))
  {
    // The device is recreated with a new window handle when we change main window
    g_controller_interface.AddDevice(std::make_shared<KeyboardMouse>(kb_device, mo_device, hwnd));
    return;
  }

  if (kb_device)
    kb_device->Release();
  if (mo_device)
    mo_device->Release();
}

KeyboardMouse::~KeyboardMouse()
{
  s_keyboard_mouse_exists = false;

  // kb
  m_kb_device->Unacquire();
  m_kb_device->Release();
  // mouse
  m_mo_device->Unacquire();
  m_mo_device->Release();
}

KeyboardMouse::KeyboardMouse(const LPDIRECTINPUTDEVICE8 kb_device,
                             const LPDIRECTINPUTDEVICE8 mo_device, HWND hwnd)
    : m_kb_device(kb_device), m_mo_device(mo_device), m_hwnd(hwnd), m_last_update(GetTickCount()),
      m_state_in()
{
  s_keyboard_mouse_exists = true;

  m_kb_device->Acquire();
  m_mo_device->Acquire();

  // KEYBOARD
  // add keys
  for (u8 i = 0; i < sizeof(named_keys) / sizeof(*named_keys); ++i)
    AddInput(new Key(i, m_state_in.keyboard[named_keys[i].code]));

  // MOUSE
  DIDEVCAPS mouse_caps = {};
  mouse_caps.dwSize = sizeof(mouse_caps);
  m_mo_device->GetCapabilities(&mouse_caps);
  // mouse buttons
  mouse_caps.dwButtons = std::min(mouse_caps.dwButtons, (DWORD)sizeof(m_state_in.mouse.rgbButtons));
  for (u8 i = 0; i < mouse_caps.dwButtons; ++i)
    AddInput(new Button(i, m_state_in.mouse.rgbButtons[i]));
  // mouse axes
  mouse_caps.dwAxes = std::min(mouse_caps.dwAxes, (DWORD)3);
  for (unsigned int i = 0; i < mouse_caps.dwAxes; ++i)
  {
    const LONG& axis = (&m_state_in.mouse.lX)[i];
    const LONG& prev_axis = (&m_state_in.previous_mouse.lX)[i];

    // each axis gets a negative and a positive input instance associated with it
    AddInput(new Axis(i, axis, prev_axis, (2 == i) ? -1 : -MOUSE_AXIS_SENSITIVITY));
    AddInput(new Axis(i, axis, prev_axis, (2 == i) ? 1 : MOUSE_AXIS_SENSITIVITY));
  }
  // cursor, with a hax for-loop
  for (unsigned int i = 0; i <= 3; ++i)
    AddInput(new Cursor(!!(i & 2), (&m_state_in.cursor.x)[i / 2], !!(i & 1)));
}

void KeyboardMouse::UpdateCursorInput()
{
  POINT point = {};
  GetCursorPos(&point);

  // Get the cursor position relative to the upper left corner of the current window
  // (separate or render to main)
  ScreenToClient(m_hwnd, &point);

  // Get the size of the current window. (In my case Rect.top and Rect.left was zero.)
  RECT rect;
  GetClientRect(m_hwnd, &rect);

  // Width and height are the size of the rendering window.
  const auto win_width = rect.right - rect.left;
  const auto win_height = rect.bottom - rect.top;

  const auto window_scale = g_controller_interface.GetWindowInputScale();

  // Convert the cursor position to a range from -1 to 1.
  m_state_in.cursor.x = (ControlState(point.x) / win_width * 2 - 1) * window_scale.x;
  m_state_in.cursor.y = (ControlState(point.y) / win_height * 2 - 1) * window_scale.y;
}

void KeyboardMouse::UpdateInput()
{  
  HRESULT kb_hr = m_kb_device->GetDeviceState(sizeof(m_state_in.keyboard), &m_state_in.keyboard);
  if (DIERR_INPUTLOST == kb_hr || DIERR_NOTACQUIRED == kb_hr)
  {
    if (SUCCEEDED(m_kb_device->Acquire()))
      kb_hr = m_kb_device->GetDeviceState(sizeof(m_state_in.keyboard), &m_state_in.keyboard);
  }

  UpdateCursorInput();

  // Only update the mouse axis twice per game video frame, or always if a game is not running.
  // This hack is needed because we use the mouse as a fake analog stick axis.
  // Given that the mouse works with dots (position), all we can get is the number of dots
  // it moved by since the last update, but updates to this device are called constantly from
  // different parts of Dolphin, not the game itself, so most of the small changes would be lost
  // if we always just returned the very last offset.
  // Another problem in simulating the mouse as an axis is that its speed varies extremely between
  // frames, differently from an analog stick, this is why we blend the last 2 values together.
  // If for some reason we needed this axis to be sub-frame accurate while a game is running
  // (e.g. to use it with the emulated wiimote, which runs at a higher tick) we could add yet
  // another input for that exclusively (e.g. WiiMoteRefreshRate Axis+ vs GameRefreshRate Axis+).
  // Note that we also lerp the Z (wheel) axis
  if (g_controller_interface.ShouldUpdateFakeRelativeAxes() ||
      ::Core::GetState() != ::Core::State::Running)
  {
    DIMOUSESTATE2 tmp_mouse;

    // if mouse position hasn't been updated in a short while, skip a dev state
    DWORD cur_time = GetTickCount();

    // TODO: this just sounds wrong, whatever value we get, it should be handled
    if (cur_time - m_last_update > DROP_INPUT_TIME)
    {
      // set axes to zero
      m_state_in.mouse = {};
      m_state_in.previous_mouse = m_state_in.mouse;
      // skip this input state (by calling this, the next call will return 0)
      m_mo_device->GetDeviceState(sizeof(tmp_mouse), &tmp_mouse);
    }

    m_last_update = cur_time;

    // The mouse location this returns is the difference from the last time it was called
    HRESULT mo_hr = m_mo_device->GetDeviceState(sizeof(tmp_mouse), &tmp_mouse);
    if (DIERR_INPUTLOST == mo_hr || DIERR_NOTACQUIRED == mo_hr)
    {
      if (SUCCEEDED(m_mo_device->Acquire()))
        mo_hr = m_mo_device->GetDeviceState(sizeof(tmp_mouse), &tmp_mouse);
    }
    if (SUCCEEDED(mo_hr))
    {
      m_state_in.previous_mouse = m_state_in.mouse;
      m_state_in.mouse = tmp_mouse;
    }
  }
}

std::string KeyboardMouse::GetName() const
{
  return "Keyboard Mouse";
}

std::string KeyboardMouse::GetSource() const
{
  return DINPUT_SOURCE_NAME;
}

// names
std::string KeyboardMouse::Key::GetName() const
{
  return named_keys[m_index].name;
}

std::string KeyboardMouse::Button::GetName() const
{
  return std::string("Click ") + char('0' + m_index);
}

std::string KeyboardMouse::Axis::GetName() const
{
  static char tmpstr[] = "Axis ..";
  tmpstr[5] = (char)('X' + m_index);
  tmpstr[6] = (m_range < 0 ? '-' : '+');
  return tmpstr;
}

std::string KeyboardMouse::Cursor::GetName() const
{
  static char tmpstr[] = "Cursor ..";
  tmpstr[7] = (char)('X' + m_index);
  tmpstr[8] = (m_positive ? '+' : '-');
  return tmpstr;
}

// get/set state
ControlState KeyboardMouse::Key::GetState() const
{
  return (m_key != 0);
}

ControlState KeyboardMouse::Button::GetState() const
{
  return (m_button != 0);
}

ControlState KeyboardMouse::Axis::GetState() const
{
  // Blend between the last two values (see comments above to understand)
  return (ControlState(m_axis) + ControlState(m_prev_axis)) * 0.5 / m_range;
}

ControlState KeyboardMouse::Cursor::GetState() const
{
  return m_axis * (m_positive ? 1.0 : -1.0);
}
}  // namespace ciface::DInput
