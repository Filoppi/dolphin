// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"

namespace ciface
{
// Assumed to be u8
enum class InputChannel : u8
{
  SerialInterface,  // GC Controllers
  Bluetooth,        // WiiMote and other BT devices
  Host,             // Hotkeys (worker thread) and UI (game input config panels, main thread)
  Max
};
}  // namespace ciface
