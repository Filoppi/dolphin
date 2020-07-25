// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/WiimoteEmu/Speaker.h"

#include <memory>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Core/ConfigManager.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

//#define WIIMOTE_SPEAKER_DUMP
#ifdef WIIMOTE_SPEAKER_DUMP
#include <cstdlib>
#include <fstream>
#include "AudioCommon/WaveFile.h"
#include "Common/FileUtil.h"
#endif
//#pragma optimize("", off) //To delete

namespace WiimoteEmu
{
// Yamaha ADPCM decoder code based on The ffmpeg Project (Copyright (s) 2001-2003)

static const s32 yamaha_difflookup[] = {1,  3,  5,  7,  9,  11,  13,  15,
                                        -1, -3, -5, -7, -9, -11, -13, -15};

static const s32 yamaha_indexscale[] = {230, 230, 230, 230, 307, 409, 512, 614,
                                        230, 230, 230, 230, 307, 409, 512, 614};

static double av_clip16(double a)
{
  if ((s32(a) + 32768) & ~65535)
    return (s32(a) >> 31) ^ 32767;
  else
    return a;
}

static s16 adpcm_yamaha_expand_nibble(ADPCMState& s, u8 nibble)
{
  s.predictor += (s.step * yamaha_difflookup[nibble]) / 8.0;
  s.predictor = av_clip16(s.predictor);
  s.step = (s.step * yamaha_indexscale[nibble]) / 256.0;
  s.step = std::clamp(s.step, 127.0, 24576.0);
  return s.predictor;
}

#ifdef WIIMOTE_SPEAKER_DUMP
std::ofstream ofile;
WaveFileWriter wav;

void stopdamnwav()
{
  wav.Stop();
  ofile.close();
}
#endif

void SpeakerLogic::SpeakerData(const u8* data, int length, float speaker_pan)
{
  if (reg_data.sample_rate == 0 || length == 0)
    return;

  // Even if volume is zero or WiimoteEnableSpeaker is off we process samples to maintain proper
  // decoder state

  // TODO consider using static max size instead of new
  std::unique_ptr<s16[]> samples(new s16[length * 2]);

  unsigned int sample_rate_dividend, sample_length;
  u8 volume_divisor;

  if (reg_data.format == SpeakerLogic::DATA_FORMAT_PCM)
  {
    // 8 bit PCM
    for (int i = 0; i < length; ++i)
    {
      samples[i] = ((s16)(s8)data[i]) * 0x100;
    }

    // Following details from http://wiibrew.org/wiki/Wiimote#Speaker
    sample_rate_dividend = 12000000;
    volume_divisor = 0xff;
    sample_length = (unsigned int)length;
  }
  else if (reg_data.format == SpeakerLogic::DATA_FORMAT_ADPCM)
  {
    //\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b 8 Starts with sometimes
    //€€€€€€€€€€€€€€€€€€€€ 128 Ends with sometimes
    // I'm not exactly sure how the real wii mote resets its Yamaha ADPCM decoder state,
    // but every time a new samples batch is started, if playing the same sound, it's exactly the same as the one before,
    // so the decoder should not be maintained between samples.
    // testing NSMBW, the data it sends is always the same again, except this time is is shifted differently every time, there is some padding at the beginning, always different in number and value
    // Unfortunately we aren't aware of a "new sound started/stopped"
    // event. If we open the wii menu while a sound from a game was playing, it will keep playing.
    // On real HW it might have been reset with a timer, or it would naturally reset itself with some more accurate math,
    // or it might have some unknown flag in the data which tells to reset it, or it knew the length of a sound,
    // and with a counter, every time a num of samples have played, it resets itself.
    // There definitely is a "sound index" as otherwise the samples would stop playing when entering the wii menu, so we need to get that and know when a new one has started
    // This just sounds different than from real HW, the easiest way to tell the difference is
    // open the wii home button menu, go to the wii mote settings, and turn down the volume to 1, then change it to 0, it will only make a beeeeep noise, while on the real wii mote it makes a smooth relaxing sound

    // 4 bit Yamaha ADPCM (same as dreamcast)
    for (int i = 0; i < length; ++i)
    {
      samples[i * 2] = adpcm_yamaha_expand_nibble(adpcm_state, data[i] >> 4);
      samples[i * 2 + 1] = adpcm_yamaha_expand_nibble(adpcm_state, data[i] & 0xf);
    }
    //for (int i = length; i > 0; --i)
    //{
    //  samples[(length - i) * 2] = adpcm_yamaha_expand_nibble(adpcm_state, data[i] & 0xf);
    //  samples[(length - i) * 2 + 1] = adpcm_yamaha_expand_nibble(adpcm_state, data[i] >> 4);
    //}
    //for (int n = length; n > 0; n--)
    //{
    //  u8 v = bytestream2_get_byteu(&gb);
    //  *samples++ = adpcm_yamaha_expand_nibble(&c->status, v & 0x0F);
    //  *samples++ = adpcm_yamaha_expand_nibble(&c->status, v >> 4);
    //}

    // Following details from http://wiibrew.org/wiki/Wiimote#Speaker
    sample_rate_dividend = 6000000;
    volume_divisor = 0x7F;
    sample_length = (unsigned int)length * 2;
  }
  else
  {
    ERROR_LOG(IOS_WIIMOTE, "Unknown speaker format %x", reg_data.format);
    return;
  }

  if (reg_data.volume > volume_divisor)
  {
    DEBUG_LOG(IOS_WIIMOTE, "Wiimote volume is higher than suspected maximum!");
    volume_divisor = reg_data.volume;
  }

  if (SConfig::GetInstance().m_WiimoteEnableSpeaker)
  {
    // SetWiimoteSpeakerVolume expects values from 0 to 255.
    // Multiply by 256, floor to int, and clamp to 255 for a uniformly mapped conversion.
    const double volume = float(reg_data.volume) * 256.f / volume_divisor;

    // This used the "Constant Power Pan Law", but it is undesiderable
    // if the pan is 0, and it implied that the loudness of a wiimote speaker
    // is equal to the loudness of your device speakers, which isn't true at all.
    // This way, if the pan is 0, it's like if it is not there.
    // We should play the samples from the wiimote at the native volume they came with,
    // because you can lower their volume from the wii settings, and because they are
    // already extremely low quality, so any additional quality loss isn't welcome.
    speaker_pan = std::clamp(speaker_pan, -1.f, 1.f);
    u32 l_volume = std::min(u32(std::min(1.f - speaker_pan, 1.f) * volume), 255u);
    u32 r_volume = std::min(u32(std::min(1.f + speaker_pan, 1.f) * volume), 255u);

    g_sound_stream->GetMixer()->SetWiimoteSpeakerVolume(m_index, l_volume, r_volume);

    // ADPCM sample rate is thought to be x2 (3000 x2 = 6000).
    const unsigned int sample_rate = sample_rate_dividend / reg_data.sample_rate;
    g_sound_stream->GetMixer()->PushWiimoteSpeakerSamples(m_index, samples.get(), sample_length,
                                                          sample_rate * 2);
  }

#ifdef WIIMOTE_SPEAKER_DUMP
  static int num = 0;

  if (num == 0)
  {
    File::Delete("rmtdump.wav");
    File::Delete("rmtdump.bin");
    atexit(stopdamnwav);
    File::OpenFStream(ofile, "rmtdump.bin", ofile.binary | ofile.out);
    wav.Start("rmtdump.wav", 6000);
  }
  wav.AddMonoSamples(samples.get(), length * 2);
  if (ofile.good())
  {
    for (int i = 0; i < length; i++)
    {
      ofile << data[i];
    }
  }
  num++;
#endif
}

void SpeakerLogic::Reset()
{
  reg_data = {};

  // Yamaha ADPCM state initialize
  adpcm_state.predictor = 0; //To make sure it's called (what about when the speaker is muted or disabled)
  adpcm_state.step = 127;
}

void SpeakerLogic::DoState(PointerWrap& p)
{
  p.Do(adpcm_state);
  p.Do(reg_data);
}

int SpeakerLogic::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  if (I2C_ADDR != slave_addr)
    return 0;

  return RawRead(&reg_data, addr, count, data_out);
}

int SpeakerLogic::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  if (I2C_ADDR != slave_addr)
    return 0;

  if (0x00 == addr)
  {
    SpeakerData(data_in, count, m_speaker_pan_setting.GetValue() / 100);
    return count;
  }
  else
  {
    // TODO: Does writing immediately change the decoder config even when active
    // or does a write to 0x08 activate the new configuration or something?
    return RawWrite(&reg_data, addr, count, data_in);
  }
}

}  // namespace WiimoteEmu
