// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerEmu/ControlGroup/IMUAccelerometer.h"

#include <memory>

#include "Common/Common.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Control/Input.h"

namespace ControllerEmu
{
IMUAccelerometer::IMUAccelerometer(std::string name_, std::string ui_name_)
    : ControlGroup(std::move(name_), std::move(ui_name_), GroupType::IMUAccelerometer)
{
  AddInput(Translate, _trans("Up"));
  AddInput(Translate, _trans("Down"));
  AddInput(Translate, _trans("Left"));
  AddInput(Translate, _trans("Right"));
  AddInput(Translate, _trans("Forward"));
  AddInput(Translate, _trans("Backward"));
}

bool IMUAccelerometer::AreInputsBound() const
{
  return controls[0]->control_ref->BoundCount() && controls[1]->control_ref->BoundCount() &&
         controls[2]->control_ref->BoundCount() && controls[3]->control_ref->BoundCount() &&
         controls[4]->control_ref->BoundCount() && controls[5]->control_ref->BoundCount();
}

std::optional<IMUAccelerometer::StateData> IMUAccelerometer::GetState() const
{
  if (!AreInputsBound())
    return std::nullopt;

  StateData state;
  state.x = (controls[2]->GetState() - controls[3]->GetState());
  state.y = (controls[5]->GetState() - controls[4]->GetState());
  state.z = (controls[0]->GetState() - controls[1]->GetState());
  return state;
}

}  // namespace ControllerEmu
