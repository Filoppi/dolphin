// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Control/Input.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
Cursor::Cursor(std::string name, std::string ui_name)
    : ReshapableInput(std::move(name), std::move(ui_name), GroupType::Cursor), m_last_update{
                                                                                   Clock::now(),
                                                                                   Clock::now()}
{
  for (auto& named_direction : named_directions)
    AddInput(Translate, named_direction);

  AddInput(Translate, _trans("Hide"));
  AddInput(Translate, _trans("Recenter"));

  AddInput(Translate, _trans("Relative Input Hold"));

  // Default values are optimized for "Super Mario Galaxy 2".
  // This seems to be acceptable for a good number of games.

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
             14.65, 0, 360);

  AddSetting(&m_pitch_setting,
             // i18n: Refers to an amount of rotational movement about the "pitch" axis.
             {_trans("Total Pitch"),
              // i18n: The symbol/abbreviation for degrees (unit of angular measure).
              _trans("°"),
              // i18n: Refers to emulated wii remote movements.
              _trans("Total rotation about the pitch axis.")},
             13.25, 0, 360);

  AddSetting(&m_relative_setting, {_trans("Relative Input")}, false);
  // TODO: link to the setting above and grey it out if it's off
  AddSetting(&m_relative_absolute_time_setting, {_trans("Relative Input Absolute Time")}, false);
  AddSetting(&m_autohide_setting, {_trans("Auto-Hide")}, false);
  numeric_settings.back().get()->GetInputReference().default_range = AUTO_HIDE_MS / 1000.0;
  numeric_settings.back().get()->GetInputReference().range = AUTO_HIDE_MS / 1000.0;
}

Cursor::ReshapeData Cursor::GetReshapableState(bool adjusted)
{
  const ControlState y = controls[0]->GetState() - controls[1]->GetState();
  const ControlState x = controls[3]->GetState() - controls[2]->GetState();

  // Return raw values
  if (!adjusted)
    return {x, y};

  return Reshape(x, y, 0.0, std::numeric_limits<ControlState>::infinity());
}

ControlState Cursor::GetGateRadiusAtAngle(double ang) const
{
  return SquareStickGate(1.0).GetRadiusAtAngle(ang);
}

Cursor::StateData Cursor::GetState(float absolute_time_elapsed, bool is_ui)
{
  const auto input = GetReshapableState(true);

  int i = is_ui ? 1 : 0;

  // Kill this after state is moved into wiimote rather than this class.
  const auto now = Clock::now();
  const auto ms_since_update =
      std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_update[i]).count() /
      1000.0;
  m_last_update[i] = now;

  // Relative input:
  if (m_relative_setting.GetValue() ^ (controls[6]->GetState<bool>()))
  {
    // Recenter:
    if (controls[5]->GetState<bool>())
    {
      m_state[i].x = 0.0;
      m_state[i].y = 0.0;
    }
    else
    {
      // If we are using a mouse axis to drive the cursor (there are reasons to), we don't want the
      // step to be independent from the game speed, otherwise it would move at a different speed
      // depending on the game speed. I know it sounds like it doesn't make sense but it does.
      // In other words, we want to be indipendent from time, and just use it as an absolute cursor
      bool use_absolute_time = m_relative_absolute_time_setting.GetValue() && !is_ui;
      const double step =
          STEP_PER_SEC * (use_absolute_time ? absolute_time_elapsed : (ms_since_update / 1000.0));

      m_state[i].x = std::clamp(m_state[i].x + input.x * step, -1.0, 1.0);
      m_state[i].y = std::clamp(m_state[i].y + input.y * step, -1.0, 1.0);
    }
  }
  // Absolute input:
  else
  {
    m_state[i].x = input.x;
    m_state[i].y = input.y;
  }

  StateData result = m_state[i];

  const bool autohide = m_autohide_setting.GetValue();

  // Auto-hide timer (ignores Z):
  if (!autohide || std::abs(m_prev_result[i].x - result.x) > AUTO_HIDE_DEADZONE ||
      std::abs(m_prev_result[i].y - result.y) > AUTO_HIDE_DEADZONE)
  {
    m_auto_hide_timer[i] = AUTO_HIDE_MS;
  }
  else if (m_auto_hide_timer[i])
  {
    m_auto_hide_timer[i] -= std::min<int>(ms_since_update, m_auto_hide_timer[i]);
  }

  m_prev_result[i] = result;

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
  int i = is_ui ? 1 : 0;

  m_state[i] = {};
  m_prev_result[i] = {};

  m_auto_hide_timer[i] = AUTO_HIDE_MS;

  m_last_update[i] = Clock::now();
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
