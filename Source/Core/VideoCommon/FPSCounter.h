// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <fstream>

#include "Common/CommonTypes.h"

class FPSCounter
{
public:
  FPSCounter();
  ~FPSCounter();

  // Called when a frame is rendered (updated every second).
  void Update();

  float GetFPS() const { return m_fps; }

private:
  void SetPaused(bool paused);

  u64 m_last_time = 0;
  u64 m_time_since_update = 0;
  u64 m_last_time_pause = 0;
  u32 m_frame_counter = 0;
  int m_on_state_changed_handle = -1;
  float m_fps = 0.f;
  std::ofstream m_bench_file;

  void LogRenderTimeToFile(u64 val);
};
