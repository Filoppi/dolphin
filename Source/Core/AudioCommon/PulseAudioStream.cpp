// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstring>

#include "AudioCommon/PulseAudioStream.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"

namespace
{
const size_t BUFFER_SAMPLES = 512;  // ~10 ms - needs to be at least 240 for surround
}

bool PulseAudio::Init()
{
  m_stereo = !SConfig::GetInstance().ShouldUseDPL2Decoder();
  m_channels = m_stereo ? 2 : 6;  // will tell PA we use a Stereo or 5.0 channel setup

  NOTICE_LOG_FMT(AUDIO, "PulseAudio backend using {} channels", m_channels);

  if (PulseInit())
  {
    m_run_thread.Set();
    m_thread = std::thread(&PulseAudio::SoundLoop, this);
    return true;
  }
  else
  {
    PulseShutdown();
  }
  return false;
}

bool PulseAudio::SetRunning(bool running)
{
  // Differently from other backends, we don't start or stop the stream here,
  // we just play mute/zero samples if we are not running
  m_running = running;
  return true;
}

PulseAudio::~PulseAudio()
{
  m_run_thread.Clear();
  m_thread.join();
}

// Called on audio thread
void PulseAudio::SoundLoop()
{
  Common::SetCurrentThreadName("Audio thread - Pulse");

  while (m_run_thread.IsSet() && m_pa_connected == 1 && m_pa_error >= 0)
    m_pa_error = pa_mainloop_iterate(m_pa_ml, 1, nullptr);

  if (m_pa_error < 0)
    ERROR_LOG_FMT(AUDIO, "PulseAudio error: {}", pa_strerror(m_pa_error));

  PulseShutdown();
}

bool PulseAudio::PulseInit()
{
  m_pa_error = 0;
  m_pa_connected = 0;

  // create PulseAudio main loop and context
  // also register the async state callback which is called when the connection to the pa server has
  // changed
  m_pa_ml = pa_mainloop_new();
  m_pa_mlapi = pa_mainloop_get_api(m_pa_ml);
  m_pa_ctx = pa_context_new(m_pa_mlapi, "dolphin-emu");
  m_pa_error = pa_context_connect(m_pa_ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
  pa_context_set_state_callback(m_pa_ctx, StateCallback, this);

  // wait until we're connected to the PulseAudio server
  while (m_pa_connected == 0 && m_pa_error >= 0)
    m_pa_error = pa_mainloop_iterate(m_pa_ml, 1, nullptr);

  if (m_pa_connected == 2 || m_pa_error < 0)
  {
    ERROR_LOG_FMT(AUDIO, "PulseAudio failed to initialize: {}", pa_strerror(m_pa_error));
    return false;
  }

  // create a new audio stream with our sample format
  // also connect the callbacks for this stream
  pa_sample_spec ss;
  pa_channel_map channel_map;
  pa_channel_map* channel_map_p = nullptr;  // auto channel map
  if (m_stereo)
  {
    ss.format = PA_SAMPLE_S16LE;
    m_bytespersample = sizeof(s16);
  }
  else
  {
    // surround is remixed in floats, use a float PA buffer to save another conversion
    ss.format = PA_SAMPLE_FLOAT32NE;
    m_bytespersample = sizeof(float);

    channel_map_p = &channel_map;  // explicit channel map:
    channel_map.channels = 6;
    channel_map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
    channel_map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
    channel_map.map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
    channel_map.map[3] = PA_CHANNEL_POSITION_LFE;
    channel_map.map[4] = PA_CHANNEL_POSITION_REAR_LEFT;
    channel_map.map[5] = PA_CHANNEL_POSITION_REAR_RIGHT;
  }
  ss.channels = m_channels;
  ss.rate = m_mixer->GetSampleRate();
  assert(pa_sample_spec_valid(&ss));
  m_pa_s = pa_stream_new(m_pa_ctx, "Playback", &ss, channel_map_p);
  pa_stream_set_write_callback(m_pa_s, WriteCallback, this);
  pa_stream_set_underflow_callback(m_pa_s, UnderflowCallback, this);

  // connect this audio stream to the default audio playback
  // limit buffersize to reduce latency
  m_pa_ba.fragsize = -1;
  m_pa_ba.maxlength = -1;  // max buffer, so also max latency
  m_pa_ba.minreq = -1;     // don't read every byte, try to group them _a bit_
  m_pa_ba.prebuf = -1;     // start as early as possible
  m_pa_ba.tlength =
      BUFFER_SAMPLES * m_channels *
      m_bytespersample;  // designed latency, only change this flag for low latency output
  // TODO: review this, audio stretching won't work correctly if latency is dynamic
  pa_stream_flags flags = pa_stream_flags(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY |
                                          PA_STREAM_AUTO_TIMING_UPDATE);
  m_pa_error = pa_stream_connect_playback(m_pa_s, nullptr, &m_pa_ba, flags, nullptr, nullptr);
  if (m_pa_error < 0)
  {
    // Theoretically PulseAudio should not fail based on the number of channels (as it just remixes
    // anyway), but we never know so we fallback to stereo
    if (!m_stereo)
    {
      ERROR_LOG(AUDIO, "PulseAudio failed to initialize (6.0, falling back to 2.0): %s",
                pa_strerror(m_pa_error));
      m_stereo = true;

      pa_stream_disconnect(m_pa_s);
      pa_stream_unref(m_pa_s);

      m_channels = 2;
      ss.format = PA_SAMPLE_S16LE;
      m_bytespersample = sizeof(s16);
      ss.channels = m_channels;
      m_pa_ba.tlength = BUFFER_SAMPLES * m_channels * m_bytespersample;
      channel_map_p = nullptr;
      assert(pa_sample_spec_valid(&ss));

      m_pa_s = pa_stream_new(m_pa_ctx, "Playback", &ss, channel_map_p);
      pa_stream_set_write_callback(m_pa_s, WriteCallback, this);
      pa_stream_set_underflow_callback(m_pa_s, UnderflowCallback, this);

      m_pa_error = pa_stream_connect_playback(m_pa_s, nullptr, &m_pa_ba, flags, nullptr, nullptr);
    }

    if (m_pa_error < 0)
    {
      ERROR_LOG_FMT(AUDIO, "PulseAudio failed to initialize (2.0): {}", pa_strerror(m_pa_error));
      return false;
    }
  }

  INFO_LOG_FMT(AUDIO, "PulseAudio successfully initialized");
  return true;
}

void PulseAudio::PulseShutdown()
{
  if (m_pa_s)
  {
    pa_stream_disconnect(m_pa_s);
    pa_stream_unref(m_pa_s);
    m_pa_s = nullptr;
  }
  pa_context_disconnect(m_pa_ctx);
  pa_context_unref(m_pa_ctx);
  pa_mainloop_free(m_pa_ml);
  m_pa_ml = nullptr;
  m_pa_mlapi = nullptr;
  m_pa_ctx = nullptr;
}

AudioCommon::SurroundState PulseAudio::GetSurroundState() const
{
  if (m_run_thread.IsSet() && m_pa_connected == 1 && m_pa_error >= 0)
  {
    if (!m_stereo)
    {
      return AudioCommon::SurroundState::Enabled;
    }
    if (SConfig::GetInstance().ShouldUseDPL2Decoder())
    {
      return AudioCommon::SurroundState::Failed;
    }
  }
  return SConfig::GetInstance().ShouldUseDPL2Decoder() ?
             AudioCommon::SurroundState::EnabledNotRunning :
             AudioCommon::SurroundState::Disabled;
}

void PulseAudio::StateCallback(pa_context* c)
{
  pa_context_state_t state = pa_context_get_state(c);
  switch (state)
  {
  case PA_CONTEXT_FAILED:
  case PA_CONTEXT_TERMINATED:
    m_pa_connected = 2;
    break;
  case PA_CONTEXT_READY:
    m_pa_connected = 1;
    break;
  default:
    break;
  }
}

// On underflow, increase PulseAudio latency in ~10ms steps
void PulseAudio::UnderflowCallback(pa_stream* s)
{
  m_pa_ba.tlength += BUFFER_SAMPLES * m_channels * m_bytespersample;
  pa_operation* op = pa_stream_set_buffer_attr(s, &m_pa_ba, nullptr, nullptr);
  pa_operation_unref(op);

  WARN_LOG_FMT(AUDIO, "PulseAudio underflow, new latency: {} bytes", m_pa_ba.tlength);
}

void PulseAudio::WriteCallback(pa_stream* s, size_t length)
{
  int bytes_per_frame = m_channels * m_bytespersample;
  int frames = (length / bytes_per_frame);
  size_t trunc_length = frames * bytes_per_frame;

  // fetch dst buffer directly from PulseAudio, so no memcpy is needed
  void* buffer;
  m_pa_error = pa_stream_begin_write(s, &buffer, &trunc_length);

  if (!buffer || m_pa_error < 0)
    return;  // error will be printed from main loop

  if (m_running)
  {
    if (m_stereo)
    {
      // use the raw s16 stereo mix
      m_mixer->Mix((s16*)buffer, frames);
    }
    else
    {
      // Extract dpl2/5.1 Surround
      m_mixer->MixSurround((float*)buffer, frames);
    }
  }

  m_pa_error = pa_stream_write(s, buffer, trunc_length, nullptr, 0, PA_SEEK_RELATIVE);
}

// Callbacks that forward to internal methods (required because PulseAudio is a C API)

void PulseAudio::StateCallback(pa_context* c, void* userdata)
{
  PulseAudio* p = (PulseAudio*)userdata;
  p->StateCallback(c);
}

void PulseAudio::UnderflowCallback(pa_stream* s, void* userdata)
{
  PulseAudio* p = (PulseAudio*)userdata;
  p->UnderflowCallback(s);
}

void PulseAudio::WriteCallback(pa_stream* s, size_t length, void* userdata)
{
  PulseAudio* p = (PulseAudio*)userdata;
  p->WriteCallback(s, length);
}
