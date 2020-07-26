// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cubeb/cubeb.h>

#include <algorithm>

#include "AudioCommon/CubebStream.h"
#include "AudioCommon/CubebUtils.h"
#include "AudioCommon/AudioCommon.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"

long CubebStream::DataCallback(cubeb_stream* stream, void* user_data, const void* /*input_buffer*/,
                               void* output_buffer, long num_frames)
{
  auto* self = static_cast<CubebStream*>(user_data);

  if (self->m_stereo)
    self->m_mixer->Mix(static_cast<short*>(output_buffer), num_frames);
  else
    self->m_mixer->MixSurround(static_cast<float*>(output_buffer), num_frames);

  return num_frames;
}

void CubebStream::StateCallback(cubeb_stream* stream, void* user_data, cubeb_state state)
{
}

bool CubebStream::Init()
{
  m_ctx = CubebUtils::GetContext();
  return m_ctx != nullptr;
}

bool CubebStream::SetRunning(bool running)
{
  assert(running != m_running);

  m_should_restart = false;

  if (running)
  {
    m_mixer->UpdateSettings(SConfig::GetInstance().bUseOSMixerSampleRate ?
                                AudioCommon::GetOSMixerSampleRate() :
                                AudioCommon::GetDefaultSampleRate());

    m_stereo = !SConfig::GetInstance().ShouldUseDPL2Decoder();

    cubeb_stream_params params;
    params.rate = m_mixer->GetSampleRate();
    if (m_stereo)
    {
      params.channels = 2;
      params.format = CUBEB_SAMPLE_S16NE;
      params.layout = CUBEB_LAYOUT_STEREO;
    }
    else
    {
      params.channels = 6;
      params.format = CUBEB_SAMPLE_FLOAT32NE;
      params.layout = CUBEB_LAYOUT_3F2_LFE;
    }

    // In samples
    // Max supported by cubeb is 96000 and min is 1 (in samples)
    u32 minimum_latency = 0;
    if (cubeb_get_min_latency(m_ctx.get(), &params, &minimum_latency) != CUBEB_OK)
      ERROR_LOG(AUDIO, "Error getting minimum latency");

    u32 final_latency;

    uint32_t target_latency = AudioCommon::GetUserTargetLatency() / 1000.0 * params.rate;
#ifdef _WIN32
    // WASAPI supports up to 5000ms but let's clamp to 500ms
    uint32_t max_latency = 500 / 1000.0 * params.rate;
    // This doesn't actually seem to work, latency is ignored on Window 10
    final_latency = std::clamp(target_latency, minimum_latency, max_latency);
#else
    final_latency = std::clamp(target_latency, minimum_latency, 96000);
#endif
    INFO_LOG(AUDIO, "Latency: %u frames", final_latency);

    // It's very hard for cubeb to fail starting so we don't trigger a restart request
    if (cubeb_stream_init(m_ctx.get(), &m_stream, "Dolphin Audio Output", nullptr, nullptr, nullptr,
                          &params, final_latency, DataCallback, StateCallback, this) == CUBEB_OK)
    {
      if (cubeb_stream_start(m_stream) == CUBEB_OK)
      {
        m_running = true;
        return true;
      }
    }
    return false;
  }
  else
  {
    int result = cubeb_stream_stop(m_stream);
    cubeb_stream_destroy(m_stream);
    if (result == CUBEB_OK)
      m_running = false;
    else
      ERROR_LOG(AUDIO, "Cubeb failed to stop. Dolphin might crash");
    // Not sure how to proceed here. Destroying cubeb can't fail but stopping it can?
    // Does destroy imply stopping? Probably, but is it safe?
    return result == CUBEB_OK;
  }
}

CubebStream::~CubebStream()
{
  if (m_running)
    SetRunning(false);
  m_ctx.reset();
}

void CubebStream::Update()
{
  // TODO: move this out of the game (main) thread, restarting WASAPI should not lock the game
  // thread, but as of now there isn't any other constant and safe access point to restart

  // If the sound loop failed for some reason, re-initialize ASAPI to resume playback
  if (m_should_restart)
  {
    m_should_restart = false;
    if (m_running)
    {
      // We need to pass through AudioCommon as it has a mutex and
      // to make sure s_sound_stream_running is updated
      if (AudioCommon::SetSoundStreamRunning(false, false))
      {
        // m_should_restart is triggered when the device is currently
        // invalidated, and it will stay for a while, so this new call
        // to SetRunning(true) might fail, but if it fails some
        // specific reasons, it will set m_should_restart true again.
        // A Sleep(10) call also seemed to fix the problem but it's hacky.
        AudioCommon::SetSoundStreamRunning(true, false);
      }
    }
    else
    {
      AudioCommon::SetSoundStreamRunning(true, false);
    }
  }
}

void CubebStream::SetVolume(int volume)
{
  cubeb_stream_set_volume(m_stream, volume / 100.0f);
}
