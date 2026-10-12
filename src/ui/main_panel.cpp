#include "klip/ui/main_panel.h"
#include "klip/ui/event_driven_tabs.h"
#include "klip/core/path_text.h"
#include "klip/obs/replay_policy.h"

#include <Windows.h>
#include <imgui.h>
#if defined(KLIP_USE_LIBOBS)
#include <obs.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string_view>

namespace klip {
namespace {
#if defined(KLIP_USE_LIBOBS)
std::string ExecutableAudioSelector(std::string_view window_value) {
  // OBS window selectors are serialized as title:class:executable. Store only
  // the executable portion so title changes (Discord chats, Spotify tracks)
  // cannot invalidate the selection. The WASAPI source uses priority=2 (exe).
  const auto first = window_value.find(':');
  if (first == std::string_view::npos) return {};
  const auto second = window_value.find(':', first + 1);
  if (second == std::string_view::npos || second + 1 >= window_value.size()) return {};
  return "::" + std::string(window_value.substr(second + 1));
}

void DrawAudioWindowSelector(const char* label, std::string& selected) {
  // Migrate settings saved by earlier builds, which stored the whole changing
  // title:class:executable window value.
  if (!selected.empty() && !selected.starts_with("::")) {
    const auto stable = ExecutableAudioSelector(selected);
    if (!stable.empty()) selected = stable;
  }
  obs_properties_t* props = obs_get_source_properties("wasapi_process_output_capture");
  if (!props) { ImGui::TextDisabled("Application audio capture is unavailable."); return; }
  obs_property_t* window = obs_properties_get(props, "window");
  std::string selected_preview = selected.empty()
      ? "Choose an application executable"
      : "Selected: " + selected.substr(2) + " (may be closed)";
  if (window) for (std::size_t i = 0; i < obs_property_list_item_count(window); ++i) {
    const char* value = obs_property_list_item_string(window, i);
    if (value && ExecutableAudioSelector(value) == selected) {
      selected_preview = std::string(obs_property_list_item_name(window, i)) +
          "  (" + selected.substr(2) + ")";
      break;
    }
  }
  const char* preview = selected_preview.c_str();
  if (ImGui::BeginCombo(label, preview)) {
    if (ImGui::Selectable("Not selected", selected.empty())) selected.clear();
    if (window) for (std::size_t i = 0; i < obs_property_list_item_count(window); ++i) {
      if (obs_property_list_item_disabled(window, i)) continue;
      const char* value = obs_property_list_item_string(window, i);
      if (!value || !*value) continue;
      const std::string executable_selector = ExecutableAudioSelector(value);
      if (executable_selector.empty()) continue;
      const bool active = selected == executable_selector;
      const std::string item_label = std::string(obs_property_list_item_name(window, i)) +
          "  (" + executable_selector.substr(2) + ")";
      if (ImGui::Selectable(item_label.c_str(), active)) selected = executable_selector;
      if (active) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  obs_properties_destroy(props);
}
#endif


constexpr ImVec4 kMuted{0.48F, 0.65F, 0.69F, 1.0F};
constexpr ImVec4 kMint{0.12F, 0.95F, 0.88F, 1.0F};
constexpr ImVec4 kCoral{1.0F, 0.38F, 0.43F, 1.0F};
constexpr ImVec4 kWarningText{0.97F, 0.72F, 0.27F, 1.0F};
constexpr ImU32 kBackgroundTop = IM_COL32(3, 10, 14, 255);
constexpr ImU32 kBackgroundBottom = IM_COL32(4, 9, 13, 255);
constexpr ImU32 kPanel = IM_COL32(7, 18, 23, 246);
constexpr ImU32 kPanelHover = IM_COL32(10, 30, 37, 255);
constexpr ImU32 kPanelBorder = IM_COL32(25, 69, 77, 210);
constexpr ImU32 kDivider = IM_COL32(30, 221, 226, 32);
constexpr ImU32 kTextU32 = IM_COL32(235, 249, 250, 255);
constexpr ImU32 kMutedU32 = IM_COL32(132, 164, 171, 255);
constexpr ImU32 kPurple = IM_COL32(0, 166, 182, 255);
constexpr ImU32 kPurpleBright = IM_COL32(24, 229, 234, 255);
constexpr ImU32 kGreen = IM_COL32(22, 223, 199, 255);
constexpr ImU32 kWarning = IM_COL32(250, 176, 65, 255);
constexpr ImU32 kRed = IM_COL32(255, 97, 110, 255);

void DrawText(ImDrawList* draw, ImFont* font, float size, ImVec2 position, ImU32 color,
              std::string_view text);

constexpr float kMinGainDb = -60.0F;
constexpr float kMaxGainDb = 6.0F;

float GainToDb(float gain) {
  if (!std::isfinite(gain) || gain <= 0.001F) return kMinGainDb;
  return std::clamp(20.0F * std::log10(gain), kMinGainDb, kMaxGainDb);
}

float DbToGain(float db) {
  if (!std::isfinite(db) || db <= kMinGainDb) return 0.0F;
  return std::clamp(std::pow(10.0F, db / 20.0F), 0.0F, 2.0F);
}

const char* StatusText(CaptureStatus status) {
  switch (status) {
    case CaptureStatus::kIdle:
      return "idle";
    case CaptureStatus::kStarting:
      return "starting";
    case CaptureStatus::kBuffering:
      return "buffer live";
    case CaptureStatus::kSaving:
      return "saving clip";
    case CaptureStatus::kStopping:
      return "stopping";
    case CaptureStatus::kFailed:
      return "needs attention";
  }
  return "idle";
}

ImVec4 StatusColor(CaptureStatus status) {
  if (status == CaptureStatus::kBuffering) return kMint;
  if (status == CaptureStatus::kSaving) return {1.0F, 0.73F, 0.25F, 1.0F};
  if (status == CaptureStatus::kFailed) return kCoral;
  return kMuted;
}

ImFont* UiFont(std::size_t index) {
  const auto& fonts = ImGui::GetIO().Fonts->Fonts;
  return index < static_cast<std::size_t>(fonts.Size) ? fonts[static_cast<int>(index)]
                                                      : ImGui::GetFont();
}

void SectionLabel(const char* text) {
  ImGui::Spacing();
  ImGui::PushFont(UiFont(4));
  ImGui::TextColored(kMuted, "%s", text);
  ImGui::PopFont();
  ImGui::Separator();
}

void DrawText(ImDrawList* draw, ImFont* font, float size, ImVec2 position, ImU32 color,
              std::string_view text) {
  draw->AddText(font, size, position, color, text.data(), text.data() + text.size());
}

float TextWidth(ImFont* font, float size, std::string_view text) {
  return font->CalcTextSizeA(size, 10000.0F, 0.0F, text.data(), text.data() + text.size()).x;
}

void DrawCard(ImDrawList* draw, ImVec2 minimum, ImVec2 size, bool hovered = false,
              ImU32 fill = kPanel, ImU32 border = kPanelBorder, float rounding = 8.0F) {
  const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
  draw->AddRectFilled(minimum, maximum, hovered ? kPanelHover : fill, rounding);
  draw->AddRect(minimum, maximum, border, rounding, 0, 1.0F);
}

void DrawLogo(ImDrawList* draw, ImVec2 center, float scale) {
  // Incoming point is the visual center so the mark is aligned consistently
  // across the sidebar, hero, and settings header.
  const auto p = [&](float x, float y) {
    return ImVec2{center.x + (x - 25.0F) * scale,
                  center.y + (y - 24.0F) * scale};
  };
  const ImU32 glow = IM_COL32(0, 226, 239, 34);
  const ImU32 shadow = IM_COL32(0, 88, 105, 150);
  const ImU32 cyan = IM_COL32(25, 232, 239, 255);
  const ImU32 bright = IM_COL32(110, 255, 255, 255);
  const ImU32 teal = IM_COL32(0, 179, 205, 255);

  // Soft cyan glow and a dark, offset extrusion echo the supplied REVO mark.
  draw->AddLine(p(7, 5), p(28, 5), glow, 17.0F * scale);
  draw->AddLine(p(28, 5), p(37, 14), glow, 17.0F * scale);
  draw->AddLine(p(37, 14), p(37, 22), glow, 17.0F * scale);
  draw->AddLine(p(37, 22), p(28, 30), glow, 17.0F * scale);
  draw->AddLine(p(28, 30), p(42, 44), glow, 17.0F * scale);

  draw->AddLine(p(9, 7), p(9, 41), shadow, 12.0F * scale);
  draw->AddLine(p(9, 7), p(27, 7), shadow, 12.0F * scale);
  draw->AddLine(p(27, 7), p(35, 15), shadow, 12.0F * scale);
  draw->AddLine(p(35, 15), p(35, 21), shadow, 12.0F * scale);
  draw->AddLine(p(35, 21), p(27, 28), shadow, 12.0F * scale);
  draw->AddLine(p(27, 28), p(40, 42), shadow, 12.0F * scale);

  draw->AddLine(p(7, 4), p(26, 4), bright, 8.0F * scale);
  draw->AddBezierCubic(p(26, 4), p(33, 4), p(37, 10), p(39, 15), cyan, 8.0F * scale);
  draw->AddBezierCubic(p(39, 15), p(43, 22), p(38, 27), p(30, 30), cyan, 8.0F * scale);
  draw->AddLine(p(7, 4), p(7, 42), cyan, 8.0F * scale);
  draw->AddLine(p(14, 25), p(28, 25), teal, 7.0F * scale);
  draw->AddLine(p(25, 28), p(40, 43), cyan, 9.0F * scale);
  draw->AddLine(p(7, 4), p(26, 4), bright, 2.0F * scale);
}

void DrawMonitorIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddRect({center.x - 15.0F, center.y - 12.0F}, {center.x + 15.0F, center.y + 8.0F}, color,
                3.0F, 0, 2.0F);
  draw->AddLine({center.x, center.y + 8.0F}, {center.x, center.y + 14.0F}, color, 2.0F);
  draw->AddLine({center.x - 8.0F, center.y + 14.0F}, {center.x + 8.0F, center.y + 14.0F}, color,
                2.0F);
}

void DrawControllerIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddRect({center.x - 16.0F, center.y - 9.0F}, {center.x + 16.0F, center.y + 10.0F}, color,
                7.0F, 0, 2.0F);
  draw->AddLine({center.x - 10.0F, center.y}, {center.x - 4.0F, center.y}, color, 2.0F);
  draw->AddLine({center.x - 7.0F, center.y - 3.0F}, {center.x - 7.0F, center.y + 3.0F}, color,
                2.0F);
  draw->AddCircleFilled({center.x + 7.0F, center.y - 2.0F}, 1.8F, color);
  draw->AddCircleFilled({center.x + 11.0F, center.y + 2.0F}, 1.8F, color);
}

void DrawSpeakerIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  const ImVec2 speaker[] = {
      {center.x - 15.0F, center.y - 5.0F}, {center.x - 9.0F, center.y - 5.0F},
      {center.x - 2.0F, center.y - 12.0F}, {center.x - 2.0F, center.y + 12.0F},
      {center.x - 9.0F, center.y + 5.0F},  {center.x - 15.0F, center.y + 5.0F}};
  draw->AddPolyline(speaker, 6, color, ImDrawFlags_Closed, 2.0F);
  draw->PathArcTo(center, 9.0F, -0.8F, 0.8F, 16);
  draw->PathStroke(color, 0, 2.0F);
  draw->PathArcTo(center, 15.0F, -0.7F, 0.7F, 16);
  draw->PathStroke(color, 0, 2.0F);
}

void DrawMicrophoneIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddRect({center.x - 6.0F, center.y - 13.0F}, {center.x + 6.0F, center.y + 5.0F}, color,
                6.0F, 0, 2.0F);
  draw->PathArcTo(center, 12.0F, 0.15F, 3.0F, 24);
  draw->PathStroke(color, 0, 2.0F);
  draw->AddLine({center.x, center.y + 12.0F}, {center.x, center.y + 17.0F}, color, 2.0F);
  draw->AddLine({center.x - 7.0F, center.y + 17.0F}, {center.x + 7.0F, center.y + 17.0F}, color,
                2.0F);
}

void DrawRecordIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddCircle(center, 15.0F, color, 32, 2.5F);
  draw->AddCircleFilled(center, 6.0F, color);
}

void DrawClipIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddRect({center.x - 13.0F, center.y - 12.0F}, {center.x + 13.0F, center.y + 12.0F}, color,
                4.0F, 0, 2.0F);
  draw->AddLine({center.x - 6.0F, center.y - 3.0F}, {center.x + 6.0F, center.y - 3.0F}, color,
                2.0F);
  draw->AddLine({center.x + 6.0F, center.y - 3.0F}, {center.x + 3.0F, center.y - 6.0F}, color,
                2.0F);
  draw->AddLine({center.x + 6.0F, center.y - 3.0F}, {center.x + 3.0F, center.y}, color, 2.0F);
  draw->AddLine({center.x + 6.0F, center.y + 4.0F}, {center.x - 6.0F, center.y + 4.0F}, color,
                2.0F);
}

void DrawFolderIcon(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddRect({center.x - 13.0F, center.y - 8.0F}, {center.x + 13.0F, center.y + 10.0F}, color,
                3.0F, 0, 2.0F);
  draw->AddLine({center.x - 11.0F, center.y - 9.0F}, {center.x - 3.0F, center.y - 9.0F}, color,
                2.0F);
  draw->AddLine({center.x - 3.0F, center.y - 9.0F}, {center.x + 1.0F, center.y - 5.0F}, color,
                2.0F);
}

void DrawChevron(ImDrawList* draw, ImVec2 center, ImU32 color) {
  draw->AddLine({center.x - 6.0F, center.y - 3.0F}, {center.x, center.y + 3.0F}, color, 2.0F);
  draw->AddLine({center.x, center.y + 3.0F}, {center.x + 6.0F, center.y - 3.0F}, color, 2.0F);
}

void DrawProgress(ImDrawList* draw, ImVec2 minimum, ImVec2 size, float level,
                  ImU32 active = kPurpleBright) {
  draw->AddRectFilled(minimum, {minimum.x + size.x, minimum.y + size.y}, IM_COL32(35, 42, 52, 255),
                      size.y * 0.5F);
  const float width = size.x * std::clamp(level, 0.0F, 1.0F);
  if (width > 0.5F)
    draw->AddRectFilled(minimum, {minimum.x + width, minimum.y + size.y}, active, size.y * 0.5F);
}

bool Switch(const char* id, ImVec2 position, bool enabled) {
  ImGui::SetCursorScreenPos(position);
  ImGui::InvisibleButton(id, {48.0F, 26.0F});
  const bool clicked = ImGui::IsItemClicked();
  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(position, {position.x + 48.0F, position.y + 26.0F},
                      enabled ? kPurple : IM_COL32(75, 82, 95, 255), 13.0F);
  const float knob_x = position.x + (enabled ? 35.0F : 13.0F);
  draw->AddCircleFilled({knob_x, position.y + 13.0F}, 10.0F, IM_COL32(232, 235, 241, 255));
  return clicked;
}

bool GainSlider(const char* id, ImVec2 position, float width, float& gain) {
  constexpr float height = 22.0F;
  ImGui::SetCursorScreenPos(position);
  ImGui::InvisibleButton(id, {width, height});
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  bool changed = false;
  float db = GainToDb(gain);
  if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float normalized =
        std::clamp((ImGui::GetIO().MousePos.x - position.x) / width, 0.0F, 1.0F);
    const float updated_db = kMinGainDb + normalized * (kMaxGainDb - kMinGainDb);
    const float updated_gain = DbToGain(updated_db);
    if (std::abs(updated_gain - gain) > 0.001F) {
      gain = updated_gain;
      db = updated_db;
      changed = true;
    }
  }
  auto* draw = ImGui::GetWindowDrawList();
  const float track_y = position.y + height * 0.5F - 3.0F;
  draw->AddRectFilled({position.x, track_y}, {position.x + width, track_y + 6.0F},
                      IM_COL32(22, 42, 49, 255), 3.0F);
  const float progress = std::clamp((db - kMinGainDb) / (kMaxGainDb - kMinGainDb), 0.0F, 1.0F);
  const float knob_x = position.x + width * progress;
  if (progress > 0.001F)
    draw->AddRectFilled({position.x, track_y}, {knob_x, track_y + 6.0F}, kPurpleBright, 3.0F);
  if (hovered || active)
    draw->AddCircleFilled({knob_x, position.y + height * 0.5F}, 9.0F, IM_COL32(0, 226, 234, 44));
  draw->AddCircleFilled({knob_x, position.y + height * 0.5F}, active ? 6.0F : 5.0F,
                        IM_COL32(202, 252, 253, 255));
  return changed;
}

bool PopupOption(const char* id, std::string_view label, bool selected) {
  const ImVec2 position = ImGui::GetCursorScreenPos();
  const ImVec2 size{ImGui::GetContentRegionAvail().x, 42.0F};
  ImGui::InvisibleButton(id, size);
  const bool clicked = ImGui::IsItemClicked();
  const bool hovered = ImGui::IsItemHovered();
  auto* draw = ImGui::GetWindowDrawList();
  if (selected || hovered) {
    draw->AddRectFilled(position, {position.x + size.x, position.y + size.y},
                        selected ? IM_COL32(0, 155, 178, 150) : IM_COL32(255, 255, 255, 10), 8.0F);
  }
  draw->AddCircle({position.x + 20.0F, position.y + 21.0F}, 7.0F,
                  selected ? kPurpleBright : kMutedU32, 16, 1.5F);
  if (selected)
    draw->AddCircleFilled({position.x + 20.0F, position.y + 21.0F}, 3.5F, kPurpleBright);
  DrawText(draw, UiFont(1), 14.0F, {position.x + 39.0F, position.y + 12.0F},
           selected ? kTextU32 : kMutedU32, label);
  return clicked;
}

std::string Ellipsize(ImFont* font, float size, std::string_view text, float maximum_width) {
  if (TextWidth(font, size, text) <= maximum_width) return std::string(text);
  std::string shortened(text);
  constexpr std::string_view ellipsis = "...";
  while (!shortened.empty() &&
         TextWidth(font, size, shortened + std::string(ellipsis)) > maximum_width) {
    shortened.pop_back();
  }
  shortened += ellipsis;
  return shortened;
}

std::string CompactSourceLabel(std::string_view label) {
  const auto open = label.rfind('[');
  const auto close = label.rfind(']');
  if (open != std::string_view::npos && close != std::string_view::npos && close > open + 1)
    return std::string(label.substr(open + 1, close - open - 1));
  return std::string(label);
}

#if !defined(KLIP_USE_LIBOBS)
int EncoderProfile(const std::vector<std::string>& preferences) {
  if (preferences.size() > 1) return 0;
  if (preferences.empty()) return 0;
  if (preferences[0] == "h264_nvenc") return 1;
  if (preferences[0] == "h264_amf") return 2;
  if (preferences[0] == "h264_mf") return 3;
  return 0;
}

void SetEncoderProfile(AppConfig& config, int profile) {
  switch (profile) {
    case 1:
      config.encoder_preferences = {"h264_nvenc"};
      break;
    case 2:
      config.encoder_preferences = {"h264_amf"};
      break;
    case 3:
      config.encoder_preferences = {"h264_mf"};
      break;
    default:
      config.encoder_preferences = {"h264_nvenc", "h264_amf", "h264_mf"};
      break;
  }
}

#endif

std::filesystem::path ParsePath(const char* text) {
  std::u8string utf8;
  for (const auto* byte = reinterpret_cast<const unsigned char*>(text); *byte != 0; ++byte)
    utf8.push_back(static_cast<char8_t>(*byte));
  return std::filesystem::path(utf8);
}

void ResizeReplayBudget(AppConfig& config) {
  config.rolling_buffer_seconds = config.clip_duration_seconds + 15.0;
#if defined(KLIP_USE_LIBOBS)
  // CQ bitrate is scene-dependent; the legacy fixed-bitrate estimate used to
  // shrink OBS's buffer when selecting a duration. Keep its explicit budget.
  return;
#endif
  const auto bits_per_second = static_cast<double>(config.video_bitrate + config.audio_bitrate);
  const auto required = bits_per_second / 8.0 * config.rolling_buffer_seconds * 1.25;
  constexpr auto minimum = 32ULL * 1024ULL * 1024ULL;
  config.rolling_buffer_bytes = std::max<std::size_t>(minimum, static_cast<std::size_t>(required));
}

void ApplyPerformanceMode(AppConfig& config, bool enabled) {
#if defined(KLIP_USE_LIBOBS)
  config.encoder_quality = enabled ? EncoderQuality::kPerformance : EncoderQuality::kBalanced;
#else
  config.target_fps = 60;
  config.output_width = enabled ? 1280U : 1920U;
  config.output_height = enabled ? 720U : 1080U;
  config.video_bitrate = enabled ? 8'000'000 : 12'000'000;
  config.encoder_quality = enabled ? EncoderQuality::kPerformance : EncoderQuality::kBalanced;
  if (enabled) {
    config.capture_preview_enabled = false;
    config.static_overlay_enabled = false;
    config.live_overlay_enabled = false;
  }
  SetEncoderProfile(config, 0);
  ResizeReplayBudget(config);
#endif
}

bool IsPerformanceMode(const AppConfig& config) {
#if defined(KLIP_USE_LIBOBS)
  return config.encoder_quality == EncoderQuality::kPerformance;
#else
  return config.target_fps == 60 && config.output_width == 1280 && config.output_height == 720 &&
         config.video_bitrate == 8'000'000 &&
         config.encoder_quality == EncoderQuality::kPerformance && !config.static_overlay_enabled &&
         !config.live_overlay_enabled && EncoderProfile(config.encoder_preferences) == 0;
#endif
}

std::optional<unsigned int> VirtualKeyForImGuiKey(ImGuiKey key) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
    return static_cast<unsigned int>('A' + (key - ImGuiKey_A));
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
    return static_cast<unsigned int>('0' + (key - ImGuiKey_0));
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
    return static_cast<unsigned int>(0x70 + (key - ImGuiKey_F1));
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
    return static_cast<unsigned int>(0x60 + (key - ImGuiKey_Keypad0));
  switch (key) {
    case ImGuiKey_Tab:
      return 0x09;
    case ImGuiKey_LeftArrow:
      return 0x25;
    case ImGuiKey_RightArrow:
      return 0x27;
    case ImGuiKey_UpArrow:
      return 0x26;
    case ImGuiKey_DownArrow:
      return 0x28;
    case ImGuiKey_PageUp:
      return 0x21;
    case ImGuiKey_PageDown:
      return 0x22;
    case ImGuiKey_Home:
      return 0x24;
    case ImGuiKey_End:
      return 0x23;
    case ImGuiKey_Insert:
      return 0x2D;
    case ImGuiKey_Delete:
      return 0x2E;
    case ImGuiKey_Backspace:
      return 0x08;
    case ImGuiKey_Space:
      return 0x20;
    case ImGuiKey_Enter:
      return 0x0D;
    case ImGuiKey_CapsLock:
      return 0x14;
    case ImGuiKey_ScrollLock:
      return 0x91;
    case ImGuiKey_NumLock:
      return 0x90;
    case ImGuiKey_PrintScreen:
      return 0x2C;
    case ImGuiKey_Pause:
      return 0x13;
    case ImGuiKey_KeypadDecimal:
      return 0x6E;
    case ImGuiKey_KeypadDivide:
      return 0x6F;
    case ImGuiKey_KeypadMultiply:
      return 0x6A;
    case ImGuiKey_KeypadSubtract:
      return 0x6D;
    case ImGuiKey_KeypadAdd:
      return 0x6B;
    case ImGuiKey_KeypadEnter:
      return 0x0D;
    case ImGuiKey_Apostrophe:
      return 0xDE;
    case ImGuiKey_Comma:
      return 0xBC;
    case ImGuiKey_Minus:
      return 0xBD;
    case ImGuiKey_Period:
      return 0xBE;
    case ImGuiKey_Slash:
      return 0xBF;
    case ImGuiKey_Semicolon:
      return 0xBA;
    case ImGuiKey_Equal:
      return 0xBB;
    case ImGuiKey_LeftBracket:
      return 0xDB;
    case ImGuiKey_Backslash:
      return 0xDC;
    case ImGuiKey_RightBracket:
      return 0xDD;
    case ImGuiKey_GraveAccent:
      return 0xC0;
    default:
      return std::nullopt;
  }
}

bool SameChord(unsigned int modifiers_a, unsigned int key_a, unsigned int modifiers_b,
               unsigned int key_b) {
  constexpr unsigned int chord_mask =
      HotkeyConfig::kAlt | HotkeyConfig::kControl | HotkeyConfig::kShift | HotkeyConfig::kWindows;
  return (modifiers_a & chord_mask) == (modifiers_b & chord_mask) && key_a == key_b;
}

struct WindowsVersionInfo {
  std::string summary = "unavailable";
  std::optional<DWORD> build;
};

const WindowsVersionInfo& CurrentWindowsVersion() {
  static const auto version_info = [] {
    WindowsVersionInfo info;
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto get_version =
        module != nullptr
            ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(module, "RtlGetVersion"))
            : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version != nullptr && get_version(&version) == 0) {
      info.build = version.dwBuildNumber;
      info.summary = std::to_string(version.dwMajorVersion) + "." +
                     std::to_string(version.dwMinorVersion) + " (build " +
                     std::to_string(version.dwBuildNumber) + ")";
    }
    return info;
  }();
  return version_info;
}

std::string WindowsBuildSummary() { return CurrentWindowsVersion().summary; }

std::optional<DWORD> WindowsBuildNumber() { return CurrentWindowsVersion().build; }

bool IsSupportedArchitecture() {
  static const bool is_x64 = [] {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    return system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
  }();
  return is_x64;
}

bool IsSupportedD3dLevel(const std::string& level) {
  constexpr std::string_view supported_levels[] = {"10.0", "10.1", "11.0", "11.1",
                                                   "12.0", "12.1", "12.2"};
  return std::find(std::begin(supported_levels), std::end(supported_levels), level) !=
         std::end(supported_levels);
}

std::string SystemHardwareSummary() {
  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  const char* architecture = "unknown";
  switch (system.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      architecture = "x64";
      break;
    case PROCESSOR_ARCHITECTURE_ARM64:
      architecture = "ARM64";
      break;
    case PROCESSOR_ARCHITECTURE_INTEL:
      architecture = "x86";
      break;
    default:
      break;
  }
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  const auto total_memory_gib =
      GlobalMemoryStatusEx(&memory) ? (memory.ullTotalPhys + (1ULL << 29U)) / (1ULL << 30U) : 0;
  std::ostringstream summary;
  summary << architecture << ", " << system.dwNumberOfProcessors << " logical processors";
  if (total_memory_gib != 0) summary << ", " << total_memory_gib << " GiB RAM";
  return summary.str();
}

std::string BuildDiagnostics(const ApplicationSnapshot& snapshot, const AppConfig& config,
                             bool include_private_details) {
  std::ostringstream report;
  report << "REVO v3.0.4 diagnostics\n"
         << "Application build: Windows x64\n"
         << "Windows version: " << WindowsBuildSummary() << '\n'
         << "System: " << SystemHardwareSummary() << '\n'
         << "Capture mode: "
         << (snapshot.target_mode == CaptureTargetMode::kDisplay ? "display" : "game/window")
         << '\n'
         << "D3D feature level: "
         << (snapshot.graphics_feature_level.empty() ? "unavailable" : snapshot.graphics_feature_level)
         << '\n'
         << "Capture/display GPU routing: "
         << (snapshot.capture_adapter_relationship.empty() ? "unknown"
                                                            : snapshot.capture_adapter_relationship)
         << '\n'
         << "Status: " << StatusText(snapshot.status) << '\n'
         << "Output: "
         << (config.output_width == 0 ? "source" : std::to_string(config.output_width)) << 'x'
         << (config.output_height == 0 ? "source" : std::to_string(config.output_height)) << " at "
         << config.target_fps << " fps\n"
         << "Encoder: "
         << (snapshot.selected_encoder.empty() ? "not selected" : snapshot.selected_encoder) << '\n'
         << "Encoder status: "
         << (snapshot.encoder_status.empty() ? "preferred path active" : snapshot.encoder_status)
         << '\n'
         << "Source updates: " << snapshot.metrics.source_fps << " fps\n"
         << "Encoded output: " << snapshot.metrics.capture_fps << " fps\n"
         << "Replay buffer: " << snapshot.metrics.rolling_buffer_seconds << " sec, "
         << (snapshot.metrics.rolling_buffer_bytes / (1024 * 1024)) << " MiB\n"
         << "Video bitrate: " << (config.video_bitrate / 1'000'000) << " Mbps\n"
         << "Dropped frames: "
         << (snapshot.metrics.dropped_raw_frames + snapshot.metrics.dropped_encode_frames)
         << " (recording packets " << snapshot.metrics.dropped_recording_packets << ")\n"
         << "Encode latency: " << snapshot.metrics.encode_latency_ms << " ms\n"
         << "Desktop audio: " << (snapshot.desktop_audio_active ? "active" : "off") << '\n'
         << "Microphone: " << (snapshot.microphone_active ? "active" : "off") << '\n'
         << "Microphone endpoints: " << snapshot.microphones.size() << '\n';
  std::error_code space_error;
  const auto clip_available_bytes =
      std::filesystem::space(config.output_directory, space_error).available;
  report << "Clip destination free space: ";
  if (space_error) {
    report << "unavailable\n";
  } else {
    report << (clip_available_bytes / (1024ULL * 1024ULL * 1024ULL)) << " GiB\n";
  }
  space_error.clear();
  const auto recording_available_bytes =
      std::filesystem::space(config.recording_directory, space_error).available;
  report << "Recording destination free space: ";
  if (space_error)
    report << "unavailable\n";
  else
    report << (recording_available_bytes / (1024ULL * 1024ULL * 1024ULL)) << " GiB\n";
  if (include_private_details) {
    report << "Graphics adapter: "
           << (snapshot.graphics_adapter.empty() ? "unknown" : snapshot.graphics_adapter)
           << " (vendor 0x" << std::hex << snapshot.graphics_adapter_vendor_id << std::dec << ")\n"
           << "Capture target: "
           << (snapshot.capture_target.empty() ? "not selected" : snapshot.capture_target) << '\n'
           << "Capture display adapter: "
           << (snapshot.capture_adapter.empty() ? "unknown" : snapshot.capture_adapter) << '\n'
           << "Clip output folder: " << PathToUtf8(config.output_directory) << '\n'
           << "Recording output folder: " << PathToUtf8(config.recording_directory) << '\n';
    for (std::size_t index = 0; index < snapshot.microphones.size(); ++index)
      report << "Microphone " << index + 1 << ": " << snapshot.microphones[index] << '\n';
  }
  if (snapshot.last_error) {
    report << "Error: " << ComponentName(snapshot.last_error->component) << " / "
           << snapshot.last_error->operation;
    if (include_private_details) report << " / " << snapshot.last_error->message;
    report << '\n';
  }
  report << (include_private_details
                 ? "\nContains device names and file paths; review before sharing."
                 : "\nPrivate device names, paths, and detailed error text are hidden by default.");
  return report.str();
}

}  // namespace

void MainPanel::Render(const ApplicationSnapshot& snapshot, const AppConfig& config,
                       const UiCommands& commands, bool hotkeys_available,
                       const CapturePreviewView& preview) {
  UpdateSetupTest(snapshot, commands);
  auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
  ImGui::SetNextWindowContentSize({viewport->WorkSize.x, std::max(viewport->WorkSize.y, 800.0F)});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  ImGui::Begin("REVO", nullptr, window_flags);

  if (settings_open_) {
    RenderSettings(snapshot, config, commands);
  } else {
    RenderDashboard(snapshot, config, commands, hotkeys_available);
  }
  ImGui::End();
  ImGui::PopStyleVar();
#if !defined(KLIP_USE_LIBOBS)
  if (config.capture_preview_enabled) RenderCapturePreview(preview, config);
#else
  (void)preview;  // No preview render path in the lightweight libobs build.
#endif
}

void MainPanel::RenderCapturePreview(const CapturePreviewView& preview, const AppConfig& config) {
  auto* viewport = ImGui::GetMainViewport();
  const float preview_width = std::min(410.0F, std::max(240.0F, viewport->WorkSize.x - 32.0F));
  const float preview_height = std::min(285.0F, std::max(180.0F, viewport->WorkSize.y - 96.0F));
  ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - preview_width - 16.0F,
                           viewport->WorkPos.y + 16.0F},
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize({preview_width, preview_height}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.97F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0F, 12.0F});
  if (ImGui::Begin("capture preview###klip-capture-preview", nullptr,
                   ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextColored(kMuted, "preview / 15 fps / display only");
    const auto available = ImGui::GetContentRegionAvail();
    const float source_aspect = preview.width > 0 && preview.height > 0
                                    ? static_cast<float>(preview.width) / preview.height
                                    : 16.0F / 9.0F;
    const float output_aspect = config.output_width > 0 && config.output_height > 0
                                    ? static_cast<float>(config.output_width) / config.output_height
                                    : source_aspect;
    ImVec2 canvas_size{available.x, available.x / output_aspect};
    if (canvas_size.y > available.y) {
      canvas_size.y = available.y;
      canvas_size.x = canvas_size.y * output_aspect;
    }
    if (preview.texture != nullptr && canvas_size.x > 1.0F && canvas_size.y > 1.0F) {
      const auto cursor = ImGui::GetCursorScreenPos();
      const ImVec2 canvas_min{cursor.x + (available.x - canvas_size.x) * 0.5F, cursor.y};
      const ImVec2 canvas_max{canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y};
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(canvas_min, canvas_max, IM_COL32(2, 3, 7, 255), 5.0F);

      ImVec2 capture_min = canvas_min;
      ImVec2 capture_max = canvas_max;
      if (config.scaling_mode == VideoScalingMode::kFit && source_aspect > 0.0F) {
        if (source_aspect > output_aspect) {
          const float height = canvas_size.x / source_aspect;
          const float inset = (canvas_size.y - height) * 0.5F;
          capture_min.y += inset;
          capture_max.y -= inset;
        } else if (source_aspect < output_aspect) {
          const float width = canvas_size.y * source_aspect;
          const float inset = (canvas_size.x - width) * 0.5F;
          capture_min.x += inset;
          capture_max.x -= inset;
        }
      }
      const auto texture_id =
          static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(preview.texture));
      draw->AddImage(ImTextureRef(texture_id), capture_min, capture_max);

      if ((config.static_overlay_enabled || config.live_overlay_enabled) &&
          preview.overlay_texture != nullptr) {
        const ImVec2 overlay_min{
            canvas_min.x + canvas_size.x * static_cast<float>(config.static_overlay_x),
            canvas_min.y + canvas_size.y * static_cast<float>(config.static_overlay_y)};
        const ImVec2 overlay_max{
            overlay_min.x + canvas_size.x * static_cast<float>(config.static_overlay_width),
            overlay_min.y + canvas_size.y * static_cast<float>(config.static_overlay_height)};
        const auto overlay_id =
            static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(preview.overlay_texture));
        const auto alpha =
            static_cast<int>(std::clamp(config.static_overlay_opacity, 0.0, 1.0) * 255.0 + 0.5);
        draw->AddImage(ImTextureRef(overlay_id), overlay_min, overlay_max, {0.0F, 0.0F},
                       {1.0F, 1.0F}, IM_COL32(255, 255, 255, alpha));
      }
      draw->AddRect(canvas_min, canvas_max, IM_COL32(78, 71, 91, 255), 5.0F);
      ImGui::Dummy({available.x, canvas_size.y});
    } else {
      ImGui::Dummy({1.0F, 70.0F});
      ImGui::TextColored(kMuted, "waiting for the selected capture source...");
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void MainPanel::RenderDashboard(const ApplicationSnapshot& snapshot, const AppConfig& config,
                                const UiCommands& commands, bool hotkeys_available) {
  auto* draw = ImGui::GetWindowDrawList();
  const auto visible_min = ImGui::GetWindowPos();
  const ImVec2 window_size = ImGui::GetWindowSize();
  const float content_height = std::max(window_size.y, 800.0F);
  const ImVec2 window_min{visible_min.x, visible_min.y - ImGui::GetScrollY()};
  const ImVec2 window_max{window_min.x + window_size.x, window_min.y + content_height};
  constexpr float sidebar_width = 246.0F;
  draw->AddRectFilledMultiColor(window_min, window_max, kBackgroundTop, kBackgroundTop,
                                kBackgroundBottom, kBackgroundBottom);
  draw->AddCircleFilled({window_max.x - 85.0F, window_min.y + 74.0F}, 295.0F,
                        IM_COL32(0, 224, 235, 14), 64);
  draw->AddCircleFilled({window_max.x - 150.0F, window_min.y + 165.0F}, 210.0F,
                        IM_COL32(0, 173, 194, 10), 64);
  draw->AddRectFilled(window_min, {window_min.x + sidebar_width, window_max.y},
                      IM_COL32(4, 16, 22, 252));
  draw->AddLine({window_min.x + sidebar_width, window_min.y + 1.0F},
                {window_min.x + sidebar_width, window_max.y - 1.0F},
                IM_COL32(22, 67, 75, 210), 1.0F);
  draw->AddRect(window_min, window_max, IM_COL32(21, 65, 74, 210), 10.0F, 0, 1.0F);

  auto* regular = UiFont(0);
  auto* semibold = UiFont(1);
  auto* title_font = UiFont(2);
  auto* number_font = UiFont(3);
  auto* label_font = UiFont(4);

  // Centered, slightly larger navigation mark with clear wordmark spacing.
  DrawLogo(draw, {window_min.x + 38.0F, window_min.y + 75.0F}, 0.82F);
  DrawText(draw, title_font, 25.0F, {window_min.x + 71.0F, window_min.y + 39.0F},
           kTextU32, "REVO");
  DrawText(draw, label_font, 9.0F, {window_min.x + 71.0F, window_min.y + 72.0F},
           kMutedU32, "CAPTURE / REPLAY");
  draw->AddLine({window_min.x + 20.0F, window_min.y + 111.0F},
                {window_min.x + sidebar_width - 20.0F, window_min.y + 111.0F},
                IM_COL32(18, 69, 78, 220), 1.0F);

  const auto draw_nav_item = [&](const char* id, const char* label, float y, bool active) {
    const ImVec2 position{window_min.x + 16.0F, window_min.y + y};
    const ImVec2 size{sidebar_width - 32.0F, 50.0F};
    ImGui::SetCursorScreenPos(position);
    ImGui::InvisibleButton(id, size);
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    if (active || hovered) {
      draw->AddRectFilled(position, {position.x + size.x, position.y + size.y},
                          active ? IM_COL32(0, 109, 121, 190)
                                 : IM_COL32(7, 37, 44, 220), 9.0F);
      draw->AddRect(position, {position.x + size.x, position.y + size.y},
                    active ? IM_COL32(16, 225, 230, 210) : IM_COL32(16, 87, 95, 180),
                    9.0F, 0, 1.0F);
    }
    if (active)
      draw->AddRectFilled({position.x, position.y + 7.0F},
                          {position.x + 3.0F, position.y + size.y - 7.0F},
                          kPurpleBright, 2.0F);
    const ImU32 text_color = active ? kPurpleBright : kTextU32;
    if (label[0] == 'H') {
      // Minimal house glyph.
      draw->AddLine({position.x + 22.0F, position.y + 24.0F},
                    {position.x + 31.0F, position.y + 16.0F}, text_color, 2.0F);
      draw->AddLine({position.x + 31.0F, position.y + 16.0F},
                    {position.x + 40.0F, position.y + 24.0F}, text_color, 2.0F);
      draw->AddRect({position.x + 24.0F, position.y + 23.0F},
                    {position.x + 38.0F, position.y + 35.0F}, text_color, 2.0F);
    } else {
      const ImVec2 center{position.x + 31.0F, position.y + 25.0F};
      draw->AddCircle(center, 7.0F, text_color, 20, 2.0F);
      draw->AddCircle(center, 2.2F, text_color, 16, 1.5F);
      for (int spoke = 0; spoke < 8; ++spoke) {
        const float angle = static_cast<float>(spoke) * 0.785398163F;
        const ImVec2 inner{center.x + std::cos(angle) * 7.0F,
                           center.y + std::sin(angle) * 7.0F};
        const ImVec2 outer{center.x + std::cos(angle) * 10.0F,
                           center.y + std::sin(angle) * 10.0F};
        draw->AddLine(inner, outer, text_color, 1.8F);
      }
    }
    DrawText(draw, semibold, 14.0F, {position.x + 52.0F, position.y + 17.0F},
             active ? kTextU32 : kMutedU32, label);
    return clicked;
  };
  if (draw_nav_item("##revo-home", "Home", 132.0F, !settings_open_)) {
    settings_open_ = false;
  }
  if (draw_nav_item("##revo-settings", "Settings", 190.0F, settings_open_)) {
    ResetDraft(config);
    settings_open_ = true;
  }

  const char* sidebar_status =
      snapshot.status == CaptureStatus::kBuffering ? "Ready to capture" : StatusText(snapshot.status);
  const ImU32 status_color = ImGui::ColorConvertFloat4ToU32(StatusColor(snapshot.status));
  const ImVec2 sidebar_status_pos{window_min.x + 18.0F, window_max.y - 96.0F};
  DrawCard(draw, sidebar_status_pos, {sidebar_width - 36.0F, 48.0F}, false,
           IM_COL32(5, 24, 30, 240), IM_COL32(15, 68, 76, 210), 9.0F);
  draw->AddCircleFilled({sidebar_status_pos.x + 20.0F, sidebar_status_pos.y + 19.0F},
                        5.5F, status_color);
  DrawText(draw, semibold, 11.0F, {sidebar_status_pos.x + 34.0F, sidebar_status_pos.y + 8.0F},
           kTextU32, sidebar_status);
  DrawText(draw, label_font, 9.0F, {sidebar_status_pos.x + 34.0F, sidebar_status_pos.y + 27.0F},
           kMutedU32, snapshot.status == CaptureStatus::kBuffering
                          ? "Live capture / replay buffer"
                          : "Capture status / details");
  const float margin_x = sidebar_width + 30.0F;
  const float content_width = window_size.x - margin_x - 30.0F;
  const ImVec2 content{window_min.x + margin_x, window_min.y};

  // REVO hero banner, with restrained cyan linework echoing the brand mark.
  DrawText(draw, label_font, 10.0F, {content.x + 18.0F, window_min.y + 18.0F},
           kMutedU32, "C A P T U R E   T H E   M O M E N T");
  DrawLogo(draw, {content.x + 50.0F, window_min.y + 90.0F}, 1.05F);
  DrawText(draw, title_font, 54.0F, {content.x + 82.0F, window_min.y + 37.0F},
           kTextU32, "REVO");
  DrawText(draw, label_font, 10.0F, {content.x + 84.0F, window_min.y + 98.0F},
           kPurpleBright, "C L I P S   •   R E C O R D I N G S   •   R E P L A Y S");

  // Abstract contour lines in the hero keep the top panel polished without
  // importing raster art or affecting the capture/render path.
  const float contour_right = content.x + content_width - 12.0F;
  for (int layer = 0; layer < 7; ++layer) {
    const float inset = static_cast<float>(layer) * 12.0F;
    const ImU32 color = IM_COL32(0, 210, 224, 20 + layer * 2);
    draw->AddLine({contour_right - 240.0F + inset, window_min.y + 20.0F},
                  {contour_right - 112.0F + inset, window_min.y + 20.0F},
                  color, 2.0F);
    draw->AddLine({contour_right - 112.0F + inset, window_min.y + 20.0F},
                  {contour_right - 40.0F + inset, window_min.y + 64.0F},
                  color, 2.0F);
    draw->AddLine({contour_right - 40.0F + inset, window_min.y + 64.0F},
                  {contour_right + 8.0F, window_min.y + 128.0F + inset},
                  color, 2.0F);
  }

  const double requested_seconds = std::max(1.0, config.clip_duration_seconds);
  const double buffered_seconds =
      std::clamp(snapshot.metrics.rolling_buffer_seconds, 0.0, requested_seconds);
  // The actions sit directly beneath the REVO hero banner, matching the compact
  // reference layout while retaining the existing save/start-recording behavior.
  const float hero_y = window_min.y + 132.0F;

  const bool can_save = snapshot.status == CaptureStatus::kBuffering &&
                        !snapshot.finalizing_recording && buffered_seconds >= 1.0;
  char save_label[80]{};
#if defined(KLIP_USE_LIBOBS)
  std::snprintf(save_label, sizeof(save_label), "save up to %.0f seconds", requested_seconds);
#else
  if (buffered_seconds + 0.5 >= requested_seconds) {
    std::snprintf(save_label, sizeof(save_label), "save last %.0f seconds", requested_seconds);
  } else {
    std::snprintf(save_label, sizeof(save_label), "save available %.0f seconds", buffered_seconds);
  }
#endif
  const float action_gap = 20.0F;
  const float action_start_x = content.x;
  const float action_width = (content_width - action_gap) * 0.5F;
  const ImVec2 save_pos{action_start_x, hero_y + 1.0F};
  const ImVec2 action_size{action_width, 94.0F};
  ImGui::SetCursorScreenPos(save_pos);
  if (!can_save) ImGui::BeginDisabled();
  ImGui::InvisibleButton("##save-clip-card", action_size);
  const bool save_clicked = ImGui::IsItemClicked();
  const bool save_hovered = ImGui::IsItemHovered();
  if (!can_save) ImGui::EndDisabled();
  DrawCard(draw, save_pos, action_size, save_hovered && can_save,
           can_save ? IM_COL32(0, 94, 105, 242) : IM_COL32(9, 25, 31, 255),
           can_save ? IM_COL32(17, 223, 230, 235) : kPanelBorder, 9.0F);
  draw->AddCircleFilled({save_pos.x + 42.0F, save_pos.y + 47.0F}, 22.0F,
                        IM_COL32(255, 255, 255, can_save ? 18 : 8));
  DrawClipIcon(draw, {save_pos.x + 42.0F, save_pos.y + 47.0F}, can_save ? kTextU32 : kMutedU32);
  DrawText(draw, semibold, 17.0F, {save_pos.x + 75.0F, save_pos.y + 36.0F},
           can_save ? kTextU32 : kMutedU32, save_label);
  const auto save_hotkey =
      FormatHotkey(config.hotkeys.save_modifiers, config.hotkeys.save_virtual_key);
  const float save_pill_width = TextWidth(label_font, 11.0F, save_hotkey) + 22.0F;
  const ImVec2 save_pill{save_pos.x + action_size.x - save_pill_width - 18.0F, save_pos.y + 32.0F};
  draw->AddRectFilled(save_pill, {save_pill.x + save_pill_width, save_pill.y + 31.0F},
                      IM_COL32(255, 255, 255, 15), 7.0F);
  DrawText(draw, label_font, 11.0F, {save_pill.x + 11.0F, save_pill.y + 9.0F},
           can_save ? IM_COL32(182, 248, 250, 255) : kMutedU32, save_hotkey);
  if (save_clicked && can_save && commands.save_clip) commands.save_clip();

  const bool record_disabled =
      snapshot.finalizing_recording ||
      (!snapshot.recording && snapshot.status != CaptureStatus::kBuffering);
  const ImVec2 record_pos{save_pos.x + action_size.x + action_gap, save_pos.y};
  ImGui::SetCursorScreenPos(record_pos);
  if (record_disabled) ImGui::BeginDisabled();
  ImGui::InvisibleButton("##record-card", action_size);
  const bool record_clicked = ImGui::IsItemClicked();
  const bool record_hovered = ImGui::IsItemHovered();
  if (record_disabled) ImGui::EndDisabled();
  DrawCard(draw, record_pos, action_size, record_hovered && !record_disabled,
           snapshot.recording ? IM_COL32(55, 24, 31, 255) : kPanel,
           snapshot.recording ? kRed : kPanelBorder, 9.0F);
  DrawRecordIcon(draw, {record_pos.x + 42.0F, record_pos.y + 47.0F},
                 snapshot.recording ? kRed : kTextU32);
  const char* record_label = snapshot.recording ? "stop recording" : "start recording";
  DrawText(draw, semibold, 17.0F, {record_pos.x + 75.0F, record_pos.y + 36.0F},
           record_disabled ? kMutedU32 : kTextU32, record_label);
  const auto record_hotkey =
      FormatHotkey(config.hotkeys.record_modifiers, config.hotkeys.record_virtual_key);
  const float record_pill_width = TextWidth(label_font, 11.0F, record_hotkey) + 22.0F;
  const ImVec2 record_pill{record_pos.x + action_size.x - record_pill_width - 18.0F,
                           record_pos.y + 32.0F};
  draw->AddRectFilled(record_pill, {record_pill.x + record_pill_width, record_pill.y + 31.0F},
                      IM_COL32(255, 255, 255, 10), 7.0F);
  DrawText(draw, label_font, 11.0F, {record_pill.x + 11.0F, record_pill.y + 9.0F}, kMutedU32,
           record_hotkey);
  if (record_clicked && !record_disabled && commands.toggle_recording) commands.toggle_recording();
  if (snapshot.recording) {
    char recording_time[32]{};
    std::snprintf(recording_time, sizeof(recording_time), "rec / %02d:%02d",
                  static_cast<int>(snapshot.recording_seconds) / 60,
                  static_cast<int>(snapshot.recording_seconds) % 60);
    DrawText(draw, regular, 12.0F, {record_pos.x + 76.0F, record_pos.y + 61.0F}, kRed,
             recording_time);
  } else if (snapshot.finalizing_recording) {
    DrawText(draw, regular, 12.0F, {record_pos.x + 76.0F, record_pos.y + 61.0F}, kMutedU32,
             "finalizing...");
  }

  const float section_line_y = window_min.y + 247.0F;
  draw->AddLine({window_min.x + 1.0F, section_line_y}, {window_max.x - 1.0F, section_line_y},
                kDivider, 1.0F);
  DrawText(draw, label_font, 11.0F, {content.x, section_line_y + 25.0F}, kMutedU32,
           "capture source / choose one");
#if !defined(KLIP_USE_LIBOBS)
  bool show_capture_preview = config.capture_preview_enabled;
  ImGui::SetCursorScreenPos({content.x + content_width - 390.0F, section_line_y + 17.0F});
  if (ImGui::Checkbox("live preview", &show_capture_preview) &&
      commands.set_capture_preview_enabled) {
    commands.set_capture_preview_enabled(show_capture_preview);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("15 fps dashboard preview. The recording path stays at full quality.");
  bool show_capture_highlight = config.capture_border;
  ImGui::SetCursorScreenPos({content.x + content_width - 224.0F, section_line_y + 17.0F});
  if (ImGui::Checkbox("show capture highlight", &show_capture_highlight) &&
      commands.set_capture_border) {
    commands.set_capture_border(show_capture_highlight);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Keeps the Windows selection border visible. Turning this off does not stop capture.");
#endif

  const bool source_disabled = snapshot.recording || snapshot.finalizing_recording ||
                               snapshot.status == CaptureStatus::kSaving;
  int mode = snapshot.target_mode == CaptureTargetMode::kDisplay ? 1 : 0;
#if defined(KLIP_USE_LIBOBS)
  constexpr const char* modes[] = {"game capture / OBS", "display / OBS"};
#else
  constexpr const char* modes[] = {"game / window", "display"};
#endif
  const float capture_y = section_line_y + 52.0F;
  const float capture_gap = 20.0F;
  const ImVec2 capture_size{(content_width - capture_gap) * 0.5F, 78.0F};
  const ImVec2 mode_pos{content.x, capture_y};
  ImGui::SetCursorScreenPos(mode_pos);
  if (source_disabled) ImGui::BeginDisabled();
  ImGui::InvisibleButton("##mode-card", capture_size);
  const bool mode_clicked = ImGui::IsItemClicked();
  const bool mode_hovered = ImGui::IsItemHovered();
  if (source_disabled) ImGui::EndDisabled();
  if (mode_clicked && !source_disabled) ImGui::OpenPopup("capture-mode-popup");
  DrawCard(draw, mode_pos, capture_size, mode_hovered && !source_disabled);
  DrawMonitorIcon(draw, {mode_pos.x + 42.0F, mode_pos.y + 39.0F},
                  source_disabled ? kMutedU32 : IM_COL32(202, 207, 218, 255));
  DrawText(draw, label_font, 10.0F, {mode_pos.x + 84.0F, mode_pos.y + 21.0F}, kMutedU32, "mode");
  DrawText(draw, semibold, 17.0F, {mode_pos.x + 84.0F, mode_pos.y + 41.0F},
           source_disabled ? kMutedU32 : kTextU32, modes[mode]);
  DrawChevron(draw, {mode_pos.x + capture_size.x - 34.0F, mode_pos.y + 39.0F}, kMutedU32);
  ImGui::SetNextWindowPos({mode_pos.x, mode_pos.y + capture_size.y + 8.0F}, ImGuiCond_Appearing);
  ImGui::SetNextWindowSize({capture_size.x, 108.0F}, ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0F, 8.0F});
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0F);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0.018F, 0.052F, 0.064F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.06F, 0.35F, 0.39F, 1.0F});
  if (ImGui::BeginPopup("capture-mode-popup", ImGuiWindowFlags_NoTitleBar)) {
    for (int option = 0; option < IM_ARRAYSIZE(modes); ++option) {
      ImGui::PushID(option);
      if (PopupOption("##mode-option", modes[option], option == mode) && commands.set_target_mode) {
        commands.set_target_mode(option == 1 ? CaptureTargetMode::kDisplay
                                             : CaptureTargetMode::kGameWindow);
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);

  const auto selected_mode =
      mode == 1 ? CaptureTargetMode::kDisplay : CaptureTargetMode::kGameWindow;
  const auto& sources = mode == 1 ? snapshot.display_sources : snapshot.game_sources;
  const char* selected_label = sources.empty() ? "no compatible source found" : "choose a source";
  for (const auto& source : sources) {
    if (source.id == snapshot.selected_capture_source_id) {
      selected_label = source.label.c_str();
      break;
    }
  }
  // Automatic fullscreen capture uses source id 0, so it will not match an
  // entry in the selectable-window list. Show the active target description
  // instead of incorrectly displaying "choose a source".
  if (snapshot.selected_capture_source_id == 0 &&
      selected_mode == CaptureTargetMode::kGameWindow &&
      !snapshot.capture_target.empty()) {
    selected_label = snapshot.capture_target.c_str();
  }
  const ImVec2 source_pos{mode_pos.x + capture_size.x + capture_gap, capture_y};
  ImGui::SetCursorScreenPos(source_pos);
  if (source_disabled) ImGui::BeginDisabled();
  ImGui::InvisibleButton("##source-card", capture_size);
  const bool source_clicked = ImGui::IsItemClicked();
  const bool source_hovered = ImGui::IsItemHovered();
  if (source_disabled) ImGui::EndDisabled();
  if (source_clicked && !source_disabled && !sources.empty())
    ImGui::OpenPopup("capture-source-popup");
  DrawCard(draw, source_pos, capture_size, source_hovered && !source_disabled);
  DrawControllerIcon(draw, {source_pos.x + 42.0F, source_pos.y + 39.0F},
                     source_disabled ? kMutedU32 : IM_COL32(202, 207, 218, 255));
  DrawText(draw, label_font, 10.0F, {source_pos.x + 84.0F, source_pos.y + 21.0F}, kMutedU32,
           "source");
  const auto compact_source =
      Ellipsize(semibold, 17.0F, CompactSourceLabel(selected_label), capture_size.x - 140.0F);
  DrawText(draw, semibold, 17.0F, {source_pos.x + 84.0F, source_pos.y + 41.0F},
           source_disabled ? kMutedU32 : kTextU32, compact_source);
  DrawChevron(draw, {source_pos.x + capture_size.x - 34.0F, source_pos.y + 39.0F}, kMutedU32);
  const float source_popup_height =
      std::clamp(18.0F + static_cast<float>(sources.size()) * 42.0F, 60.0F, 286.0F);
  ImGui::SetNextWindowPos({source_pos.x, source_pos.y + capture_size.y + 8.0F},
                          ImGuiCond_Appearing);
  ImGui::SetNextWindowSize({capture_size.x, source_popup_height}, ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0F, 8.0F});
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0F);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0.018F, 0.052F, 0.064F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.06F, 0.35F, 0.39F, 1.0F});
  if (ImGui::BeginPopup("capture-source-popup", ImGuiWindowFlags_NoTitleBar)) {
    if (ImGui::IsWindowAppearing() && commands.refresh_capture_sources) commands.refresh_capture_sources();
    for (const auto& source : sources) {
      const bool selected = source.id == snapshot.selected_capture_source_id;
      ImGui::PushID(static_cast<int>(source.id));
      const auto option_label =
          Ellipsize(semibold, 14.0F, CompactSourceLabel(source.label), capture_size.x - 70.0F);
      if (PopupOption("##source-option", option_label, selected) &&
          commands.select_capture_source) {
        commands.select_capture_source(selected_mode, source.id, source.label);
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);

  const float audio_title_y = capture_y + capture_size.y + 22.0F;
  DrawText(draw, label_font, 11.0F, {content.x, audio_title_y + 1.0F}, kMutedU32,
           "audio / live mix");
  constexpr float audio_row_height = 52.0F;
  constexpr float audio_row_gap = 6.0F;
  const float audio_row_y = audio_title_y + 27.0F;
  const ImVec2 audio_row_size{content_width, audio_row_height};
  const float level_x = content.x + 265.0F;
  const float gain_width = std::max(160.0F, content_width - 650.0F);
  DrawCard(draw, {content.x, audio_row_y}, audio_row_size, false, kPanel, kPanelBorder, 7.0F);
  DrawSpeakerIcon(draw, {content.x + 38.0F, audio_row_y + 26.0F}, kMutedU32);
  DrawText(draw, semibold, 13.0F, {content.x + 76.0F, audio_row_y + 18.0F}, kMutedU32,
           "desktop audio");
  float desktop_gain = static_cast<float>(config.desktop_audio_gain);
  if (GainSlider("##desktop-gain", {level_x, audio_row_y + 15.0F}, gain_width, desktop_gain) &&
      commands.set_desktop_audio_gain) {
    desktop_gain_dirty_ = true;
    commands.set_desktop_audio_gain(desktop_gain);
  }
  char desktop_gain_text[24]{};
  std::snprintf(desktop_gain_text, sizeof(desktop_gain_text), "%.1f dB", GainToDb(desktop_gain));
  DrawText(draw, semibold, 12.0F, {level_x + gain_width + 12.0F, audio_row_y + 18.0F}, kTextU32,
           desktop_gain_text);
  bool desktop_enabled = config.desktop_audio_enabled;
  if (Switch("##desktop-audio-toggle", {content.x + content_width - 70.0F, audio_row_y + 13.0F},
             desktop_enabled) &&
      commands.set_desktop_audio_enabled) {
    desktop_enabled = !desktop_enabled;
    commands.set_desktop_audio_enabled(desktop_enabled);
  }

  const float microphone_y = audio_row_y + audio_row_height + audio_row_gap;
  DrawCard(draw, {content.x, microphone_y}, audio_row_size, false, kPanel, kPanelBorder, 7.0F);
  bool microphone_enabled = snapshot.microphone_enabled;
  const char* selected_microphone = "no microphone detected";
  if (snapshot.selected_microphone_index >= 0 &&
      snapshot.selected_microphone_index < static_cast<int>(snapshot.microphones.size()))
    selected_microphone =
        snapshot.microphones[static_cast<std::size_t>(snapshot.selected_microphone_index)].c_str();
  std::string_view microphone_name = selected_microphone;
  constexpr std::string_view microphone_prefix = "Microphone (";
  if (microphone_name.starts_with(microphone_prefix) && microphone_name.ends_with(')')) {
    microphone_name.remove_prefix(microphone_prefix.size());
    microphone_name.remove_suffix(1);
  }
  DrawMicrophoneIcon(draw, {content.x + 38.0F, microphone_y + 24.0F},
                     snapshot.microphone_active ? kGreen : kPurpleBright);
  ImGui::SetCursorScreenPos({content.x + 76.0F, microphone_y + 7.0F});
  ImGui::InvisibleButton("##microphone-input", {174.0F, 39.0F});
  const bool input_clicked = ImGui::IsItemClicked();
  const bool input_hovered = ImGui::IsItemHovered();
  if (input_hovered) ImGui::SetTooltip("Choose microphone input");
  if (input_clicked && !snapshot.microphones.empty()) ImGui::OpenPopup("microphone-popup");
  DrawText(draw, label_font, 9.0F, {content.x + 76.0F, microphone_y + 7.0F}, kMutedU32,
           "microphone / input");
  const auto mic_label = Ellipsize(semibold, 12.0F, microphone_name, 166.0F);
  DrawText(draw, semibold, 12.0F, {content.x + 76.0F, microphone_y + 21.0F},
           microphone_enabled ? kTextU32 : kMutedU32, mic_label);
  float microphone_gain = static_cast<float>(config.microphone_audio_gain);
  if (GainSlider("##microphone-gain", {level_x, microphone_y + 15.0F}, gain_width,
                 microphone_gain) &&
      commands.set_microphone_audio_gain) {
    microphone_gain_dirty_ = true;
    commands.set_microphone_audio_gain(microphone_gain);
  }
  char microphone_gain_text[24]{};
  std::snprintf(microphone_gain_text, sizeof(microphone_gain_text), "%.1f dB",
                GainToDb(microphone_gain));
  DrawText(draw, semibold, 12.0F, {level_x + gain_width + 12.0F, microphone_y + 18.0F},
           microphone_enabled ? kTextU32 : kMutedU32, microphone_gain_text);
  if (Switch("##microphone-toggle", {content.x + content_width - 70.0F, microphone_y + 13.0F},
             microphone_enabled) &&
      commands.set_microphone_enabled) {
    microphone_enabled = !microphone_enabled;
    commands.set_microphone_enabled(microphone_enabled);
  }
  const float discord_y = microphone_y + audio_row_height + audio_row_gap;
  DrawCard(draw, {content.x, discord_y}, audio_row_size, false, kPanel, kPanelBorder, 7.0F);
  DrawSpeakerIcon(draw, {content.x + 38.0F, discord_y + 26.0F},
                  config.discord_audio_window.empty() ? kMutedU32 : kPurpleBright);
  DrawText(draw, semibold, 13.0F, {content.x + 76.0F, discord_y + 18.0F},
           config.discord_audio_window.empty() ? kMutedU32 : kTextU32, "Discord audio");
  float discord_gain = static_cast<float>(config.discord_audio_gain);
  if (GainSlider("##discord-gain", {level_x, discord_y + 15.0F}, gain_width, discord_gain) &&
      commands.set_discord_audio_gain) {
    microphone_gain_dirty_ = true;
    commands.set_discord_audio_gain(discord_gain);
  }
  char discord_gain_text[24]{};
  std::snprintf(discord_gain_text, sizeof(discord_gain_text), "%.1f dB", GainToDb(discord_gain));
  DrawText(draw, semibold, 12.0F, {level_x + gain_width + 12.0F, discord_y + 18.0F},
           config.discord_audio_window.empty() ? kMutedU32 : kTextU32, discord_gain_text);
  if (ImGui::IsItemHovered() && config.discord_audio_window.empty())
    ImGui::SetTooltip("Select Discord's executable in Settings > Audio to enable this source.");

  const float music_y = discord_y + audio_row_height + audio_row_gap;
  DrawCard(draw, {content.x, music_y}, audio_row_size, false, kPanel, kPanelBorder, 7.0F);
  DrawSpeakerIcon(draw, {content.x + 38.0F, music_y + 26.0F},
                  config.music_audio_window.empty() ? kMutedU32 : kPurpleBright);
  DrawText(draw, semibold, 13.0F, {content.x + 76.0F, music_y + 18.0F},
           config.music_audio_window.empty() ? kMutedU32 : kTextU32, "Spotify audio");
  float music_gain = static_cast<float>(config.music_audio_gain);
  if (GainSlider("##spotify-gain", {level_x, music_y + 15.0F}, gain_width, music_gain) &&
      commands.set_music_audio_gain) {
    microphone_gain_dirty_ = true;
    commands.set_music_audio_gain(music_gain);
  }
  char music_gain_text[24]{};
  std::snprintf(music_gain_text, sizeof(music_gain_text), "%.1f dB", GainToDb(music_gain));
  DrawText(draw, semibold, 12.0F, {level_x + gain_width + 12.0F, music_y + 18.0F},
           config.music_audio_window.empty() ? kMutedU32 : kTextU32, music_gain_text);
  if (ImGui::IsItemHovered() && config.music_audio_window.empty())
    ImGui::SetTooltip("Select Spotify's executable in Settings > Audio to enable this source.");

  if ((desktop_gain_dirty_ || microphone_gain_dirty_) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Left) && commands.persist_audio_gains) {
    commands.persist_audio_gains();
    desktop_gain_dirty_ = false;
    microphone_gain_dirty_ = false;
  }

  const float microphone_popup_height =
      std::clamp(18.0F + static_cast<float>(snapshot.microphones.size()) * 42.0F, 60.0F, 244.0F);
  ImGui::SetNextWindowPos({content.x + 76.0F, microphone_y + audio_row_size.y + 8.0F},
                          ImGuiCond_Appearing);
  ImGui::SetNextWindowSize({std::min(420.0F, content_width - 100.0F), microphone_popup_height},
                           ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0F, 8.0F});
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0F);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0.052F, 0.055F, 0.063F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.29F, 0.23F, 0.36F, 1.0F});
  if (ImGui::BeginPopup("microphone-popup", ImGuiWindowFlags_NoTitleBar)) {
    if (ImGui::IsWindowAppearing() && commands.refresh_capture_sources) commands.refresh_capture_sources();
    for (int index = 0; index < static_cast<int>(snapshot.microphones.size()); ++index) {
      const bool selected = index == snapshot.selected_microphone_index;
      ImGui::PushID(index);
      const auto microphone_option =
          Ellipsize(semibold, 14.0F, snapshot.microphones[static_cast<std::size_t>(index)],
                    std::min(420.0F, content_width - 140.0F));
      if (PopupOption("##microphone-option", microphone_option, selected) &&
          commands.select_microphone) {
        commands.select_microphone(index);
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);

  // Anchor status below the final mixer row (Spotify), not the microphone row.
  // The mixer now has four rows; using microphone_y caused the status panel to
  // overlap Discord and Spotify as the dashboard grew.
  const float system_line_y = music_y + audio_row_height + 20.0F;
  draw->AddLine({window_min.x + 1.0F, system_line_y}, {window_max.x - 1.0F, system_line_y},
                kDivider, 1.0F);
  DrawText(draw, label_font, 11.0F, {content.x, system_line_y + 22.0F}, kMutedU32,
           "system status / capture health");
  const float metric_y = system_line_y + 49.0F;
  constexpr float metric_width = 190.0F;
#if defined(KLIP_USE_LIBOBS)
  const char* metric_labels[] = {"OBS render fps", "encoder", "replay memory cap", "render lagged"};
#else
  const char* metric_labels[] = {"output fps", "encoder", "buffer ram", "frames skipped"};
#endif
  char fps_value[24]{};
  char buffer_value[24]{};
  char dropped_value[24]{};
  std::snprintf(fps_value, sizeof(fps_value), "%.1f", snapshot.metrics.capture_fps);
  std::snprintf(buffer_value, sizeof(buffer_value), "%.0f MiB",
#if defined(KLIP_USE_LIBOBS)
                static_cast<double>(config.rolling_buffer_bytes) / (1024.0 * 1024.0));
#else
                static_cast<double>(snapshot.metrics.rolling_buffer_bytes) / (1024.0 * 1024.0));
#endif
  std::snprintf(dropped_value, sizeof(dropped_value), "%llu",
                static_cast<unsigned long long>(snapshot.metrics.dropped_raw_frames +
                                                snapshot.metrics.dropped_encode_frames));
  const char* metric_values[] = {
      fps_value,
      snapshot.selected_encoder.empty() ? "warming up" : snapshot.selected_encoder.c_str(),
      buffer_value, dropped_value};
  for (int metric = 0; metric < 4; ++metric) {
    const float x = content.x + metric * metric_width;
    DrawText(draw, label_font, 9.0F, {x, metric_y}, kMutedU32, metric_labels[metric]);
    DrawText(draw, semibold, 20.0F, {x, metric_y + 24.0F}, kTextU32, metric_values[metric]);
    if (metric < 3)
      draw->AddLine({x + metric_width - 26.0F, metric_y - 4.0F},
                    {x + metric_width - 26.0F, metric_y + 56.0F}, kDivider, 1.0F);
  }
  char source_fps_value[40]{};
#if defined(KLIP_USE_LIBOBS)
  std::snprintf(source_fps_value, sizeof(source_fps_value), "source cadence / inspect decoded clips");
#else
  std::snprintf(source_fps_value, sizeof(source_fps_value), "WGC arrivals %.1f / sec",
                snapshot.metrics.source_fps);
#endif
  DrawText(draw, label_font, 9.0F, {content.x, metric_y + 51.0F}, kMutedU32, source_fps_value);
  if (!snapshot.encoder_status.empty()) {
#if defined(KLIP_USE_LIBOBS)
    const char* encoder_status = !config.obs_replay_enabled ? "buffering disabled"
        : snapshot.encoder_status.find("fallback") != std::string::npos ? "capture fallback / hover for cause"
        : snapshot.selected_encoder == "obs_x264" ? "software encoding / hover for details"
        : "OBS encoder / hover for settings";
#else
    const auto status_view = std::string_view(snapshot.encoder_status);
    const char* encoder_status =
        status_view.find("fallback") != std::string_view::npos ? "fallback active / hover for cause"
        : status_view.starts_with("Checking") ? "checking encoders"
                                              : "encoder retry / hover for details";
#endif
    DrawText(draw, label_font, 9.0F, {content.x + metric_width, metric_y + 51.0F}, kWarning,
             encoder_status);
  }
  ImGui::SetCursorScreenPos({content.x, metric_y - 4.0F});
  ImGui::InvisibleButton("##wgc-arrivals-help", {metric_width - 26.0F, 62.0F});
  if (ImGui::IsItemHovered())
#if defined(KLIP_USE_LIBOBS)
    ImGui::SetTooltip("OBS video-render FPS is not a count of unique captured images. Inspect decoded frame IDs or game motion to verify source cadence.");
#else
    ImGui::SetTooltip(
        "Windows capture updates. Static content can update less often; output "
        "FPS may hold the last image between updates.");
#endif
  ImGui::SetCursorScreenPos({content.x + metric_width, metric_y - 4.0F});
  ImGui::InvisibleButton("##encoder-status-help", {metric_width - 26.0F, 62.0F});
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0F);
    ImGui::TextUnformatted(snapshot.encoder_status.empty() ? "Preferred video encoder is active."
                                                           : snapshot.encoder_status.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::SetCursorScreenPos({content.x + metric_width * 3.0F, metric_y - 4.0F});
  ImGui::InvisibleButton("##frame-skips-help", {metric_width - 26.0F, 62.0F});
  if (ImGui::IsItemHovered())
#if defined(KLIP_USE_LIBOBS)
    ImGui::SetTooltip("OBS video-render frames that missed their scheduled deadline. Encoder-skipped frames are reported separately in diagnostics and the OBS log.");
#else
    ImGui::SetTooltip(
        "Frames skipped or coalesced to keep capture current instead of building delay.");
#endif

  const ImVec2 folder_pos{content.x + content_width - 250.0F, metric_y - 4.0F};
  const ImVec2 folder_size{250.0F, 52.0F};
  ImGui::SetCursorScreenPos(folder_pos);
  ImGui::InvisibleButton("##open-clips-folder", folder_size);
  const bool folder_clicked = ImGui::IsItemClicked();
  const bool folder_hovered = ImGui::IsItemHovered();
  DrawCard(draw, folder_pos, folder_size, folder_hovered, IM_COL32(5, 25, 31, 255),
           IM_COL32(10, 211, 222, 230), 7.0F);
  DrawFolderIcon(draw, {folder_pos.x + 34.0F, folder_pos.y + 26.0F}, kPurpleBright);
  DrawText(draw, semibold, 14.0F, {folder_pos.x + 59.0F, folder_pos.y + 17.0F}, kPurpleBright,
           "open clips folder");
  if (folder_clicked && commands.open_output_folder) commands.open_output_folder();

  const auto hotkey_copy =
      hotkeys_available
          ? FormatHotkey(config.hotkeys.record_modifiers, config.hotkeys.record_virtual_key) +
                " record   /   " +
                FormatHotkey(config.hotkeys.save_modifiers, config.hotkeys.save_virtual_key) +
                " clip   /   " +
                FormatHotkey(config.hotkeys.toggle_ui_modifiers,
                             config.hotkeys.toggle_ui_virtual_key) +
                " hide"
          : std::string("global hotkeys unavailable");
  const float hotkey_width = TextWidth(label_font, 10.0F, hotkey_copy);
  if (snapshot.last_error.has_value()) {
    const ImVec2 error_pos{content.x, metric_y + 61.0F};
    const ImVec2 error_size{content_width, 45.0F};
    ImGui::SetCursorScreenPos(error_pos);
    ImGui::InvisibleButton("##capture-error-card", error_size);
    DrawCard(draw, error_pos, error_size, false, IM_COL32(62, 25, 32, 245),
             IM_COL32(161, 58, 70, 220), 7.0F);
    const auto issue = snapshot.last_error->operation + ": " + snapshot.last_error->message;
    const auto compact_issue = Ellipsize(regular, 11.0F, issue, content_width - 212.0F);
    DrawText(draw, regular, 11.0F, {error_pos.x + 12.0F, error_pos.y + 16.0F}, kRed, compact_issue);
    ImGui::SetCursorScreenPos({error_pos.x + content_width - 190.0F, error_pos.y + 7.0F});
    const bool details_clicked = ImGui::Button("details", {82.0F, 31.0F});
    ImGui::SameLine();
    const bool copy_clicked = ImGui::Button("copy", {82.0F, 31.0F});
    if (details_clicked) ImGui::OpenPopup("capture-error-details");
    if (copy_clicked) {
      diagnostics_preview_open_ = true;
      diagnostics_include_private_details_ = false;
      if (!settings_open_) {
        ResetDraft(config);
        settings_open_ = true;
          }
    }
    if (ImGui::BeginPopup("capture-error-details")) {
      ImGui::TextColored(kCoral, "%s / %s", ComponentName(snapshot.last_error->component).c_str(),
                         snapshot.last_error->operation.c_str());
      ImGui::Separator();
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(680.0F, content_width - 48.0F));
      ImGui::TextUnformatted(snapshot.last_error->ToString().c_str());
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      ImGui::TextDisabled(
          "Copied diagnostics may contain device or file names; review before sharing.");
      ImGui::EndPopup();
    }
  } else {
    DrawText(draw, label_font, 10.0F, {content.x + content_width - hotkey_width, metric_y + 60.0F},
             kMutedU32, hotkey_copy);
  }

  const auto now = std::chrono::steady_clock::now();
  if (snapshot.last_saved_clip != last_clip_seen_) {
    last_clip_seen_ = snapshot.last_saved_clip;
    if (!last_clip_seen_.empty()) clip_toast_until_ = now + std::chrono::seconds(6);
  }
  if (now < clip_toast_until_) {
    ImGui::SetNextWindowPos({window_max.x - 352.0F, window_min.y + 10.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96F);
    if (ImGui::Begin("clip saved###revo-clip-saved", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImGui::TextColored(kMint, "clip saved");
      const auto clip_filename = PathToUtf8(last_clip_seen_.filename());
      ImGui::TextUnformatted(clip_filename.c_str());
      if (ImGui::Button("open clip") && commands.open_file) commands.open_file(last_clip_seen_);
      ImGui::SameLine();
      if (ImGui::Button("show folder") && commands.open_output_folder)
        commands.open_output_folder();
    }
    ImGui::End();
  } else if (now < diagnostics_copied_until_) {
    ImGui::SetNextWindowPos({window_max.x - 270.0F, window_min.y + 10.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96F);
    if (ImGui::Begin("diagnostics copied###revo-diagnostics-copied", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
      ImGui::TextColored(kMint, "diagnostics copied / review before sharing");
    ImGui::End();
  }


}

void MainPanel::RenderSettings(const ApplicationSnapshot& snapshot, const AppConfig& config,
                               const UiCommands& commands) {
  // Keep the visible game-audio selector in sync with the executable detected by the engine.
  // The UI draft is otherwise a separate copy and would continue showing stale javaw.exe.
  if (!snapshot.detected_game_audio_application.empty()) {
    draft_.game_audio_window = "::" + snapshot.detected_game_audio_application;
  }

  if (!draft_initialized_) ResetDraft(config);
  const ImVec2 settings_window_size = ImGui::GetWindowSize();
  const ImVec2 settings_window_origin = ImGui::GetWindowPos();
  auto* settings_draw = ImGui::GetWindowDrawList();
  settings_draw->AddRectFilled(
      settings_window_origin,
      {settings_window_origin.x + settings_window_size.x,
       settings_window_origin.y + settings_window_size.y},
      kBackgroundBottom);

  ImGui::SetCursorPos({22.0F, 18.0F});
  if (ImGui::Button("< home", {98.0F, 36.0F})) {
    ResetDraft(config);
    settings_open_ = false;
    diagnostics_preview_open_ = false;
    return;
  }
  DrawLogo(ImGui::GetWindowDrawList(),
           {settings_window_origin.x + 144.5F, settings_window_origin.y + 32.0F},
           0.56F);
  ImGui::SameLine(0.0F, 46.0F);
  ImGui::PushFont(UiFont(2));
  ImGui::TextUnformatted("REVO / SETTINGS");
  ImGui::PopFont();
  ImGui::SameLine(0.0F, 18.0F);
  ImGui::TextColored(kMuted, "All controls on one scrollable page");

  ImGui::SetCursorPos({22.0F, 68.0F});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0F, 16.0F});
  ImGui::BeginChild("##settings-scroll",
                    {std::max(320.0F, settings_window_size.x - 44.0F),
                     std::max(200.0F, settings_window_size.y - 176.0F)},
                    false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
  ImGui::PopStyleVar();

  if (diagnostics_preview_open_) {
    RenderDiagnosticsPreview(snapshot, config);
  } else {

    {
      SectionLabel("compatibility / system checks");
      RenderCompatibility(snapshot, config, commands);
    }

    {
      SectionLabel("video / encoder and quality");
      SectionLabel("performance / preset");
      bool performance_mode = IsPerformanceMode(draft_);
      if (ImGui::Checkbox("performance mode", &performance_mode))
        ApplyPerformanceMode(draft_, performance_mode);
      ImGui::TextColored(
#if defined(KLIP_USE_LIBOBS)
          kMuted, "OBS encoder preset only; your resolution, FPS, and CQ stay selected.");
      ImGui::Checkbox("enable replay buffering", &draft_.obs_replay_enabled);
#else
          kMuted, performance_mode
                      ? "active / 720p60, 8 Mbps, fastest hardware preset, automatic GPU"
                      : "off / recommended quality uses 1080p60, 12 Mbps, balanced hardware");
#endif

      SectionLabel("video / quality");
      int fps = draft_.target_fps == 30 ? 0 : draft_.target_fps == 60 ? 1 : draft_.target_fps == 120 ? 2 : 3;
      constexpr const char* frame_rates[] = {"30 fps / lightest", "60 fps / recommended",
                                             "120 fps / high load", "240 fps / very high load"};
      if (ImGui::Combo("frame rate", &fps, frame_rates, IM_ARRAYSIZE(frame_rates)))
        draft_.target_fps = fps == 0 ? 30U : fps == 1 ? 60U : fps == 2 ? 120U : 240U;
      if (draft_.target_fps >= 120)
        ImGui::TextColored(
            kMuted,
            "Use a source and display pipeline that can sustain this rate. Hardware encoding is strongly recommended; 240 fps at high resolution is very demanding.");

      int resolution = draft_.output_width == 1280 ? 2 : draft_.output_width == 1920 ? 1 : 0;
      constexpr const char* resolutions[] = {"source resolution", "1920 x 1080", "1280 x 720"};
      if (ImGui::Combo("output size", &resolution, resolutions, IM_ARRAYSIZE(resolutions))) {
        draft_.output_width = resolution == 1 ? 1920U : resolution == 2 ? 1280U : 0U;
        draft_.output_height = resolution == 1 ? 1080U : resolution == 2 ? 720U : 0U;
      }
      int scaling = draft_.scaling_mode == VideoScalingMode::kFit ? 0 : 1;
      constexpr const char* scaling_modes[] = {"fit / preserve aspect ratio",
                                               "stretch / fill output"};
      if (ImGui::Combo("scaling", &scaling, scaling_modes, IM_ARRAYSIZE(scaling_modes)))
        draft_.scaling_mode = scaling == 0 ? VideoScalingMode::kFit : VideoScalingMode::kStretch;

#if defined(KLIP_USE_LIBOBS)
      ImGui::Checkbox("prioritize full replay duration", &draft_.obs_duration_priority);
      if (draft_.obs_duration_priority) {
        ImGui::SliderInt("target video bitrate / kbps", &draft_.obs_replay_bitrate_kbps, 1000, 60000);
        ImGui::TextColored(kMuted, "Effective video bitrate: %.1f Mbps (fits duration, audio tracks and memory).",
                          ObsReplayBitrateKbps(draft_) / 1000.0);
        ImGui::TextColored(kMuted, "Duration-first uses bounded bitrate for clips AND recordings. Detail may be lower in complex scenes.");
      } else {
        ImGui::SliderInt("CQ / lower is higher quality", &draft_.obs_cq, 1, 51);
        ImGui::TextColored(kMuted, "Constant quality may shorten clips when replay memory fills.");
      }
      if (ImGui::BeginCombo("OBS encoder", draft_.obs_encoder_id.c_str())) {
        if (ImGui::Selectable("auto / detected hardware", draft_.obs_encoder_id == "auto"))
          draft_.obs_encoder_id = "auto";
        for (const auto& id : snapshot.available_encoders) {
          const char* label = id.c_str();
          if (id == "obs_nvenc_h264_tex") label = "NVIDIA NVENC H.264";
          else if (id == "obs_nvenc_hevc_tex") label = "NVIDIA NVENC HEVC (Minecraft only)";
          else if (id == "h264_texture_amf") label = "AMD AMF H.264";
          else if (id == "obs_qsv11_v2") label = "Intel QSV H.264";
          else if (id == "obs_x264") label = "Software x264";
          if (ImGui::Selectable(label, draft_.obs_encoder_id == id))
            draft_.obs_encoder_id = id;
        }
        ImGui::EndCombo();
      }
      ImGui::TextWrapped(
          "Automatic codec policy: H.264 for all normal apps and games; NVIDIA NVENC HEVC only when javaw.exe or Minecraft for Windows is detected. It returns to H.264 when Minecraft exits.");
      ImGui::TextColored(kMuted,
          "If a manual recording is active, codec switching waits until it stops so the recording file is not interrupted.");
#else
      int bitrate_mbps = static_cast<int>(draft_.video_bitrate / 1'000'000);
      if (ImGui::SliderInt("video bitrate", &bitrate_mbps, 4, 40, "%d Mbps")) {
        draft_.video_bitrate = static_cast<std::int64_t>(bitrate_mbps) * 1'000'000;
        ResizeReplayBudget(draft_);
      }
      int encoder = EncoderProfile(draft_.encoder_preferences);
      constexpr const char* encoders[] = {"auto / recommended", "NVIDIA NVENC", "AMD AMF",
                                          "Windows hardware / Intel + fallback"};
      if (ImGui::Combo("encoder", &encoder, encoders, IM_ARRAYSIZE(encoders)))
        SetEncoderProfile(draft_, encoder);
#endif
      int quality = static_cast<int>(draft_.encoder_quality);
      constexpr const char* qualities[] = {"performance", "balanced", "maximum quality"};
      if (ImGui::Combo("encoder load", &quality, qualities, IM_ARRAYSIZE(qualities)))
        draft_.encoder_quality = static_cast<EncoderQuality>(quality);
#if defined(KLIP_USE_LIBOBS)
      if (draft_.encoder_quality == EncoderQuality::kPerformance)
        ImGui::TextWrapped("performance uses lighter encoder tuning. image quality and file size may differ; resolution, fps and cq stay unchanged.");
#endif
    }

    {
      SectionLabel("capture / source and overlays");
#if defined(KLIP_USE_LIBOBS)
      constexpr const char* methods[] = {"OBS automatic", "OBS DXGI duplication", "OBS Windows Graphics Capture"};
      ImGui::Combo("display capture method", &draft_.obs_display_method, methods, 3);
      ImGui::Checkbox("reduce game capture copies", &draft_.obs_limit_game_capture_fps);
      ImGui::TextWrapped("Limits OBS game-capture copies to clip FPS, not game FPS. Quality settings stay unchanged; disable if motion looks less smooth.");
#endif
      ImGui::Checkbox("capture mouse cursor", &draft_.capture_cursor);
#if !defined(KLIP_USE_LIBOBS)
      ImGui::Checkbox("show Windows capture highlight", &draft_.capture_border);
      ImGui::Checkbox("show low-rate capture preview", &draft_.capture_preview_enabled);
      ImGui::TextColored(kMuted,
                         "preview refreshes at 15 fps and does not change recorded frame rate.");
#else
      ImGui::TextWrapped("OBS game capture is borderless. DXGI display capture is preferred to avoid the yellow Windows Graphics Capture border.");
#endif

      SectionLabel("studio / map cover or handcam");
      if (ImGui::Checkbox("static image overlay", &draft_.static_overlay_enabled) &&
          draft_.static_overlay_enabled)
        draft_.live_overlay_enabled = false;
      ImGui::BeginDisabled(!draft_.static_overlay_enabled);
      ImGui::InputText("image file", overlay_path_.data(), overlay_path_.size());
      ImGui::EndDisabled();

      if (ImGui::Checkbox("live window overlay / handcam", &draft_.live_overlay_enabled) &&
          draft_.live_overlay_enabled) {
        draft_.static_overlay_enabled = false;
#if defined(KLIP_USE_LIBOBS)
        const auto& overlay_sources = snapshot.overlay_sources;
#else
        const auto& overlay_sources = snapshot.game_sources;
#endif
        if (draft_.live_overlay_window_title.empty()) {
          const auto first = std::find_if(
              overlay_sources.begin(), overlay_sources.end(),
              [&](const auto& source) { return source.id != snapshot.selected_capture_source_id; });
          if (first != overlay_sources.end()) draft_.live_overlay_window_title = first->label;
        }
      }
      const char* live_overlay_label = draft_.live_overlay_window_title.empty()
                                           ? "open Camera or another preview window first"
                                           : draft_.live_overlay_window_title.c_str();
      ImGui::BeginDisabled(!draft_.live_overlay_enabled);
      if (ImGui::BeginCombo("handcam window", live_overlay_label)) {
        if (ImGui::IsWindowAppearing() && commands.refresh_capture_sources) commands.refresh_capture_sources();
#if defined(KLIP_USE_LIBOBS)
        const auto& overlay_sources = snapshot.overlay_sources;
#else
        const auto& overlay_sources = snapshot.game_sources;
#endif
        for (const auto& source : overlay_sources) {
          if (source.id == snapshot.selected_capture_source_id) continue;
          const bool selected = source.label == draft_.live_overlay_window_title;
          if (ImGui::Selectable(source.label.c_str(), selected))
            draft_.live_overlay_window_title = source.label;
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();

      const bool overlay_enabled = draft_.static_overlay_enabled || draft_.live_overlay_enabled;
      ImGui::BeginDisabled(!overlay_enabled);
      int overlay_x = static_cast<int>(draft_.static_overlay_x * 100.0 + 0.5);
      int overlay_y = static_cast<int>(draft_.static_overlay_y * 100.0 + 0.5);
      int overlay_width = static_cast<int>(draft_.static_overlay_width * 100.0 + 0.5);
      int overlay_height = static_cast<int>(draft_.static_overlay_height * 100.0 + 0.5);
      int overlay_opacity = static_cast<int>(draft_.static_overlay_opacity * 100.0 + 0.5);
      bool overlay_changed = false;
      overlay_changed |= ImGui::SliderInt("left", &overlay_x, 0, 99, "%d%%");
      overlay_changed |= ImGui::SliderInt("top", &overlay_y, 0, 99, "%d%%");
      overlay_changed |= ImGui::SliderInt("width", &overlay_width, 1, 100, "%d%%");
      overlay_changed |= ImGui::SliderInt("height", &overlay_height, 1, 100, "%d%%");
      overlay_changed |= ImGui::SliderInt("opacity", &overlay_opacity, 0, 100, "%d%%");
      if (overlay_changed) {
        overlay_x = std::min(overlay_x, 100 - overlay_width);
        overlay_y = std::min(overlay_y, 100 - overlay_height);
        draft_.static_overlay_x = static_cast<double>(overlay_x) / 100.0;
        draft_.static_overlay_y = static_cast<double>(overlay_y) / 100.0;
        draft_.static_overlay_width = static_cast<double>(overlay_width) / 100.0;
        draft_.static_overlay_height = static_cast<double>(overlay_height) / 100.0;
        draft_.static_overlay_opacity = static_cast<double>(overlay_opacity) / 100.0;
      }
      ImGui::EndDisabled();
#if defined(KLIP_USE_LIBOBS)
      ImGui::TextWrapped("One image or window is composed by OBS before encoding, so it is included in both replays and recordings. Open Camera for a handcam source.");
#else
      ImGui::TextColored(kMuted,
                         "Open Windows Camera for a webcam source. One overlay is composed in the "
                         "existing GPU pass and applies when you save.");
#endif
    }

    {
      SectionLabel("audio / devices and routing");
      SectionLabel("clips / audio");
      ImGui::Checkbox("enable desktop audio track", &draft_.desktop_audio_enabled);
#if defined(KLIP_USE_LIBOBS)
      ImGui::TextColored(kMuted, "Five separate audio tracks are always enabled.");
      ImGui::TextColored(kMuted, "Tracks: 1 desktop + mic / 2 mic / 3 game / 4 Discord / 5 Spotify");
      DrawAudioWindowSelector("game audio application", draft_.game_audio_window);
      DrawAudioWindowSelector("Discord audio application", draft_.discord_audio_window);
      DrawAudioWindowSelector("music audio application", draft_.music_audio_window);
      constexpr int durations[] = {15, 30, 60, 120};
      constexpr const char* duration_names[] = {"15 seconds", "30 seconds", "60 seconds", "120 seconds"};
      int duration_index = -1;
      for (int i = 0; i < 4; ++i) if (static_cast<int>(draft_.clip_duration_seconds) == durations[i]) duration_index = i;
      char duration_label[40]{};
      std::snprintf(duration_label, sizeof(duration_label), "%.0f seconds%s", draft_.clip_duration_seconds,
                    duration_index == -1 ? " / custom" : "");
      if (ImGui::BeginCombo("replay length", duration_label)) {
        for (int i = 0; i < 4; ++i) {
          if (ImGui::Selectable(duration_names[i], duration_index == i)) {
            draft_.clip_duration_seconds = durations[i];
            ResizeReplayBudget(draft_);
          }
        }
        ImGui::EndCombo();
      }
      ImGui::TextColored(kMuted, "OBS replay memory cap: %.0f MiB. Duration-first limits bitrate to fit; constant quality may shorten clips.",
                         static_cast<double>(draft_.rolling_buffer_bytes) / (1024.0 * 1024.0));
#else
      int clip_seconds = static_cast<int>(draft_.clip_duration_seconds);
      if (ImGui::SliderInt("replay length", &clip_seconds, 15, 300, "%d seconds")) {
        draft_.clip_duration_seconds = static_cast<double>(clip_seconds);
        ResizeReplayBudget(draft_);
      }
#endif
      int audio_bitrate = static_cast<int>(draft_.audio_bitrate / 1000);
      if (ImGui::SliderInt("audio bitrate", &audio_bitrate, 96, 320, "%d kbps")) {
        draft_.audio_bitrate = static_cast<std::int64_t>(audio_bitrate) * 1000;
        ResizeReplayBudget(draft_);
      }
      int desktop_volume_db = static_cast<int>(std::lround(GainToDb(
          static_cast<float>(draft_.desktop_audio_gain))));
      if (ImGui::SliderInt("desktop gain", &desktop_volume_db, -60, 6, "%d dB"))
        draft_.desktop_audio_gain = static_cast<double>(DbToGain(
            static_cast<float>(desktop_volume_db)));
      int game_volume_db = static_cast<int>(std::lround(GainToDb(
          static_cast<float>(draft_.game_audio_gain))));
      if (ImGui::SliderInt("game gain", &game_volume_db, -60, 6, "%d dB"))
        draft_.game_audio_gain = static_cast<double>(DbToGain(
            static_cast<float>(game_volume_db)));
      int discord_volume_db = static_cast<int>(std::lround(GainToDb(
          static_cast<float>(draft_.discord_audio_gain))));
      if (ImGui::SliderInt("Discord gain", &discord_volume_db, -60, 6, "%d dB"))
        draft_.discord_audio_gain = static_cast<double>(DbToGain(
            static_cast<float>(discord_volume_db)));
      int music_volume_db = static_cast<int>(std::lround(GainToDb(
          static_cast<float>(draft_.music_audio_gain))));
      if (ImGui::SliderInt("Spotify gain", &music_volume_db, -60, 6, "%d dB"))
        draft_.music_audio_gain = static_cast<double>(DbToGain(
            static_cast<float>(music_volume_db)));

#if defined(KLIP_USE_LIBOBS)
      ImGui::TextWrapped("Track 1 mixes all Windows output, microphone, and Spotify; track 2 is microphone-only. Tracks 3–5 isolate game, Discord, and Spotify. Desktop audio follows the default Windows playback device, while Spotify is also routed into Track 1.");
      if (!draft_.excluded_audio_process.empty()) {
        ImGui::TextWrapped("Legacy per-app exclusion is ignored: desktop audio now records all Windows output.");
        if (ImGui::Button("clear legacy exclusion setting")) draft_.excluded_audio_process.clear();
      }
#else
      const std::string excluded_label = draft_.excluded_audio_process.empty()
                                             ? "record every application"
                                             : "mute " + draft_.excluded_audio_process;
      if (ImGui::BeginCombo("application audio", excluded_label.c_str())) {
        const bool all_selected = draft_.excluded_audio_process.empty();
        if (ImGui::Selectable("record every application", all_selected))
          draft_.excluded_audio_process.clear();
        for (const auto& application : snapshot.audio_applications) {
          const bool selected =
              _stricmp(draft_.excluded_audio_process.c_str(), application.name.c_str()) == 0;
          const auto label = "mute " + application.name;
          if (ImGui::Selectable(label.c_str(), selected))
            draft_.excluded_audio_process = application.name;
        }
        ImGui::EndCombo();
      }
      ImGui::TextColored(kMuted,
                         "low-overhead process-tree exclusion; it does not mute playback for you.");
#endif
      int microphone_volume_db = static_cast<int>(std::lround(GainToDb(
          static_cast<float>(draft_.microphone_audio_gain))));
      if (ImGui::SliderInt("microphone gain", &microphone_volume_db, -60, 6, "%d dB"))
        draft_.microphone_audio_gain = static_cast<double>(DbToGain(
            static_cast<float>(microphone_volume_db)));
      ImGui::Checkbox("enable microphone (voice / keyboard clicks) on launch",
                      &draft_.microphone_enabled);
    }

    {
      SectionLabel("shortcuts / global keybinds");
      ImGui::TextColored(kMuted,
                         "Choose any key or key combination for each global shortcut.");
      const char* shortcut_labels[] = {"save replay 1", "save replay 2", "save replay 3",
                                          "start / stop recording 1", "start / stop recording 2",
                                          "start / stop recording 3", "show / hide REVO"};
      unsigned int* shortcut_modifiers[] = {
          &draft_.hotkeys.save_modifiers, &draft_.hotkeys.save2_modifiers,
          &draft_.hotkeys.save3_modifiers, &draft_.hotkeys.record_modifiers,
          &draft_.hotkeys.record2_modifiers, &draft_.hotkeys.record3_modifiers,
          &draft_.hotkeys.toggle_ui_modifiers};
      unsigned int* shortcut_keys[] = {
          &draft_.hotkeys.save_virtual_key, &draft_.hotkeys.save2_virtual_key,
          &draft_.hotkeys.save3_virtual_key, &draft_.hotkeys.record_virtual_key,
          &draft_.hotkeys.record2_virtual_key, &draft_.hotkeys.record3_virtual_key,
          &draft_.hotkeys.toggle_ui_virtual_key};
      for (int index = 0; index < 7; ++index) {
        ImGui::PushID(index);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(shortcut_labels[index]);
        ImGui::SameLine(220.0F);
        const auto label = hotkey_capture_target_ == index
                               ? std::string("press shortcut...  (esc cancels)")
                               : FormatHotkey(*shortcut_modifiers[index], *shortcut_keys[index]);
        if (ImGui::Button(label.c_str(), {300.0F, 0.0F})) {
          hotkey_capture_target_ = index;
          hotkey_capture_message_.clear();
        }
        ImGui::PopID();
      }

      if (hotkey_capture_target_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        hotkey_capture_target_ = -1;
        hotkey_capture_message_ = "shortcut change cancelled";
      } else if (hotkey_capture_target_ >= 0) {
        for (int key_value = ImGuiKey_NamedKey_BEGIN; key_value < ImGuiKey_NamedKey_END;
             ++key_value) {
          const auto key = static_cast<ImGuiKey>(key_value);
          if (!ImGui::IsKeyPressed(key, false)) continue;
          const auto virtual_key = VirtualKeyForImGuiKey(key);
          if (!virtual_key.has_value()) continue;
          const auto& io = ImGui::GetIO();
          unsigned int modifiers = HotkeyConfig::kNoRepeat;
          if (io.KeyCtrl) modifiers |= HotkeyConfig::kControl;
          if (io.KeyAlt) modifiers |= HotkeyConfig::kAlt;
          if (io.KeyShift) modifiers |= HotkeyConfig::kShift;
          if (io.KeySuper) modifiers |= HotkeyConfig::kWindows;
          // Plain keys are valid global shortcuts too; modifier keys are optional.
          bool duplicate = false;
          for (int other = 0; other < 7; ++other) {
            if (other != hotkey_capture_target_ &&
                SameChord(modifiers, *virtual_key, *shortcut_modifiers[other],
                          *shortcut_keys[other])) {
              duplicate = true;
              break;
            }
          }
          if (duplicate) {
            hotkey_capture_message_ = "that shortcut is already assigned";
            break;
          }
          *shortcut_modifiers[hotkey_capture_target_] = modifiers;
          *shortcut_keys[hotkey_capture_target_] = *virtual_key;
          hotkey_capture_message_ = "shortcut ready / save & apply when finished";
          hotkey_capture_target_ = -1;
          break;
        }
      }
      if (!hotkey_capture_message_.empty())
        ImGui::TextColored(hotkey_capture_target_ >= 0 ? kCoral : kMint, "%s",
                           hotkey_capture_message_.c_str());
    }

    {
      SectionLabel("storage / files and diagnostics");
      SectionLabel("storage / local files");
#if defined(KLIP_USE_LIBOBS)
      const char* container_names[] = {"MKV (recommended / crash-safe)", "MP4"};
      int container_index = draft_.output_container == "mp4" ? 1 : 0;
      if (ImGui::Combo("clip and recording container", &container_index, container_names, 2))
        draft_.output_container = container_index == 1 ? "mp4" : "mkv";
      ImGui::TextColored(kMuted, "MKV is the safe default. MP4 is supported but may be less recoverable after an interrupted write.");
#endif
      ImGui::InputText("clips folder", clips_path_.data(), clips_path_.size());
      ImGui::InputText("recordings folder", recordings_path_.data(), recordings_path_.size());
#if defined(KLIP_USE_LIBOBS)
      ImGui::InputText("clip filename format", obs_filename_format_.data(), obs_filename_format_.size());
      ImGui::TextColored(kMuted, "OBS tokens: %%CCYY-%%MM-%%DD / %%hh-%%mm-%%ss. Consecutive saves get unique names.");
      ImGui::TextColored(kMuted, "MKV clips and recordings / closing the dashboard keeps capture in the tray.");
      int memory_cap = static_cast<int>(draft_.rolling_buffer_bytes / (1024 * 1024));
      if (ImGui::SliderInt("replay memory cap", &memory_cap, 128, 2048, "%d MiB"))
        draft_.rolling_buffer_bytes = static_cast<std::size_t>(memory_cap) * 1024 * 1024;
#endif
      ImGui::TextColored(
          kMuted, "replay RAM is capped at %.0f MiB. encoded packets are shared, not duplicated.",
          static_cast<double>(draft_.rolling_buffer_bytes) / (1024.0 * 1024.0));
      if (ImGui::Button("copy diagnostics")) {
        diagnostics_preview_open_ = true;
        diagnostics_include_private_details_ = false;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("Preview locally; private details are excluded by default.");
    }
  }

  ImGui::EndChild();
  ImGui::SetCursorPos({22.0F, settings_window_size.y - 91.0F});
  ImGui::Separator();

  const bool busy = snapshot.recording || snapshot.finalizing_recording ||
                    snapshot.status == CaptureStatus::kSaving;
  if (!snapshot.settings_message.empty()) {
    ImGui::TextColored(snapshot.last_error.has_value() ? kCoral : kMint, "%s",
                       snapshot.settings_message.c_str());
  } else {
    ImGui::TextColored(kMuted, busy ? "finish the current recording before applying changes"
                                    : "changes apply immediately; capture may refresh briefly");
  }

  const float buttons_width = 342.0F;
  ImGui::SameLine(ImGui::GetWindowWidth() - buttons_width - 26.0F);
  if (ImGui::Button("cancel", {112.0F, 46.0F})) {
    ResetDraft(config);
    settings_open_ = false;
  }
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0F, 0.42F, 0.48F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.0F, 0.61F, 0.66F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.0F, 0.33F, 0.38F, 1.0F});
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("save & apply", {214.0F, 46.0F}) && commands.save_settings) {
    draft_.output_directory = ParsePath(clips_path_.data());
    draft_.recording_directory = ParsePath(recordings_path_.data());
    draft_.static_overlay_path = ParsePath(overlay_path_.data());
#if defined(KLIP_USE_LIBOBS)
    draft_.obs_filename_format = obs_filename_format_.data();
#endif
    if (commands.save_settings(draft_)) {
      hotkey_capture_message_.clear();
      settings_open_ = false;
    }
  }
  ImGui::EndDisabled();
  ImGui::PopStyleColor(3);

}

void MainPanel::RenderCompatibility(const ApplicationSnapshot& snapshot, const AppConfig& config,
                                   const UiCommands& commands) {
  SectionLabel("this pc / local compatibility check");
  const auto row = [](const char* label, bool ready, const std::string& detail,
                      bool warning = false) {
    const auto color = ready ? kMint : warning ? kWarningText : kCoral;
    ImGui::TextColored(color, "%s", ready ? "ready" : warning ? "check" : "not ready");
    ImGui::SameLine(106.0F);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(310.0F);
    ImGui::TextColored(kMuted, "%s", detail.c_str());
  };

  const auto windows_build = WindowsBuildNumber();
  const bool windows_supported = windows_build.has_value() && *windows_build >= 18362;
  row("Windows", windows_supported, WindowsBuildSummary(), !windows_build.has_value());
  row("64-bit processor", IsSupportedArchitecture(),
      IsSupportedArchitecture() ? "x64 build / supported architecture" : "REVO currently targets x64 Windows");
#if defined(KLIP_USE_LIBOBS)
  row("Dashboard D3D11", IsSupportedD3dLevel(snapshot.graphics_feature_level),
#else
  row("Direct3D graphics", IsSupportedD3dLevel(snapshot.graphics_feature_level),
#endif
      snapshot.graphics_feature_level.empty() ? "device feature level unavailable"
                                               : "feature level " + snapshot.graphics_feature_level);
#if defined(KLIP_USE_LIBOBS)
  const bool buffering_enabled = config.obs_replay_enabled;
  row("OBS video engine", !buffering_enabled || !snapshot.capture_adapter.empty(),
      !buffering_enabled ? "disabled / no video processing" : snapshot.capture_adapter.empty()
          ? "not initialized / inspect the startup error" : snapshot.capture_adapter);
  const bool capture_ready = buffering_enabled &&
      (snapshot.status == CaptureStatus::kBuffering || snapshot.status == CaptureStatus::kSaving);
  row("Capture source", !buffering_enabled || capture_ready,
      !buffering_enabled ? "disabled / replay buffering is off" : capture_ready
          ? "OBS source connected / inspect decoded clips for cadence" : "waiting for a live capture source");
  const bool encoder_ready = capture_ready && !snapshot.selected_encoder.empty() && snapshot.selected_encoder != "disabled";
  row("Video encoder", !buffering_enabled || encoder_ready,
      !buffering_enabled ? "disabled / no encoder running" : snapshot.selected_encoder.empty()
          ? "not configured / inspect the startup error" : snapshot.selected_encoder +
              (encoder_ready ? " / shared replay and recording encoder" : " / configured; waiting for capture"));
#else
  const bool routing_known = snapshot.capture_adapter_relationship == "same adapter" ||
                             snapshot.capture_adapter_relationship == "cross-adapter";
  row("Display / capture GPU routing", routing_known,
      snapshot.capture_adapter_relationship.empty()
          ? "monitor adapter mapping unavailable"
          : snapshot.capture_adapter_relationship +
                (snapshot.capture_adapter.empty() ? "" : " / " + snapshot.capture_adapter),
      snapshot.capture_adapter_relationship == "cross-adapter" || !routing_known);

  const bool capture_ready = snapshot.metrics.captured_frames > 0 &&
                             snapshot.status != CaptureStatus::kFailed;
  row("Capture source", capture_ready,
      capture_ready ? std::to_string(static_cast<int>(snapshot.metrics.source_fps)) +
                          " source updates/sec / " +
                          (snapshot.capture_target.empty() ? "source selected" : "source connected")
                    : "waiting for a live capture source");
  const bool encoder_ready = !snapshot.selected_encoder.empty();
  row("Video encoder", encoder_ready,
      encoder_ready ? snapshot.selected_encoder +
                          (snapshot.encoder_status.empty() ? " / opened for capture"
                                                           : " / " + snapshot.encoder_status)
                    : "waiting for the first encoded frame");
#endif

  const bool desktop_ready = !config.desktop_audio_enabled || snapshot.desktop_audio_active;
  row("Desktop audio", desktop_ready,
      !config.desktop_audio_enabled ? "optional / disabled in settings"
                                    : snapshot.desktop_audio_active ? "capture active"
                                                                    : "enabled / no active signal yet",
      config.desktop_audio_enabled && !snapshot.desktop_audio_active);
  const bool microphone_ready = !config.microphone_enabled || !snapshot.microphones.empty();
  row("Microphone", microphone_ready,
      !config.microphone_enabled
          ? "optional / disabled in settings"
          : snapshot.microphones.empty()
                ? "enabled / no input device found"
                : snapshot.microphone_active ? "input active / " +
                                                   std::to_string(snapshot.microphones.size()) +
                                                   " device(s)"
                                             : "device available / speak to verify input",
      config.microphone_enabled && snapshot.microphones.empty());

  const auto now = std::chrono::steady_clock::now();
  if (compatibility_storage_path_ != config.output_directory) {
    compatibility_storage_path_ = config.output_directory;
    compatibility_storage_check_after_ = {};
  }
  if (compatibility_recording_storage_path_ != config.recording_directory) {
    compatibility_recording_storage_path_ = config.recording_directory;
    compatibility_storage_check_after_ = {};
  }
  if (now >= compatibility_storage_check_after_) {
    const auto check_folder_space = [](const std::filesystem::path& path, bool& available,
                                       std::uint64_t& free_bytes) {
      std::error_code error;
      const bool directory_exists = std::filesystem::is_directory(path, error) && !error;
      if (directory_exists) {
        const auto space = std::filesystem::space(path, error);
        if (!error) free_bytes = space.available;
      }
      available = directory_exists && !error;
      if (!available) free_bytes = 0;
    };
    check_folder_space(compatibility_storage_path_, compatibility_storage_available_,
                       compatibility_free_bytes_);
    check_folder_space(compatibility_recording_storage_path_,
                       compatibility_recording_storage_available_,
                       compatibility_recording_free_bytes_);
    compatibility_storage_check_after_ = now + std::chrono::seconds(2);
  }
  constexpr std::uint64_t kMinimumTestSpace = 512ULL * 1024ULL * 1024ULL;
  const bool enough_space = compatibility_storage_available_ &&
                            compatibility_recording_storage_available_ &&
                            compatibility_free_bytes_ >= kMinimumTestSpace &&
                            compatibility_recording_free_bytes_ >= kMinimumTestSpace;
  const std::string storage_detail =
      !compatibility_storage_available_ || !compatibility_recording_storage_available_
          ? "clip or recording folder unavailable / write test not run"
          : "clips " + std::to_string(compatibility_free_bytes_ / (1024ULL * 1024ULL * 1024ULL)) +
                " GiB / recordings " +
                std::to_string(compatibility_recording_free_bytes_ /
                               (1024ULL * 1024ULL * 1024ULL)) +
                " GiB free";
  row("Clip storage", enough_space, storage_detail, !enough_space);

  ImGui::Spacing();
  ImGui::TextWrapped(
      "This local check reads the current Windows, graphics, capture, audio, and storage state. "
      "A short test recording verifies the actual encode and file-writing path; it is saved in "
      "your configured recordings folder and is not uploaded.");
  const bool capture_busy = snapshot.recording || snapshot.finalizing_recording ||
                            snapshot.status == CaptureStatus::kSaving || setup_test_requested_;
  const bool can_test = capture_ready && encoder_ready && enough_space && commands.toggle_recording;
  ImGui::BeginDisabled(!can_test || capture_busy);
  if (ImGui::Button("record a local 5-second test", {260.0F, 38.0F})) {
    setup_test_requested_ = true;
    setup_test_recording_started_ = false;
    setup_test_stop_sent_ = false;
    setup_test_requested_at_ = std::chrono::steady_clock::now();
    setup_test_previous_recording_ = snapshot.last_saved_recording;
    setup_test_status_ = "starting test recording";
    commands.toggle_recording();
  }
  ImGui::EndDisabled();
  if (!can_test && !capture_busy)
    ImGui::TextColored(kMuted, "The test unlocks when capture, an encoder, and storage are ready.");
  if (!setup_test_status_.empty()) {
    const bool passed = setup_test_status_.starts_with("passed");
    const bool failed = setup_test_status_.starts_with("could not") ||
                        setup_test_status_.starts_with("failed");
    ImGui::TextColored(passed ? kMint : failed ? kCoral : kWarningText, "%s",
                       setup_test_status_.c_str());
  }
}

void MainPanel::UpdateSetupTest(const ApplicationSnapshot& snapshot, const UiCommands& commands) {
  if (!setup_test_requested_) return;
  const auto now = std::chrono::steady_clock::now();
  if (!setup_test_recording_started_) {
    if (snapshot.recording) {
      setup_test_recording_started_ = true;
      setup_test_stop_at_ = now + std::chrono::seconds(5);
      setup_test_status_ = "recording / will stop automatically after five seconds";
    } else if (now - setup_test_requested_at_ > std::chrono::seconds(15)) {
      setup_test_requested_ = false;
      setup_test_status_ = "could not start / check the capture error details";
    }
    return;
  }
  if (!setup_test_stop_sent_) {
    if (!snapshot.recording) {
      setup_test_requested_ = false;
      setup_test_status_ = "failed / test recording stopped before five seconds";
      return;
    }
    if (now >= setup_test_stop_at_) {
      if (commands.toggle_recording) {
        setup_test_stop_sent_ = true;
        setup_test_status_ = "finalizing local test file";
        commands.toggle_recording();
      } else {
        setup_test_requested_ = false;
        setup_test_status_ = "failed / recording could not be stopped automatically";
      }
    }
    return;
  }
  if (snapshot.recording || snapshot.finalizing_recording) return;
  setup_test_requested_ = false;
  setup_test_status_ = !snapshot.last_saved_recording.empty() &&
                               snapshot.last_saved_recording != setup_test_previous_recording_
                           ? "passed / test recording saved and finalized"
                           : "failed / no completed test file; check the capture error details";
}

void MainPanel::RenderDiagnosticsPreview(const ApplicationSnapshot& snapshot,
                                         const AppConfig& config) {
  SectionLabel("support report / local preview");
  ImGui::TextColored(kMuted, "Nothing is sent automatically. Review the report before sharing.");
  ImGui::Checkbox("include adapter, microphone, capture-source names, output paths, and full error text",
                  &diagnostics_include_private_details_);
  const auto report = BuildDiagnostics(snapshot, config, diagnostics_include_private_details_);
  ImGui::BeginChild("##support-report-text", {0.0F, -58.0F}, true,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::TextUnformatted(report.c_str());
  ImGui::EndChild();
  if (ImGui::Button("copy reviewed report", {205.0F, 38.0F})) {
    ImGui::SetClipboardText(report.c_str());
    diagnostics_copied_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    diagnostics_preview_open_ = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("back to settings", {150.0F, 38.0F})) {
    diagnostics_preview_open_ = false;
  }
}

void MainPanel::ResetDraft(const AppConfig& config) {
  draft_ = config;
  if (draft_.target_fps != 30 && draft_.target_fps != 60 && draft_.target_fps != 120 && draft_.target_fps != 240) {
    draft_.target_fps = 60;
    ResizeReplayBudget(draft_);
  }
  const auto clips = PathToUtf8(config.output_directory);
  const auto recordings = PathToUtf8(config.recording_directory);
  const auto overlay = PathToUtf8(config.static_overlay_path);
  std::snprintf(clips_path_.data(), clips_path_.size(), "%s", clips.c_str());
  std::snprintf(recordings_path_.data(), recordings_path_.size(), "%s", recordings.c_str());
  std::snprintf(overlay_path_.data(), overlay_path_.size(), "%s", overlay.c_str());
  std::snprintf(obs_filename_format_.data(), obs_filename_format_.size(), "%s", config.obs_filename_format.c_str());
  draft_initialized_ = true;
  hotkey_capture_target_ = -1;
  hotkey_capture_message_.clear();
}

}  // namespace klip
