// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ratio>
#include <memory>
#include <string>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Control/Input.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
using milli_with_remainder = std::chrono::duration<double, std::milli>;

Cursor::Cursor(std::string name_, std::string ui_name_)
    : ReshapableInput(std::move(name_), std::move(ui_name_), GroupType::Cursor),
      m_last_ui_update(Clock::now())
{
  for (auto& named_direction : named_directions)
    AddInput(Translate, named_direction);

  AddInput(Translate, _trans("Hide"));
  AddInput(Translate, _trans("Recenter"));

  AddInput(Translate, _trans("Relative Input Hold"));

  // Default values chosen to reach screen edges in most games including the Wii Menu.

  AddSetting(&m_vertical_offset_setting,
             // i18n: Refers to a positional offset applied to an emulated wiimote.
             {_trans("Vertical Offset"),
              // i18n: The symbol/abbreviation for centimeters.
              _trans("cm")},
             10, -100, 100);

  AddSetting(&m_yaw_setting,
             // i18n: Refers to an amount of rotational movement about the "yaw" axis.
             {_trans("Total Yaw"),
              // i18n: The symbol/abbreviation for degrees (unit of angular measure).
              _trans("°"),
              // i18n: Refers to emulated wii remote movements.
              _trans("Total rotation about the yaw axis.")},
             25, 0, 360);

  AddSetting(&m_pitch_setting,
             // i18n: Refers to an amount of rotational movement about the "pitch" axis.
             {_trans("Total Pitch"),
              // i18n: The symbol/abbreviation for degrees (unit of angular measure).
              _trans("°"),
              // i18n: Refers to emulated wii remote movements.
              _trans("Total rotation about the pitch axis.")},
             20, 0, 360);

  AddSetting(&m_relative_setting, {_trans("Relative Input")}, false);
  const NumericSetting<bool>* edit_condition =
      static_cast<NumericSetting<bool>*>(numeric_settings.back().get());
  AddSetting(&m_relative_absolute_time_setting,
             {_trans("Relative Input Absolute Time"), _trans(""),
              _trans("Enable if you are using a relative input device (e.g. mouse axis, touch "
                     "surface),\nit will make it independent from the emulation speed.")},
             false, false, true, edit_condition);
  AddSetting(&m_autohide_setting, {_trans("Auto-Hide")}, false);
}

Cursor::ReshapeData Cursor::GetReshapableState(bool adjusted) const
{
  const ControlState y = controls[0]->GetState() - controls[1]->GetState();
  const ControlState x = controls[3]->GetState() - controls[2]->GetState();

  // Return raw values
  if (!adjusted)
    return {x, y};

  // Values are clamped later on, the maximum movement between two frames
  // should not be clamped in relative mode
  return Reshape(x, y, 0.0, std::numeric_limits<ControlState>::infinity());
}

ControlState Cursor::GetGateRadiusAtAngle(double ang) const
{
  return SquareStickGate(1.0).GetRadiusAtAngle(ang);
}

// TODO: pass in the state as reference and let wiimote and UI keep their own state.
Cursor::StateData Cursor::GetState(bool is_ui, float absolute_time_elapsed)
{
  const int i = is_ui ? 1 : 0;

  const auto input = GetReshapableState(true);

  double ms_since_update;
  // The UI updates at arbitrary refresh rates which aren't syncronized with devices updates
  // so we need to calculate the time outselves. While if we are in game, we can just use the
  // time of the controller interface channel, which is more reliable.
  if (is_ui)
  {
    const auto now = Clock::now();
    ms_since_update =
        std::chrono::duration_cast<milli_with_remainder>(now - m_last_ui_update).count();
    m_last_ui_update = now;
  }
  else
  {
    ms_since_update = g_controller_interface.GetCurrentRealInputDeltaSeconds() * 1000.0;
  }

  // Relative input (the second check is for Hold):
  if (m_relative_setting.GetValue() ^ controls[6]->GetState<bool>())
  {
    // Recenter:
    if (controls[5]->GetState<bool>())
    {
      m_state[i].x = 0.0;
      m_state[i].y = 0.0;
    }
    else
    {
      // If we are using a mouse axis to drive the cursor (there are reasons to), we want the
      // step to be independent from the emu speed, otherwise it would move at a different speed
      // depending on it.
      // In other words, we want to be indipendent from time, and just use it as an absolute cursor.
      // Of course if the emulation can't keep up with full speed, absolute time won't be accurate.
      // Also the chrono timer as of now is extremely unstable between frames,
      // so it add quite a lot of imprecision.
      const bool use_absolute_time =
          m_relative_absolute_time_setting.GetValue() && absolute_time_elapsed >= 0.f;
      const double step =
          STEP_PER_SEC * (use_absolute_time ? absolute_time_elapsed : (ms_since_update / 1000.0));

      m_state[i].x += input.x * step;
      m_state[i].y += input.y * step;
    }
  }
  // Absolute input:
  else
  {
    m_state[i].x = input.x;
    m_state[i].y = input.y;
  }

  // Clamp between -1 and 1, before auto hide. Clamping after auto hide could make it easier to find
  // when you've lost the cursor but it could also make it more annoying.
  // If we don't do this, we could go over the user specified angles (which can just be increased
  // instead), or lose the cursor over the borders.
  m_state[i].x = std::clamp(m_state[i].x, -1.0, 1.0);
  m_state[i].y = std::clamp(m_state[i].y, -1.0, 1.0);

  StateData result = m_state[i];
  m_prev_state[i] = result;

  const bool autohide = m_autohide_setting.GetValue();

  // Auto-hide timer (ignores Z):
  if (!autohide || std::abs(m_prev_state[i].x - result.x) > AUTO_HIDE_DEADZONE ||
      std::abs(m_prev_state[i].y - result.y) > AUTO_HIDE_DEADZONE)
  {
    m_auto_hide_timer[i] = AUTO_HIDE_MS;
  }
  else if (m_auto_hide_timer[i])
  {
    // Auto hide is based on real world time, doesn't depend on emulation time/speed
    m_auto_hide_timer[i] -= std::min<int>(ms_since_update, m_auto_hide_timer[i]);
  }

  // If auto-hide time is up or hide button is held:
  if (!m_auto_hide_timer[i] || controls[4]->GetState<bool>())
  {
    result.x = std::numeric_limits<ControlState>::quiet_NaN();
    result.y = 0;
  }

  return result;
}

void Cursor::ResetState(bool is_ui)
{
  const int i = is_ui ? 1 : 0;

  m_state[i] = {};
  m_prev_state[i] = {};

  m_auto_hide_timer[i] = AUTO_HIDE_MS;

  if (is_ui)
    m_last_ui_update = Clock::now();
}

ControlState Cursor::GetTotalYaw() const
{
  return m_yaw_setting.GetValue() * MathUtil::TAU / 360;
}

ControlState Cursor::GetTotalPitch() const
{
  return m_pitch_setting.GetValue() * MathUtil::TAU / 360;
}

ControlState Cursor::GetVerticalOffset() const
{
  return m_vertical_offset_setting.GetValue() / 100;
}

bool Cursor::StateData::IsVisible() const
{
  return !std::isnan(x);
}

}  // namespace ControllerEmu
