// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "AudioCommon/WaveFile.h"

#include <string>

#include "Common/CommonTypes.h"
#include "Common/File.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/ConfigManager.h"

constexpr size_t WaveFileWriter::BUFFER_SIZE;

WaveFileWriter::WaveFileWriter()
{
}

WaveFileWriter::~WaveFileWriter()
{
  Stop();
}

bool WaveFileWriter::Start(const std::string& file_name, u32 sample_rate)
{
  // Ask to delete file
  if (File::Exists(file_name))
  {
    if (SConfig::GetInstance().m_DumpAudioSilent ||
        AskYesNoFmtT("Delete the existing file '{0}'?", file_name))
    {
      File::Delete(file_name);
    }
    else
    {
      // Stop and cancel dumping the audio
      return false;
    }
  }

  // Check if the file is already open
  if (file)
  {
    PanicAlertFmtT("The file {0} was already open, the file header will not be written.",
                file_name);
    return false;
  }

  file.Open(file_name, "wb");
  if (!file)
  {
    PanicAlertFmtT("The file {0} could not be opened for writing. Please check if it's already "
                "opened by another program.", file_name);
    return false;
  }

  audio_size = 0;

  if (basename.empty())
    SplitPath(file_name, nullptr, &basename, nullptr);

  current_sample_rate = sample_rate;

  // -----------------
  // Write file header
  // -----------------
  Write4("RIFF");
  Write(100 * 1000 * 1000);  // write big value in case the file gets truncated
  Write4("WAVE");
  Write4("fmt ");

  Write(16);          // size of fmt block
  Write(0x00020001);  // two channels, uncompressed

  Write(sample_rate);
  Write(sample_rate * 2 * 2);  // two channels, 16bit

  Write(0x00100004);
  Write4("data");
  Write(100 * 1000 * 1000 - 32);

  // We are now at offset 44
  if (file.Tell() != 44)
    PanicAlertFmt("Wrong offset: {}", file.Tell());

  return true;
}

void WaveFileWriter::Stop()
{
  // u32 file_size = (u32)ftello(file);
  file.Seek(4, SEEK_SET);
  Write(audio_size + 36);

  file.Seek(40, SEEK_SET);
  Write(audio_size);

  file.Close();
}

void WaveFileWriter::Write(u32 value)
{
  file.WriteArray(&value, 1);
}

void WaveFileWriter::Write4(const char* ptr)
{
  file.WriteBytes(ptr, 4);
}

void WaveFileWriter::AddStereoSamplesBE(const short* sample_data, u32 count, u32 sample_rate)
{
  if (!file)
    ERROR_LOG_FMT(AUDIO, "WaveFileWriter - file not open.");

  if (count > BUFFER_SIZE / 2)
    ERROR_LOG_FMT(AUDIO, "WaveFileWriter - buffer too small (count = {}).", count);

  if (skip_silence)
  {
    bool all_zero = true;

    for (u32 i = 0; i < count * 2; i++)
    {
      if (sample_data[i])
      {
        all_zero = false;
        break;
      }
    }

    if (all_zero)
      return;
  }

  if (sample_rate != current_sample_rate)
  {
    Stop();
    file_index++;
    std::ostringstream filename;
    filename << File::GetUserPath(D_DUMPAUDIO_IDX) << basename << file_index << ".wav";
    current_sample_rate = sample_rate;  // Avoid trying again if Start() fails
    if (!Start(filename.str(), sample_rate))
      return;
  }

  for (u32 i = 0; i < count; i++)
  {
    // Flip the audio channels from RL to LR
    conv_buffer[2 * i] = Common::swap16((u16)sample_data[2 * i + 1]);
    conv_buffer[2 * i + 1] = Common::swap16((u16)sample_data[2 * i]);
  }

  file.WriteBytes(conv_buffer.data(), count * 4);
  audio_size += count * 4;
}
