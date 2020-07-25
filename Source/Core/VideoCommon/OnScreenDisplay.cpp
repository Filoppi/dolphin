// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/OnScreenDisplay.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"

#include "Core/ConfigManager.h"

namespace OSD
{
constexpr float LEFT_MARGIN = 10.0f;         // Pixels to the left of OSD messages.
constexpr float TOP_MARGIN = 10.0f;          // Pixels above the first OSD message.
constexpr float WINDOW_PADDING = 4.0f;       // Pixels between subsequent OSD messages.
constexpr float MESSAGE_FADE_TIME = 1000.f;  // Ms to fade OSD messages at the end of their life.

struct Message
{
  Message() = default;
  Message(std::string text_, u32 timestamp_, u32 duration_, u32 color_)
      : text(std::move(text_)), timestamp(timestamp_), duration(duration_), color(color_)
  {
  }
  std::string text;
  u32 timestamp = 0;
  u32 duration = 0;
  bool ever_drawn = false;
  u32 color = 0;
};
static std::multimap<MessageType, Message> s_messages;
static std::mutex s_messages_mutex;

static ImVec4 RGBAToImVec4(const u32 rgba)
{
  return ImVec4(static_cast<float>((rgba >> 16) & 0xFF) / 255.0f,
                static_cast<float>((rgba >> 8) & 0xFF) / 255.0f,
                static_cast<float>((rgba >> 0) & 0xFF) / 255.0f,
                static_cast<float>((rgba >> 24) & 0xFF) / 255.0f);
}

static float DrawMessage(int index, Message& msg, const ImVec2& position, int time_left)
{
  // We have to provide a window name, and these shouldn't be duplicated.
  // So instead, we generate a name based on the number of messages drawn.
  const std::string window_name = fmt::format("osd_{}", index);

  // The size must be reset, otherwise the length of old messages could influence new ones.
  ImGui::SetNextWindowPos(position);
  ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));

  // Gradually fade old messages away (except in their first frame)
  const float fade_time = std::max(std::min(MESSAGE_FADE_TIME, (float)msg.duration), 1.f);
  const float alpha = std::clamp(time_left / fade_time, 0.0f, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, msg.ever_drawn ? alpha : 1.0);

  float window_height = 0.0f;
  if (ImGui::Begin(window_name.c_str(), nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    // Use %s in case message contains %.
    ImGui::TextColored(RGBAToImVec4(msg.color), "%s", msg.text.c_str());
    window_height =
        ImGui::GetWindowSize().y + (WINDOW_PADDING * ImGui::GetIO().DisplayFramebufferScale.y);
  }

  ImGui::End();
  ImGui::PopStyleVar();

  msg.ever_drawn = true;

  return window_height;
}

void AddTypedMessage(MessageType type, std::string message, u32 ms, u32 rgba)
{
  std::lock_guard lock{s_messages_mutex};
  s_messages.erase(type);
  s_messages.emplace(type, Message(std::move(message), Common::Timer::GetTimeMs() + ms, ms, rgba));
}

void AddMessage(std::string message, u32 ms, u32 rgba)
{
  std::lock_guard lock{s_messages_mutex};
  s_messages.emplace(MessageType::Typeless,
                     Message(std::move(message), Common::Timer::GetTimeMs() + ms, ms, rgba));
}

void DrawMessages()
{
  const bool draw_messages = SConfig::GetInstance().bOnScreenDisplayMessages;
  const u32 now = Common::Timer::GetTimeMs();
  const float current_x = LEFT_MARGIN * ImGui::GetIO().DisplayFramebufferScale.x;
  float current_y = TOP_MARGIN * ImGui::GetIO().DisplayFramebufferScale.y;
  int index = 0;

  std::lock_guard lock{s_messages_mutex};

  for (auto it = s_messages.begin(); it != s_messages.end();)
  {
    Message& msg = it->second;
    const int time_left = static_cast<int>(msg.timestamp - now);

    // Make sure we draw them at least once if they were printed with 0ms
    if (time_left <= 0 && msg.ever_drawn)
    {
      it = s_messages.erase(it);
      continue;
    }
    else
    {
      ++it;
    }

    if (draw_messages)
      current_y += DrawMessage(index++, msg, ImVec2(current_x, current_y), time_left);
  }
}

void ClearMessages()
{
  std::lock_guard lock{s_messages_mutex};
  s_messages.clear();
}
}  // namespace OSD
