#include "klip/obs/obs_engine.h"

#include <obs.h>
#include <obs-module.h>
#include <obs-audio-controls.h>
#include <tlhelp32.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <thread>

#include "klip/core/path_text.h"
#include "klip/obs/encoder_policy.h"
#include "klip/obs/game_profile_policy.h"
#include "klip/obs/gdi_monitor_capture.h"
#include "klip/obs/replay_policy.h"

namespace klip {
namespace {
using Data = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>;
using Properties = std::unique_ptr<obs_properties_t, decltype(&obs_properties_destroy)>;
Data MakeData() { return Data(obs_data_create(), obs_data_release); }

std::filesystem::path RuntimeRoot() {
  std::wstring path(32768, L'\0');
  auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) throw std::runtime_error("Could not locate REVO runtime");
  path.resize(size);
  return std::filesystem::path(path).parent_path().parent_path().parent_path();
}

void ObsLog(int level, const char* format, va_list args, void* parameter) noexcept {
  if (level > LOG_INFO) return;
  std::array<char, 8192> text{};
  vsnprintf(text.data(), text.size(), format, args);
  auto& logger = *static_cast<Logger*>(parameter);
  try {
    if (level <= LOG_WARNING) logger.Warning(text.data());
    else logger.Info(text.data());
  } catch (...) {
    // Never unwind a C++ exception into libobs or its module threads.
    OutputDebugStringA(text.data());
  }
}

bool SetList(obs_properties_t* props, obs_data_t* settings, const char* key,
             const char* desired, bool required = true) {
  auto* property = obs_properties_get(props, key);
  if (property && obs_property_get_type(property) == OBS_PROPERTY_LIST) {
    for (std::size_t i = 0; i < obs_property_list_item_count(property); ++i) {
      if (!obs_property_list_item_disabled(property, i) &&
          std::string_view(obs_property_list_item_string(property, i)) == desired) {
        obs_data_set_string(settings, key, desired);
        return true;
      }
    }
  }
  if (required) throw std::runtime_error(std::string("Encoder does not support ") + key + "=" + desired);
  return false;
}

void SetInt(obs_properties_t* props, obs_data_t* settings, const char* key, int value) {
  auto* p = obs_properties_get(props, key);
  if (!p) return;
  if (obs_property_get_type(p) != OBS_PROPERTY_INT || value < obs_property_int_min(p) ||
      value > obs_property_int_max(p))
    throw std::runtime_error(std::string("Encoder property is out of range: ") + key);
  obs_data_set_int(settings, key, value);
}

bool SetListInt(obs_properties_t* props, obs_data_t* settings, const char* key, int value) {
  auto* property = obs_properties_get(props, key);
  if (!property || obs_property_get_type(property) != OBS_PROPERTY_LIST) return false;
  for (std::size_t i = 0; i < obs_property_list_item_count(property); ++i) {
    if (!obs_property_list_item_disabled(property, i) && obs_property_list_item_int(property, i) == value) {
      obs_data_set_int(settings, key, value); return true;
    }
  }
  return false;
}

std::string OutputError(obs_output_t* output) {
  const char* message = output ? obs_output_get_last_error(output) : nullptr;
  return message && *message ? message : "OBS output could not start; see the preceding OBS log details";
}

std::uint64_t ChoiceId(const std::string& value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : value) hash = (hash ^ c) * 1099511628211ULL;
  return hash;
}

bool IsFullscreenWindow(HWND window) {
  if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
      window == GetDesktopWindow())
    return false;
  const auto extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((extended_style & WS_EX_TOOLWINDOW) != 0 || (extended_style & WS_EX_NOACTIVATE) != 0)
    return false;

  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id == 0 || process_id == GetCurrentProcessId()) return false;

  RECT bounds{};
  if (!GetWindowRect(window, &bounds)) return false;
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return false;

  constexpr LONG kEdgeTolerance = 3;
  const RECT& screen = info.rcMonitor;
  return bounds.left >= screen.left - kEdgeTolerance &&
         bounds.top >= screen.top - kEdgeTolerance &&
         bounds.right <= screen.right + kEdgeTolerance &&
         bounds.bottom <= screen.bottom + kEdgeTolerance &&
         bounds.left <= screen.left + kEdgeTolerance &&
         bounds.top <= screen.top + kEdgeTolerance &&
         bounds.right >= screen.right - kEdgeTolerance &&
         bounds.bottom >= screen.bottom - kEdgeTolerance;
}

bool IsIgnoredFullscreenExecutable(std::wstring_view exe) {
  // Avoid treating Windows chrome, desktop/shell windows, and REVO itself as games.
  constexpr std::array<std::wstring_view, 15> ignored{
      L"explorer.exe", L"dwm.exe", L"sihost.exe", L"searchhost.exe",
      L"searchapp.exe", L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
      L"textinputhost.exe", L"applicationframehost.exe", L"lockapp.exe",
      L"taskmgr.exe", L"gamebar.exe", L"gamebarftserver.exe", L"widgets.exe",
      L"revo.exe"};
  return std::find(ignored.begin(), ignored.end(), exe) != ignored.end();
}

bool ReserveOutputFilename(const std::filesystem::path& directory, const std::string& extension,
                           const std::string& pattern, OutputReservation& reservation,
                           std::string& format, Error& error,
                           ErrorComponent component) {
  char* generated = os_generate_formatted_filename(extension.c_str(), false, pattern.c_str());
  if (!generated) { error = Error{component, "format output filename", "OBS filename generation failed"}; return false; }
  const auto* utf8 = reinterpret_cast<const char8_t*>(generated);
  const auto stem = std::filesystem::path(std::u8string(utf8)).stem();
  bfree(generated);
  for (std::size_t suffix = 0; suffix < 1000; ++suffix) {
    format = PathToUtf8(stem) + (suffix ? "_" + std::to_string(suffix + 1) : "");
    const auto* begin = reinterpret_cast<const char8_t*>(format.data());
    const auto suffix_begin = reinterpret_cast<const char8_t*>(extension.data());
    const auto suffix_path = std::u8string(u8".") + std::u8string(suffix_begin, suffix_begin + extension.size());
    const auto path = directory / std::filesystem::path(std::u8string(begin, begin + format.size()) + suffix_path);
    DWORD code = ERROR_SUCCESS;
    if (reservation.Reserve(path, code)) return true;
    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
      error = MakeWin32Error(component, "reserve output filename", code, PathToUtf8(path));
      return false;
    }
  }
  error = Error{component, "reserve output filename", "Too many files share the selected filename pattern"};
  return false;
}
}  // namespace

bool ObsEngine::Initialize(const AppConfig& config, HWND window, Error& error) {
  Shutdown();
  config_ = config;
  config_.obs_separate_audio_tracks = true;
  // Display capture is provided by REVO's GDI/BitBlt source to avoid the WGC border.
  // Start with the non-Minecraft profile; detection upgrades to Minecraft's
  // 240 FPS / 35 Mbps profile if javaw.exe is running.
  config_.target_fps = 60;
  config_.obs_replay_bitrate_kbps = 20'000;
  config_.obs_duration_priority = true;  // Profiles require a fixed CBR bitrate.
  {
    const double retained_seconds = std::max(1.0, config_.clip_duration_seconds) + 15.0;
    const double audio_kbps = static_cast<double>(config_.audio_bitrate) / 1000.0 *
        (config_.obs_separate_audio_tracks ? 5.0 : 1.0);
    const double required_bytes = std::ceil(
        (20'000.0 + audio_kbps) * 1000.0 / 8.0 * retained_seconds * 1.25);
    config_.rolling_buffer_bytes = static_cast<std::size_t>(
        std::max(32.0 * 1024.0 * 1024.0, required_bytes));
  }
  clips_root_ = config.output_directory;
  recordings_root_ = config.recording_directory;
  active_game_.clear();
  active_game_filename_prefix_.clear();
  next_game_detection_ = std::chrono::steady_clock::now();
  window_ = window;
  shutting_down_ = false;
  replay_stop_code_ = 0;
  replay_saved_ = false;
  recording_stop_event_ = 0;
  save_failed_ = false;
  state_.SetCaptureAdapter({}, {});
  state_.SetCaptureMetrics(0, 0, 0, 0, 0, 0, 0, 0);
  state_.SetRollingMetrics(0, 0, 0);
  if (!config.obs_replay_enabled) {
    state_.SetEncoder("disabled");
    state_.SetEncoderStatus("Replay buffering disabled; no encoder is running");
    state_.SetAudio(false, false, 0, 0);
    state_.SetStatus(CaptureStatus::kIdle, "Replay buffering disabled");
    return true;
  }
  try {
    if (!config.excluded_audio_process.empty()) {
      logger_.Warning("Ignoring legacy per-application audio exclusion; desktop capture now records all Windows output.");
    }
    std::filesystem::create_directories(config.output_directory);
    std::filesystem::create_directories(config.recording_directory);
    const auto root = RuntimeRoot();
    base_set_log_handler(ObsLog, &logger_);
    if (!obs_startup("en-US", PathToUtf8(config.log_path.parent_path()).c_str(), nullptr))
      throw std::runtime_error("obs_startup failed");
    initialized_ = true;
    if (std::string_view(obs_get_version_string()) != "32.1.2")
      throw std::runtime_error("Packaged libobs version differs from pinned 32.1.2");
    obs_hotkey_enable_background_press(false);  // REVO uses RegisterHotKey, not OBS polling.
    core_data_path_ = PathToUtf8(root / "data/libobs") + "/";
    obs_add_data_path(core_data_path_.c_str());

    obs_audio_info audio{};
    audio.samples_per_sec = 48000;
    audio.speakers = SPEAKERS_STEREO;
    if (!obs_reset_audio(&audio)) throw std::runtime_error("OBS 48 kHz stereo audio initialization failed");
    obs_video_info video{};
    const auto graphics_path = PathToUtf8(root / "bin/64bit/libobs-d3d11.dll");
    video.graphics_module = graphics_path.c_str();
    video.fps_num = config_.target_fps;
    video.fps_den = 1;
    video.base_width = video.output_width = config.output_width ? config.output_width : 1920;
    video.base_height = video.output_height = config.output_height ? config.output_height : 1080;
    video.output_format = VIDEO_FORMAT_NV12;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_PARTIAL;
    video.gpu_conversion = true;
    video.scale_type = OBS_SCALE_BICUBIC;
    if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
      throw std::runtime_error("OBS D3D11 video initialization failed");
    state_.SetCaptureAdapter("OBS D3D11 video engine", "Initialized / adapter details are in the OBS log");
    obs_enter_graphics();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
      auto& logger = *reinterpret_cast<Logger*>(parameter);
      MONITORINFOEXA info{}; info.cbSize = sizeof(info);
      GetMonitorInfoA(monitor, &info);
      logger.Info(std::string("OBS DXGI monitor ") + info.szDevice + " index=" + std::to_string(gs_duplicator_get_monitor_index(monitor)));
      return TRUE;
    }, reinterpret_cast<LPARAM>(&logger_));
    obs_leave_graphics();

    std::vector<const char*> modules{"win-capture", "win-wasapi", "obs-ffmpeg", "obs-nvenc", "obs-qsv11", "obs-x264"};
    if (config.static_overlay_enabled) modules.push_back("image-source");
    if ((config.static_overlay_enabled || config.live_overlay_enabled) && config.static_overlay_opacity < 1.0)
      modules.push_back("obs-filters");
    for (const char* name : modules) {
      obs_module_t* module = nullptr;
      const auto binary = PathToUtf8(root / "obs-plugins/64bit" / (std::string(name) + ".dll"));
      const auto data = PathToUtf8(root / "data/obs-plugins" / name);
      const bool loaded = obs_open_module(&module, binary.c_str(), data.c_str()) == MODULE_SUCCESS &&
                          obs_init_module(module);
      logger_.Info(std::string("OBS module ") + name + (loaded ? " loaded" : " unavailable"));
      if (!loaded && (std::string_view(name) == "win-capture" ||
                      std::string_view(name) == "win-wasapi" || std::string_view(name) == "obs-ffmpeg" ||
                      std::string_view(name) == "image-source" || std::string_view(name) == "obs-filters"))
        throw std::runtime_error(std::string("Required OBS module failed: ") + name);
    }
    obs_post_load_modules();
    RegisterGdiMonitorCapture();
    scene_ = obs_scene_create_private("REVO capture graph");
    if (!scene_) throw std::runtime_error("Could not create internal OBS scene");
    obs_set_output_source(0, obs_scene_get_source(scene_));
    RefreshSources();
    if (!ConfigureCapture(config.target_mode,
                          config.target_mode == CaptureTargetMode::kDisplay ? config.preferred_display_name : config.preferred_game_title,
                          error)) { Shutdown(); return false; }
    if (!ConfigureOverlay(error)) { Shutdown(); return false; }

    native_resolution_pending_ = !config.output_width || !config.output_height;
    ConfigureAudio(config_);
    if (!ConfigureEncoder(error) || !StartReplayBuffer(error)) { Shutdown(); return false; }
    logger_.Info("LIBOBS ENGINE READY version=" + std::string(obs_get_version_string()) +
                 " resolution=" + std::to_string(video.output_width) + "x" + std::to_string(video.output_height) +
                 " fps=" + std::to_string(config_.target_fps) + " NV12 Rec709 limited");
    Tick();
    return true;
  } catch (const std::exception& exception) {
    error = Error{ErrorComponent::kApplication, "initialize libobs", exception.what()};
    Shutdown();
    return false;
  }
}

void ObsEngine::RefreshSources() {
  if (!initialized_) return;
  const auto read = [](const char* source, const char* key, std::vector<SourceChoice>& choices) {
    choices.clear();
    Properties properties(obs_get_source_properties(source), obs_properties_destroy);
    if (!properties) return;
    auto* list = obs_properties_get(properties.get(), key);
    if (!list) return;
    for (std::size_t i = 0; i < obs_property_list_item_count(list); ++i) {
      if (obs_property_list_item_disabled(list, i)) continue;
      const std::string value = obs_property_list_item_string(list, i);
      if (value.empty()) continue;
      choices.push_back({ChoiceId(value), obs_property_list_item_name(list, i), value});
    }
  };
  read("game_capture", "window", games_);
  read("monitor_capture", "monitor_id", displays_);
  read("wasapi_input_capture", "device_id", microphones_);
  read("window_capture", "window", overlay_windows_);
  std::vector<CaptureSourceOption> games, displays;
  for (const auto& c : games_) games.push_back({c.id, c.label});
  for (const auto& c : displays_) displays.push_back({c.id, c.label});
  state_.SetCaptureSources(std::move(games), std::move(displays), selected_source_id_);
  std::vector<CaptureSourceOption> overlays;
  for (const auto& c : overlay_windows_) overlays.push_back({c.id, c.label});
  state_.SetOverlaySources(std::move(overlays));
  std::vector<std::string> microphones;
  int selected = -1;
  for (const auto& c : microphones_) {
    if (c.label == config_.preferred_microphone_name ||
        (config_.preferred_microphone_name.empty() && c.value == "default")) selected = static_cast<int>(microphones.size());
    microphones.push_back(c.label);
  }
  state_.SetMicrophones(std::move(microphones), selected);
}

bool ObsEngine::ConfigureCapture(CaptureTargetMode mode, const std::string& label, Error& error) {
  if (!initialized_) return true;
  {
    std::scoped_lock lock(save_mutex_);
    if (saving_ || queued_saves_) {
      error = Error{ErrorComponent::kCapture, "change OBS source",
          "Finish queued clip saves before changing capture source"};
      return false;
    }
  }
  const auto& choices = mode == CaptureTargetMode::kDisplay ? displays_ : games_;
  const SourceChoice* selected = nullptr;
  for (const auto& choice : choices) if (choice.label == label || choice.value == label) selected = &choice;
  if (!selected && !label.empty()) {
    // Migrate the previous "title [exe]" label only when the title identifies one OBS choice.
    const auto old_title = label.substr(0, label.find("  ["));
    const SourceChoice* match = nullptr;
    for (const auto& choice : choices) {
      if (choice.label.find(old_title) != std::string::npos) {
        if (match) { match = nullptr; break; }
        match = &choice;
      }
    }
    selected = match;
  }
  if (!selected && mode == CaptureTargetMode::kDisplay && label.starts_with("\\\\.\\DISPLAY")) {
    for (DWORD adapter_index = 0; ; ++adapter_index) {
      DISPLAY_DEVICEA adapter{}; adapter.cb = sizeof(adapter);
      if (!EnumDisplayDevicesA(nullptr, adapter_index, &adapter, 0)) break;
      if (!label.starts_with(adapter.DeviceName)) continue;
      for (DWORD monitor_index = 0; ; ++monitor_index) {
        DISPLAY_DEVICEA monitor{}; monitor.cb = sizeof(monitor);
        if (!EnumDisplayDevicesA(adapter.DeviceName, monitor_index, &monitor, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        for (const auto& choice : choices)
          if (_stricmp(choice.value.c_str(), monitor.DeviceID) == 0) selected = &choice;
      }
    }
  }
  if (!selected && mode == CaptureTargetMode::kDisplay && label.empty() && !choices.empty()) selected = &choices.front();
  if (!selected && !label.empty() && mode == CaptureTargetMode::kDisplay) {
    error = Error{ErrorComponent::kCapture, "select OBS capture source", "The selected source is unavailable. Choose a currently running game or display.", {}, {}, label};
    return false;
  }
  auto settings = MakeData();
  const bool display_capture = mode == CaptureTargetMode::kDisplay;
  const char* type = display_capture ? "klip_gdi_monitor_capture" : "game_capture";
  if (display_capture) {
    if (!selected) { error = Error{ErrorComponent::kCapture, "select display", "OBS found no displays"}; return false; }
    obs_data_set_string(settings.get(), "monitor_id", selected->value.c_str());
    obs_data_set_bool(settings.get(), "capture_cursor", config_.capture_cursor);
  } else {
    pending_game_label_ = !selected && !label.empty() ? label : "";
    obs_data_set_string(settings.get(), "capture_mode", selected || !label.empty() ? "window" : "any_fullscreen");
    if (selected) obs_data_set_string(settings.get(), "window", selected->value.c_str());
    obs_data_set_bool(settings.get(), "capture_cursor", config_.capture_cursor);
    obs_data_set_bool(settings.get(), "limit_framerate", config_.obs_limit_game_capture_fps);
    logger_.Info(std::string("OBS game capture copy limiter=") +
                 (config_.obs_limit_game_capture_fps ? "clip FPS" : "twice clip FPS (OBS default)"));
    obs_data_set_bool(settings.get(), "capture_audio", false); // Desktop audio is captured once.
    obs_data_set_bool(settings.get(), "anti_cheat_hook", true); // Official OBS compatibility mode only.
  }
  auto* next = obs_source_create(type, display_capture ? "REVO display (GDI)" : "REVO gameplay", settings.get(), nullptr);
  if (!next) { error = Error{ErrorComponent::kCapture, "create OBS source", std::string(type) + " creation failed"}; return false; }
  if (capture_item_) obs_sceneitem_remove(capture_item_);
  if (capture_) obs_source_release(capture_);
  capture_ = next;
  capture_started_ = std::chrono::steady_clock::now();
  display_fallback_attempted_ = false;
  capture_was_ready_ = false;
  capture_item_ = obs_scene_add(scene_, capture_);
  if (!capture_item_) { error = Error{ErrorComponent::kCapture, "add OBS capture to scene", "OBS could not add the selected capture source"}; return false; }
  obs_sceneitem_set_order(capture_item_, OBS_ORDER_MOVE_BOTTOM);
  layout_width_ = layout_height_ = 0;
  logger_.Info("OBS source lifecycle scene_showing=" + std::to_string(obs_source_showing(obs_scene_get_source(scene_))) +
               " capture_showing=" + std::to_string(obs_source_showing(capture_)) +
               " capture_active=" + std::to_string(obs_source_active(capture_)));
  config_.target_mode = mode;
  selected_source_id_ = selected ? selected->id : 0;
  state_.SetTarget(mode, selected ? selected->label : !pending_game_label_.empty() ? "Waiting for " + pending_game_label_ : "Automatic fullscreen game (OBS Game Capture)", selected_source_id_);
  logger_.Info(std::string("OBS capture type=") + type + " source=" + (selected ? selected->label : "automatic fullscreen"));
  if (replay_ && obs_output_active(replay_)) {
    StopOutput(replay_);
    replay_waiting_for_source_ = true;
  }
  return true;
}

bool ObsEngine::ConfigureOverlay(Error& error) {
  if (!config_.static_overlay_enabled && !config_.live_overlay_enabled) return true;
  auto settings = MakeData();
  const char* type;
  if (config_.static_overlay_enabled) {
    if (!std::filesystem::is_regular_file(config_.static_overlay_path)) {
      error = Error{ErrorComponent::kCapture, "load static overlay", "The selected overlay image does not exist"}; return false;
    }
    type = "image_source";
    obs_data_set_string(settings.get(), "file", PathToUtf8(config_.static_overlay_path).c_str());
    obs_data_set_bool(settings.get(), "unload", false);
  } else {
    type = "window_capture";
    const SourceChoice* selected = nullptr;
    for (const auto& choice : overlay_windows_)
      if (choice.label == config_.live_overlay_window_title || choice.value == config_.live_overlay_window_title) selected = &choice;
    if (!selected) {
      const auto old_title = config_.live_overlay_window_title.substr(0, config_.live_overlay_window_title.find("  ["));
      for (const auto& choice : overlay_windows_) {
        if (choice.label.find(old_title) == std::string::npos) continue;
        if (selected) { selected = nullptr; break; }
        selected = &choice;
      }
    }
    pending_overlay_label_ = selected ? "" : config_.live_overlay_window_title;
    obs_data_set_string(settings.get(), "window", selected ? selected->value.c_str() : "");
    Properties properties(obs_get_source_properties(type), obs_properties_destroy);
    if (!properties || !SetListInt(properties.get(), settings.get(), "method", 2) ||
        !SetListInt(properties.get(), settings.get(), "priority", 0)) {
      error = Error{ErrorComponent::kCapture, "configure live overlay",
                    "OBS Windows Graphics Capture with exact-title matching is unavailable on this system"}; return false;
    }
    obs_data_set_bool(settings.get(), "cursor", false);
    obs_data_set_bool(settings.get(), "client_area", true);
    obs_data_set_bool(settings.get(), "capture_audio", false);
  }
  overlay_ = obs_source_create_private(type, "REVO overlay", settings.get());
  if (!overlay_) { error = Error{ErrorComponent::kCapture, "create OBS overlay", "OBS overlay source creation failed"}; return false; }
  if (config_.static_overlay_enabled && !obs_source_get_width(overlay_)) {
    error = Error{ErrorComponent::kCapture, "decode static overlay", "OBS could not decode the selected image"}; return false;
  }
  if (config_.static_overlay_opacity < 1.0) {
    // Pinned OBS registers its float-opacity implementation as color_filter_v2.
    Properties properties(obs_get_source_properties("color_filter_v2"), obs_properties_destroy);
    auto* opacity = properties ? obs_properties_get(properties.get(), "opacity") : nullptr;
    if (!opacity || obs_property_get_type(opacity) != OBS_PROPERTY_FLOAT ||
        config_.static_overlay_opacity < obs_property_float_min(opacity) ||
        config_.static_overlay_opacity > obs_property_float_max(opacity)) {
      error = Error{ErrorComponent::kCapture, "configure overlay opacity", "OBS opacity filter does not expose the expected float property"}; return false;
    }
    auto filter_settings = MakeData();
    obs_data_set_double(filter_settings.get(), "opacity", config_.static_overlay_opacity);
    auto* filter = obs_source_create_private("color_filter_v2", "REVO overlay opacity", filter_settings.get());
    if (!filter) { error = Error{ErrorComponent::kCapture, "create overlay opacity", "OBS opacity filter creation failed"}; return false; }
    obs_source_filter_add(overlay_, filter);
    obs_source_release(filter);  // The source now owns the filter reference.
  }
  overlay_item_ = obs_scene_add(scene_, overlay_);
  if (!overlay_item_) { error = Error{ErrorComponent::kCapture, "compose OBS overlay", "OBS could not add the overlay to its scene"}; return false; }
  layout_width_ = layout_height_ = 0;
  UpdateSceneLayout();
  logger_.Info(std::string("OBS OVERLAY READY type=") + type + " opacity=" + std::to_string(config_.static_overlay_opacity));
  if (!pending_overlay_label_.empty()) state_.SetSettingsStatus(false, "Overlay waiting for window: " + pending_overlay_label_);
  return true;
}

void ObsEngine::UpdateSceneLayout() {
  obs_video_info video{};
  if (!obs_get_video_info(&video) || (layout_width_ == video.base_width && layout_height_ == video.base_height)) return;
  layout_width_ = video.base_width; layout_height_ = video.base_height;
  vec2 bounds{}, position{};
  if (capture_item_) {
    vec2_set(&bounds, static_cast<float>(video.base_width), static_cast<float>(video.base_height));
    obs_sceneitem_set_bounds_type(capture_item_, config_.scaling_mode == VideoScalingMode::kFit ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_STRETCH);
    obs_sceneitem_set_bounds(capture_item_, &bounds);
  }
  if (overlay_item_) {
    vec2_set(&position, static_cast<float>(config_.static_overlay_x * video.base_width),
                       static_cast<float>(config_.static_overlay_y * video.base_height));
    vec2_set(&bounds, static_cast<float>(config_.static_overlay_width * video.base_width),
                     static_cast<float>(config_.static_overlay_height * video.base_height));
    obs_sceneitem_set_alignment(overlay_item_, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    obs_sceneitem_set_bounds_alignment(overlay_item_, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    obs_sceneitem_set_pos(overlay_item_, &position);
    obs_sceneitem_set_bounds_type(overlay_item_, OBS_BOUNDS_STRETCH);
    obs_sceneitem_set_bounds(overlay_item_, &bounds);
    obs_sceneitem_set_order(overlay_item_, OBS_ORDER_MOVE_TOP);
  }
}

void ObsEngine::ConfigureAudio(const AppConfig& config) {
  if (!initialized_) return;
  auto configure = [&](obs_source_t*& source, obs_volmeter_t*& meter,
                       std::atomic<float>& level, bool enabled, const char* type,
                       const char* name, std::uint32_t channel, const std::string& device,
                       float gain, std::uint32_t mixers) {
    if (!enabled) {
      obs_set_output_source(channel, nullptr);
      if (meter) { obs_volmeter_destroy(meter); meter = nullptr; }
      if (source) obs_source_release(source);
      source = nullptr;
      level.store(0.0F, std::memory_order_relaxed);
      return;
    }
    auto settings = MakeData();
    obs_data_set_string(settings.get(), "device_id", device.c_str());
    obs_data_set_bool(settings.get(), "use_device_timing",
                      std::string_view(type) == "wasapi_output_capture");
    if (!source) source = obs_source_create(type, name, settings.get(), nullptr);
    else obs_source_update(source, settings.get());
    if (source) {
      obs_source_set_audio_mixers(source, mixers);
      obs_source_set_volume(source, gain);
      obs_set_output_source(channel, source);
      if (!meter && dashboard_active_) {
        meter = obs_volmeter_create(OBS_FADER_LOG);
        if (meter) {
          obs_volmeter_add_callback(meter, MeterUpdated, &level);
          obs_volmeter_attach_source(meter, source);
        }
      }
    } else {
      level.store(0.0F, std::memory_order_relaxed);
    }
  };
  std::string device = "default";
  bool microphone_available = config.preferred_microphone_name.empty();
  for (const auto& choice : microphones_) {
    if (choice.label == config.preferred_microphone_name) {
      device = choice.value;
      microphone_available = true;
    }
  }
  if (config.microphone_enabled && !microphone_available) {
    const Error error{ErrorComponent::kAudio, "select microphone",
                      "The configured microphone is disconnected. Choose an available device.", {}, {},
                      config.preferred_microphone_name};
    state_.SetError(error);
    logger_.ErrorMessage(error);
  }

  // WASAPI endpoint loopback captures the complete default Windows output mix:
  // browsers, games, media players, and all other system sounds.
  configure(desktop_audio_, desktop_audio_meter_, desktop_audio_level_,
            config.obs_separate_audio_tracks && config.desktop_audio_enabled,
            "wasapi_output_capture", "REVO desktop audio", 5, "default",
            static_cast<float>(config.desktop_audio_gain), 1U);
  if (config.obs_separate_audio_tracks && config.desktop_audio_enabled && desktop_audio_) {
    logger_.Info("Desktop audio captures the default Windows render endpoint (all system output)");
  }

  const auto configure_process = [&](obs_source_t*& source, const char* name,
                                     const std::string& window, std::uint32_t channel,
                                     std::uint32_t dedicated_mix) {
    if (window.empty()) {
      if (std::string_view(name) == "REVO game audio") game_audio_selector_resolved_ = false;
      obs_set_output_source(channel, nullptr);
      if (source) obs_source_release(source);
      source = nullptr;
      return;
    }
    auto settings = MakeData();
    // Resolve stable executable selectors to the exact value exposed by WASAPI.
    std::string source_window = window;
    bool selector_resolved = !window.starts_with("::");
    Properties process_properties(
        obs_get_source_properties("wasapi_process_output_capture"), obs_properties_destroy);
    auto* process_window = process_properties
        ? obs_properties_get(process_properties.get(), "window") : nullptr;
    if (process_window && window.starts_with("::")) {
      const auto desired_exe = window.substr(2);
      for (std::size_t i = 0; i < obs_property_list_item_count(process_window); ++i) {
        if (obs_property_list_item_disabled(process_window, i)) continue;
        const char* candidate = obs_property_list_item_string(process_window, i);
        if (!candidate) continue;
        const auto last_colon = std::string_view(candidate).rfind(':');
        if (last_colon != std::string_view::npos &&
            _stricmp(std::string(candidate + last_colon + 1).c_str(), desired_exe.c_str()) == 0) {
          source_window = candidate;
          selector_resolved = true;
          break;
        }
      }
    }
    if (std::string_view(name) == "REVO game audio")
      game_audio_selector_resolved_ = selector_resolved;
    obs_data_set_string(settings.get(), "window", source_window.c_str());
    obs_data_set_int(settings.get(), "priority", 2);
    if (!source)
      source = obs_source_create("wasapi_process_output_capture", name, settings.get(), nullptr);
    else
      obs_source_update(source, settings.get());
    if (!source)
      throw std::runtime_error(std::string("OBS application-audio capture is unavailable for ") + name);

    // Spotify is part of the composite desktop+mic mix (Track 1) while also
    // retaining its isolated stem (Track 5).
    const bool spotify_source = std::string_view(name) == "REVO Spotify audio";
    const auto source_mixers = config.obs_separate_audio_tracks
        ? (dedicated_mix | (spotify_source ? 1U : 0U))
        : 1U;
    obs_source_set_audio_mixers(source, source_mixers);
    const float gain = std::string_view(name) == "REVO Discord audio"
                           ? static_cast<float>(config.discord_audio_gain)
                           : spotify_source ? static_cast<float>(config.music_audio_gain)
                                            : static_cast<float>(config.game_audio_gain);
    obs_source_set_volume(source, gain);
    obs_set_output_source(channel, source);
  };

  configure_process(game_audio_, "REVO game audio", config.game_audio_window, 1, 1U << 2);
  configure(microphone_, microphone_meter_, microphone_level_,
            config.microphone_enabled && microphone_available,
            "wasapi_input_capture", "REVO microphone", 2, device,
            static_cast<float>(config.microphone_audio_gain),
            config.obs_separate_audio_tracks ? (1U | (1U << 1)) : 1U);
  configure_process(discord_audio_, "REVO Discord audio", config.discord_audio_window, 3, 1U << 3);
  configure_process(music_audio_, "REVO Spotify audio", config.music_audio_window, 4, 1U << 4);

  config_.desktop_audio_enabled = config.desktop_audio_enabled;
  config_.microphone_enabled = config.microphone_enabled;
  config_.preferred_microphone_name = config.preferred_microphone_name;
  state_.SetMicrophoneEnabled(config.microphone_enabled);
  UpdateAudioMeters();
  state_.SetAudio(desktop_audio_ && obs_source_audio_active(desktop_audio_),
                  microphone_ && obs_source_audio_active(microphone_),
                  desktop_audio_level_.load(std::memory_order_relaxed),
                  microphone_level_.load(std::memory_order_relaxed));
  state_.SetProcessAudioLevels(game_audio_level_.load(std::memory_order_relaxed), 0.0F, 0.0F);
}

void ObsEngine::SetDashboardActive(bool active) {
  if (dashboard_active_ == active) return;
  dashboard_active_ = active;
  if (initialized_) {
    UpdateAudioMeters();
    logger_.Info(active ? "REVO dashboard active; audio metering resumed"
                        : "REVO dashboard inactive; audio metering paused (capture/audio continue)");
  }
}

void ObsEngine::UpdateAudioMeters() {
  const auto update = [&](obs_source_t* source, obs_volmeter_t*& meter,
                          std::atomic<float>& level) {
    if (!dashboard_active_ || !source) {
      if (meter) { obs_volmeter_destroy(meter); meter = nullptr; }
      level.store(0.0F, std::memory_order_relaxed);
    } else if (!meter) {
      meter = obs_volmeter_create(OBS_FADER_LOG);
      if (meter) {
        obs_volmeter_add_callback(meter, MeterUpdated, &level);
        obs_volmeter_attach_source(meter, source);
      } else {
        level.store(0.0F, std::memory_order_relaxed);
      }
    }
  };
  update(desktop_audio_, desktop_audio_meter_, desktop_audio_level_);
  update(game_audio_, game_audio_meter_, game_audio_level_);
  update(microphone_, microphone_meter_, microphone_level_);
}

bool ObsEngine::ConfigureEncoder(Error& error) {
  try {
    std::vector<std::string> available;
    const char* id = nullptr;
    for (std::size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
      if (std::string_view(id) == kObsNvencH264EncoderId ||
          std::string_view(id) == kObsNvencHevcEncoderId ||
          std::string_view(id) == "h264_texture_amf" || std::string_view(id) == "obs_qsv11_v2" ||
          std::string_view(id) == "obs_x264")
        available.emplace_back(id);
    }
    state_.SetAvailableEncoders(available);

    // The game profile is the authoritative codec switch. Override a manually
    // selected HEVC encoder outside Minecraft so normal clips and recordings
    // remain H.264; when Minecraft is detected, use the native NVENC HEVC encoder.
    const bool minecraft_profile = active_game_ == "Minecraft";
    bool minecraft_hevc_unavailable = false;
    std::string selected = config_.obs_encoder_id;
    if (minecraft_profile) {
      selected = std::string(kObsNvencHevcEncoderId);
      if (std::find(available.begin(), available.end(), selected) == available.end()) {
        minecraft_hevc_unavailable = true;
        selected.clear();
      }
    } else if (selected == "auto" || selected == kObsNvencHevcEncoderId) {
      selected.clear();
    }

    if (selected.empty() || selected == "auto") {
      for (const char* candidate : {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"}) {
        if (std::find(available.begin(), available.end(), candidate) != available.end()) {
          selected = candidate;
          break;
        }
      }
    }
    if (selected.empty() || std::find(available.begin(), available.end(), selected) == available.end())
      throw std::runtime_error(minecraft_hevc_unavailable
          ? "Minecraft was detected, but OBS NVIDIA NVENC HEVC is unavailable; no H.264 fallback encoder is available"
          : "The selected OBS encoder is unavailable: " + selected);

    if (minecraft_hevc_unavailable) {
      logger_.Warning("Minecraft detected but OBS NVENC HEVC is unavailable; explicitly falling back to H.264");
    }
    Data settings(obs_encoder_defaults(selected.c_str()), obs_data_release);
    Properties props(obs_get_encoder_properties(selected.c_str()), obs_properties_destroy);
    if (!settings || !props) throw std::runtime_error("Could not inspect OBS encoder properties");
    const bool performance = config_.encoder_quality == EncoderQuality::kPerformance;
    const bool maximum = config_.encoder_quality == EncoderQuality::kQuality;
    if (selected == kObsNvencH264EncoderId || selected == kObsNvencHevcEncoderId) {
      const auto policy = ObsNvencPolicyFor(config_.encoder_quality);
      SetList(props.get(), settings.get(), "rate_control", config_.obs_duration_priority ? "CBR" : "CQP");
      SetInt(props.get(), settings.get(), "cqp", config_.obs_cq);
      SetList(props.get(), settings.get(), "preset", policy.preset);
      SetList(props.get(), settings.get(), "tune", "hq");
      SetList(props.get(), settings.get(), "multipass", policy.multipass);
      // OBS NVENC HEVC uses the Main profile, not H.264's High profile.
      SetList(props.get(), settings.get(), "profile",
              selected == kObsNvencHevcEncoderId ? "main" : "high");
      SetInt(props.get(), settings.get(), "bf", 2);
      if (obs_properties_get(props.get(), "adaptive_quantization"))
        obs_data_set_bool(settings.get(), "adaptive_quantization", policy.adaptive_quantization);
      if (obs_properties_get(props.get(), "lookahead")) obs_data_set_bool(settings.get(), "lookahead", false);
    } else if (selected == "obs_x264") {
      SetList(props.get(), settings.get(), "rate_control", config_.obs_duration_priority ? "CBR" : "CRF");
      SetInt(props.get(), settings.get(), "crf", config_.obs_cq);
      SetList(props.get(), settings.get(), "preset", performance ? "veryfast" : maximum ? "medium" : "fast");
      SetList(props.get(), settings.get(), "profile", "high");
    } else {
      SetList(props.get(), settings.get(), "rate_control", config_.obs_duration_priority ? "CBR" : "CQP");
      for (const char* key : {"cqp", "qpi", "qpp", "qpb"}) SetInt(props.get(), settings.get(), key, config_.obs_cq);
      SetList(props.get(), settings.get(), "profile", "high");
      if (selected == "h264_texture_amf") SetList(props.get(), settings.get(), "preset", performance ? "speed" : maximum ? "quality" : "balanced");
      else SetList(props.get(), settings.get(), "target_usage", performance ? "TU7" : maximum ? "TU1" : "TU4");
    }
    if (config_.obs_duration_priority) {
      const int bitrate = config_.obs_replay_bitrate_kbps;
      if (!obs_properties_get(props.get(), "bitrate") || bitrate < 1000)
        throw std::runtime_error("Cannot fit the replay duration into the memory budget. Increase replay memory or shorten the duration.");
      SetInt(props.get(), settings.get(), "bitrate", bitrate);
      if (selected == "obs_x264") {
        obs_data_set_bool(settings.get(), "use_bufsize", true);
        SetInt(props.get(), settings.get(), "buffer_size", bitrate);
      }
      logger_.Info("Replay duration-first: " + std::to_string(bitrate) + " kbps video; memory budget=" +
                   std::to_string(config_.rolling_buffer_bytes) + " bytes (shared by recording)");
    }
    SetInt(props.get(), settings.get(), "keyint_sec", 2);
    logger_.Info("OBS encoder=" + selected + " settings=" + obs_data_get_json(settings.get()));
    std::ofstream(config_.log_path.parent_path() / "obs-encoder-settings.json") << obs_data_get_json(settings.get());
    video_encoder_ = obs_video_encoder_create(selected.c_str(), "REVO shared video encoder", settings.get(), nullptr);
    if (!video_encoder_) throw std::runtime_error("OBS encoder creation failed: " + selected);
    obs_encoder_set_video(video_encoder_, obs_get_video());
    auto audio = MakeData();
    obs_data_set_int(audio.get(), "bitrate", config_.audio_bitrate / 1000);
    audio_encoder_ = obs_audio_encoder_create("ffmpeg_aac", "REVO desktop AAC", audio.get(), 0, nullptr);
    if (!audio_encoder_) throw std::runtime_error("OBS AAC encoder creation failed");
    obs_encoder_set_audio(audio_encoder_, obs_get_audio());
    if (config_.obs_separate_audio_tracks) {
      microphone_encoder_ = obs_audio_encoder_create("ffmpeg_aac", "REVO microphone AAC", audio.get(), 1, nullptr);
      game_encoder_ = obs_audio_encoder_create("ffmpeg_aac", "REVO game AAC", audio.get(), 2, nullptr);
      discord_encoder_ = obs_audio_encoder_create("ffmpeg_aac", "REVO Discord AAC", audio.get(), 3, nullptr);
      music_encoder_ = obs_audio_encoder_create("ffmpeg_aac", "REVO Spotify AAC", audio.get(), 4, nullptr);
      if (!microphone_encoder_ || !game_encoder_ || !discord_encoder_ || !music_encoder_)
        throw std::runtime_error("OBS five-track AAC encoder creation failed");
      obs_encoder_set_audio(microphone_encoder_, obs_get_audio());
      obs_encoder_set_audio(game_encoder_, obs_get_audio());
      obs_encoder_set_audio(discord_encoder_, obs_get_audio());
      obs_encoder_set_audio(music_encoder_, obs_get_audio());
    }
    state_.SetEncoder(selected);
    if (minecraft_hevc_unavailable) {
      state_.SetEncoderStatus("Minecraft detected; NVENC HEVC is unavailable, so H.264 is active");
    } else if (selected == kObsNvencHevcEncoderId) {
      state_.SetEncoderStatus("NVIDIA NVENC HEVC active for Minecraft; H.264 is used outside Minecraft");
    } else if (selected == "obs_x264") {
      state_.SetEncoderStatus("Software x264 selected; CPU encoding is active");
    } else {
      state_.SetEncoderStatus("OBS H.264 encoder selected: " + selected);
    }
    return true;
  } catch (const std::exception& exception) {
    error = Error{ErrorComponent::kVideoEncoder, "configure OBS encoder", exception.what()};
    return false;
  }
}

bool ObsEngine::StartReplayBuffer(Error& error) {
  auto settings = MakeData();
  obs_data_set_string(settings.get(), "directory", PathToUtf8(config_.output_directory).c_str());
  obs_data_set_string(settings.get(), "format", config_.obs_filename_format.c_str());
  obs_data_set_string(settings.get(), "extension", config_.output_container.c_str());
  obs_data_set_bool(settings.get(), "allow_spaces", false);
  obs_data_set_int(settings.get(), "max_time_sec", ObsReplayRetentionSeconds(config_));
  obs_data_set_int(settings.get(), "max_size_mb", static_cast<long long>(config_.rolling_buffer_bytes / (1024 * 1024)));
  replay_ = obs_output_create("replay_buffer", "REVO replay buffer", settings.get(), nullptr);
  if (!replay_) { error = Error{ErrorComponent::kRollingBuffer, "create OBS replay", "replay_buffer output is unavailable"}; return false; }
  obs_output_set_video_encoder(replay_, video_encoder_);
  obs_output_set_audio_encoder(replay_, audio_encoder_, 0);
  if (microphone_encoder_) obs_output_set_audio_encoder(replay_, microphone_encoder_, 1);
  if (game_encoder_) obs_output_set_audio_encoder(replay_, game_encoder_, 2);
  if (discord_encoder_) obs_output_set_audio_encoder(replay_, discord_encoder_, 3);
  if (music_encoder_) obs_output_set_audio_encoder(replay_, music_encoder_, 4);
  signal_handler_connect(obs_output_get_signal_handler(replay_), "saved", ReplaySaved, this);
  signal_handler_connect(obs_output_get_signal_handler(replay_), "stop", ReplayStopped, this);
  replay_waiting_for_source_ = true;
  state_.SetStatus(CaptureStatus::kStarting, "Waiting for OBS capture");
  return true;
}

bool ObsEngine::SaveReplayClip() {
  if (shutting_down_ || save_failed_ || !replay_ || !obs_output_active(replay_) ||
      !capture_ || !obs_source_get_width(capture_)) return false;
  std::scoped_lock lock(save_mutex_);
  if (queued_saves_ + static_cast<std::size_t>(saving_) >= config_.clip_request_queue_capacity) return false;
  ++queued_saves_;
  return true;
}

void ObsEngine::ReplaySaved(void* parameter, calldata*) noexcept {
  auto& self = *static_cast<ObsEngine*>(parameter);
  self.replay_saved_.store(true, std::memory_order_release);
  if (self.window_) PostMessageW(self.window_, WM_APP + 55, 0, 0);
}

void ObsEngine::ReplayStopped(void* parameter, calldata* data) noexcept {
  auto& self = *static_cast<ObsEngine*>(parameter);
  self.replay_stop_code_.store(static_cast<int>(calldata_int(data, "code")));
  if (self.window_) PostMessageW(self.window_, WM_APP + 55, 0, 0);
}

bool ObsEngine::StartRecording(Error& error) {
  ProcessOutputSignals();
  if (shutting_down_) { error = Error{ErrorComponent::kRecordingWriter, "start OBS recording", "REVO is shutting down"}; return false; }
  if (!replay_ || !obs_output_active(replay_)) { error = Error{ErrorComponent::kRecordingWriter, "start OBS recording", "Replay encoder is not active"}; return false; }
  if (recording_ && obs_output_active(recording_)) return true;
  if (recording_) {
    obs_output_release(recording_); recording_ = nullptr;
    ProcessOutputSignals();
    recording_reservation_.RemoveEmpty(); recording_reservation_.Forget();
  }
  const std::string recording_pattern = active_game_filename_prefix_.empty()
      ? "recording_%CCYY-%MM-%DD_%hh-%mm-%ss"
      : active_game_filename_prefix_ + "-%CCYY-%MM-%DD_%hh-%mm-%ss";
  std::string filename_format;
  if (!ReserveOutputFilename(config_.recording_directory, config_.output_container,
                             recording_pattern, recording_reservation_,
                             filename_format, error,
                             ErrorComponent::kRecordingWriter)) return false;
  recording_path_ = recording_reservation_.Path();
  auto settings = MakeData();
  obs_data_set_string(settings.get(), "path", PathToUtf8(recording_path_).c_str());
  recording_ = obs_output_create("ffmpeg_muxer", "REVO recording", settings.get(), nullptr);
  if (!recording_) {
    recording_reservation_.RemoveEmpty(); recording_reservation_.Forget();
    error = Error{ErrorComponent::kRecordingWriter, "create OBS recording", "ffmpeg_muxer output is unavailable"}; return false;
  }
  obs_output_set_video_encoder(recording_, video_encoder_);
  obs_output_set_audio_encoder(recording_, audio_encoder_, 0);
  if (microphone_encoder_) obs_output_set_audio_encoder(recording_, microphone_encoder_, 1);
  if (game_encoder_) obs_output_set_audio_encoder(recording_, game_encoder_, 2);
  if (discord_encoder_) obs_output_set_audio_encoder(recording_, discord_encoder_, 3);
  if (music_encoder_) obs_output_set_audio_encoder(recording_, music_encoder_, 4);
  signal_handler_connect(obs_output_get_signal_handler(recording_), "stop", RecordingStopped, this);
  if (!obs_output_start(recording_)) {
    error = Error{ErrorComponent::kRecordingWriter, "start OBS recording", OutputError(recording_)};
    obs_output_release(recording_); recording_ = nullptr;
    recording_reservation_.RemoveEmpty(); recording_reservation_.Forget();
    return false;
  }
  recording_started_ = std::chrono::steady_clock::now();
  state_.SetRecording(true, false, recording_path_);
  logger_.Info("OBS RECORDING STARTED file=" + PathToUtf8(recording_path_));
  return true;
}

void ObsEngine::RecordingStopped(void* parameter, calldata* data) noexcept {
  auto& self = *static_cast<ObsEngine*>(parameter);
  const auto code = calldata_int(data, "code");
  self.recording_stop_event_.store(code == 0 ? 1 : static_cast<int>(code), std::memory_order_release);
  if (self.window_) PostMessageW(self.window_, WM_APP + 55, 0, 0);
}

void ObsEngine::StopRecording() {
  if (IsRecording()) {
    state_.SetRecording(false, true, recording_path_);
    obs_output_stop(recording_);
  }
}
bool ObsEngine::IsRecording() const { return recording_ && obs_output_active(recording_); }
void ObsEngine::MeterUpdated(void* parameter, const float*, const float* peak,
                             const float*) noexcept {
  const float value = obs_db_to_mul(std::max(peak[0], peak[1]));
  static_cast<std::atomic<float>*>(parameter)->store(
      std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F,
      std::memory_order_relaxed);
}




void ObsEngine::SetOutputError(obs_output* output, const char* operation, ErrorComponent component) {
  Error error{component, operation, OutputError(output)};
  if (component == ErrorComponent::kRollingBuffer) state_.SetStatus(CaptureStatus::kFailed, operation);
  state_.SetError(error);
  logger_.ErrorMessage(error);
}

void ObsEngine::ProcessOutputSignals() {
  if (replay_stop_code_.exchange(0) != 0) {
    save_failed_ = true;
    { std::scoped_lock lock(save_mutex_); queued_saves_ = 0; }
    SetOutputError(replay_, "OBS replay stopped unexpectedly; restart buffering in settings");
  }
  if (replay_saved_.exchange(false, std::memory_order_acquire)) {
    const auto path = replay_reservation_.Path();
    std::error_code ec;
    const auto bytes = path.empty() ? 0 : std::filesystem::file_size(path, ec);
    if (ec || !bytes) {
      save_failed_ = true;
      state_.SetStatus(CaptureStatus::kFailed, "OBS replay save failed");
      const Error error{ErrorComponent::kClipWriter, "finish OBS replay save",
                        "OBS reported a save, but its reserved file is missing or empty"};
      state_.SetError(error); logger_.ErrorMessage(error);
    } else {
      const auto elapsed = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - save_started_).count();
      state_.SetLastSavedClip(path, elapsed);
      logger_.Info("OBS REPLAY SAVED file=" + PathToUtf8(path));
      // A completed file is no longer an empty-file cleanup candidate.
      replay_reservation_.Forget();
    }
    { std::scoped_lock lock(save_mutex_); saving_ = false; }
    if (!save_failed_) state_.SetStatus(CaptureStatus::kBuffering, "Replay buffer live");
  }
  const int recording_event = recording_stop_event_.exchange(0, std::memory_order_acquire);
  if (recording_event) {
    state_.SetRecording(false, false);
    std::error_code ec;
    const auto bytes = recording_path_.empty() ? 0 : std::filesystem::file_size(recording_path_, ec);
    if (recording_event == 1 && !ec && bytes) {
      state_.SetLastSavedRecording(recording_path_);
      logger_.Info("OBS RECORDING SAVED file=" + PathToUtf8(recording_path_));
      recording_reservation_.Forget();
    } else {
      SetOutputError(recording_, "recording stopped with an error or an empty output", ErrorComponent::kRecordingWriter);
    }
  }
}

void ObsEngine::ProcessSaveQueue() {
  if (!replay_) return;
  const bool ready = capture_ && obs_source_get_width(capture_) > 0 && obs_output_active(replay_);
  std::scoped_lock lock(save_mutex_);
  if (queued_saves_ && !saving_ && ready && !save_failed_) {
    --queued_saves_;
    std::string filename_format;
    Error filename_error;
    if (!ReserveOutputFilename(config_.output_directory, config_.output_container,
                               config_.obs_filename_format, replay_reservation_, filename_format, filename_error,
                               ErrorComponent::kClipWriter)) {
      // Fail closed: never overwrite an existing file or repeatedly retry a bad path.
      save_failed_ = true; queued_saves_ = 0;
      state_.SetStatus(CaptureStatus::kFailed, "OBS output filename could not be reserved");
      state_.SetError(filename_error); logger_.ErrorMessage(filename_error); return;
    }
    auto settings = MakeData();
    obs_data_set_string(settings.get(), "format", filename_format.c_str());
    obs_output_update(replay_, settings.get());
    saving_ = true;
    save_started_ = std::chrono::steady_clock::now();
    state_.SetStatus(CaptureStatus::kSaving, "Saving OBS replay");
    calldata_t data{};
    const bool requested = proc_handler_call(obs_output_get_proc_handler(replay_), "save", &data);
    calldata_free(&data);
    if (!requested) {
      saving_ = false; save_failed_ = true; queued_saves_ = 0;
      SetOutputError(replay_, "request OBS replay save");
    }
  }
  if (saving_ && !save_failed_ &&
      std::chrono::steady_clock::now() - save_started_ > std::chrono::seconds(30)) {
    // Stock OBS has no replay-save error signal. Do not issue another request or
    // delete the reservation while its mux thread might still own the file.
    save_failed_ = true; queued_saves_ = 0;
    state_.SetStatus(CaptureStatus::kFailed, "OBS replay save timed out; restart buffering");
    const Error error{ErrorComponent::kClipWriter, "save OBS replay",
        "Save did not complete within 30 seconds. Check disk space, folder permissions and the OBS log. "
        "Restart buffering in settings before saving again; partial media is preserved."};
    state_.SetError(error); logger_.ErrorMessage(error);
  }
  if (!saving_ && ready && state_.Snapshot().status != CaptureStatus::kFailed)
    state_.SetStatus(CaptureStatus::kBuffering, "OBS replay buffer live");
  if (!ready && state_.Snapshot().status != CaptureStatus::kFailed)
    state_.SetStatus(CaptureStatus::kStarting, "Waiting for game capture; select Display for unsupported games");
}

void ObsEngine::Tick() {
  if (!initialized_ || !replay_) return;
  ProcessOutputSignals();

  // Automatically follow the first supported game process. The process list is
  // sampled once per second, never on the capture/encoding thread.
  const auto now_for_game = std::chrono::steady_clock::now();
  if (now_for_game >= next_game_detection_ && !IsRecording()) {
    next_game_detection_ = now_for_game + std::chrono::seconds(1);
    std::string detected_game;
    std::string folder_name;
    std::string filename_prefix;
    std::vector<std::string> executables;
    std::string detected_executable;

    HWND foreground_window = GetForegroundWindow();
    DWORD foreground_pid = 0;
    if (foreground_window) GetWindowThreadProcessId(foreground_window, &foreground_pid);

    struct WindowProcessLists {
      std::vector<DWORD> visible;
      std::vector<DWORD> fullscreen;
    } window_processes;
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
      if (!IsWindowVisible(hwnd)) return TRUE;
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (!pid) return TRUE;
      auto& lists = *reinterpret_cast<WindowProcessLists*>(parameter);
      if (std::find(lists.visible.begin(), lists.visible.end(), pid) == lists.visible.end())
        lists.visible.push_back(pid);
      if (IsFullscreenWindow(hwnd) &&
          std::find(lists.fullscreen.begin(), lists.fullscreen.end(), pid) == lists.fullscreen.end())
        lists.fullscreen.push_back(pid);
      return TRUE;
    }, reinterpret_cast<LPARAM>(&window_processes));

    // Existing titles keep their familiar names. Unknown fullscreen apps use
    // their executable's safe slug for the folder and filename prefix.
    const auto consider_process = [&](const PROCESSENTRY32W& process) {
      // Each candidate must completely replace the prior candidate's profile;
      // otherwise a foreground known game could block a better-scoring fullscreen app.
      detected_game.clear();
      folder_name.clear();
      filename_prefix.clear();
      detected_executable.clear();
      executables.clear();

      std::wstring exe = process.szExeFile;
      std::transform(exe.begin(), exe.end(), exe.begin(),
                     [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
      const auto executable = PathToUtf8(std::filesystem::path(process.szExeFile));
      const auto set_profile = [&](const char* game, const char* folder,
                                   const char* prefix,
                                   std::initializer_list<const char*> aliases) {
        detected_game = game;
        folder_name = folder;
        filename_prefix = prefix;
        detected_executable = executable;
        executables.clear();
        executables.emplace_back(executable);
        for (const auto* alias : aliases) {
          if (std::find(executables.begin(), executables.end(), alias) == executables.end())
            executables.emplace_back(alias);
        }
      };
      if (exe == L"javaw.exe" || exe == L"minecraft.windows.exe") {
        set_profile("Minecraft", "minecraft", "minecraft",
                    {"javaw.exe", "minecraft.windows.exe"});
      } else if (exe == L"fortnite.exe" || exe == L"fortniteclient-win64-shipping.exe" ||
                 exe == L"fortniteclient-win64-shipping_eac_eos.exe") {
        set_profile("Fortnite", "fortnite", "fortnite",
                    {"fortnite.exe", "fortniteclient-win64-shipping.exe",
                     "fortniteclient-win64-shipping_eac_eos.exe"});
      } else if (exe == L"valorant.exe" || exe == L"valorant-win64-shipping.exe") {
        set_profile("Valorant", "valorant", "valorant",
                    {"valorant.exe", "valorant-win64-shipping.exe"});
      } else if (exe == L"gta5.exe" || exe == L"gta5_enhanced.exe" ||
                 exe == L"playgtav.exe") {
        set_profile("GTA 5", "gta5", "gta5",
                    {"gta5.exe", "gta5_enhanced.exe", "playgtav.exe"});
      } else if (exe.starts_with(L"fivem_") && exe.find(L"gtaprocess.exe") != std::wstring::npos) {
        set_profile("GTA 5", "gta5", "gta5",
                    {"gta5.exe", "gta5_enhanced.exe", "playgtav.exe"});
      } else if (exe == L"cs2.exe") {
        set_profile("Counter-Strike 2", "cs2", "cs2", {"cs2.exe"});
      } else if (exe == L"r5apex.exe") {
        set_profile("Apex Legends", "apex", "apex", {"r5apex.exe"});
      } else if (exe == L"robloxplayerbeta.exe") {
        set_profile("Roblox", "roblox", "roblox", {"robloxplayerbeta.exe"});
      } else if (exe == L"overwatch.exe") {
        set_profile("Overwatch", "overwatch", "overwatch", {"overwatch.exe"});
      } else if (exe == L"rocketleague.exe") {
        set_profile("Rocket League", "rocket-league", "rocket-league", {"rocketleague.exe"});
      } else if (exe == L"eldenring.exe") {
        set_profile("Elden Ring", "elden-ring", "elden-ring", {"eldenring.exe"});
      } else if (exe == L"helldivers2.exe") {
        set_profile("Helldivers 2", "helldivers2", "helldivers2", {"helldivers2.exe"});
      } else if (exe == L"deadbydaylight-win64-shipping.exe") {
        set_profile("Dead by Daylight", "dead-by-daylight", "dead-by-daylight",
                    {"deadbydaylight-win64-shipping.exe"});
      } else if (exe == L"destiny2.exe") {
        set_profile("Destiny 2", "destiny2", "destiny2", {"destiny2.exe"});
      } else if (exe == L"rainbowsix.exe" || exe == L"rainbowsix_vulkan.exe" ||
                 exe == L"rainbow6.exe") {
        set_profile("Rainbow Six Siege", "rainbow-six", "rainbow-six",
                    {"rainbowsix.exe", "rainbowsix_vulkan.exe", "rainbow6.exe"});
      } else if (exe == L"dota2.exe") {
        set_profile("Dota 2", "dota2", "dota2", {"dota2.exe"});
      } else if (exe == L"tslgame.exe" || exe == L"pubg.exe") {
        set_profile("PUBG", "pubg", "pubg", {"tslgame.exe", "pubg.exe"});
      } else if (exe == L"discovery.exe" || exe == L"thefinals.exe") {
        set_profile("THE FINALS", "the-finals", "the-finals", {"discovery.exe", "thefinals.exe"});
      } else if (exe == L"marvel-win64-shipping.exe") {
        set_profile("Marvel Rivals", "marvel-rivals", "marvel-rivals",
                    {"marvel-win64-shipping.exe"});
      } else if (exe == L"bf2042.exe") {
        set_profile("Battlefield 2042", "battlefield-2042", "battlefield-2042", {"bf2042.exe"});
      }

      const bool fullscreen_process =
          std::find(window_processes.fullscreen.begin(), window_processes.fullscreen.end(),
                    process.th32ProcessID) != window_processes.fullscreen.end();
      if (detected_game.empty() && fullscreen_process && !IsIgnoredFullscreenExecutable(exe)) {
        const auto executable_path = std::filesystem::path(process.szExeFile);
        const auto display_name = PathToUtf8(executable_path.stem());
        const auto slug = MakeApplicationProfileSlug(PathToUtf8(executable_path.filename()));
        set_profile(display_name.c_str(), slug.c_str(), slug.c_str(), {});
      }
      return !detected_game.empty();
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W process{};
      process.dwSize = sizeof(process);
      // Pass 1: resolve the foreground application's process before all others.
      if (foreground_pid && Process32FirstW(snapshot, &process)) {
        do {
          if (process.th32ProcessID == foreground_pid) {
            consider_process(process);
            break;
          }
        } while (Process32NextW(snapshot, &process));
      }

      struct Candidate {
        PROCESSENTRY32W process{};
        int score = -1;
      } best;
      process = {};
      process.dwSize = sizeof(process);
      if (Process32FirstW(snapshot, &process)) {
        do {
          std::wstring exe = process.szExeFile;
          std::transform(exe.begin(), exe.end(), exe.begin(),
                         [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
          int score = -1;
          const bool fullscreen_process =
              std::find(window_processes.fullscreen.begin(), window_processes.fullscreen.end(),
                        process.th32ProcessID) != window_processes.fullscreen.end();
          // Minecraft remains a special profile, but a background Java process
          // must not steal the active profile from another fullscreen application.
          if (exe == L"javaw.exe")
            score = (fullscreen_process || process.th32ProcessID == foreground_pid) ? 240 : 60;
          else if (exe == L"minecraft.windows.exe")
            score = (fullscreen_process || process.th32ProcessID == foreground_pid) ? 245 : 65;
          else if (exe == L"valorant.exe" || exe == L"fortnite.exe" ||
                   exe == L"playgtav.exe") score = 35; // Launcher processes.
          else if (exe == L"valorant-win64-shipping.exe" ||
                   exe == L"fortniteclient-win64-shipping.exe" ||
                   exe == L"fortniteclient-win64-shipping_eac_eos.exe" ||
                   exe == L"gta5.exe" || exe == L"gta5_enhanced.exe" ||
                   exe == L"cs2.exe" || exe == L"r5apex.exe" ||
                   exe == L"robloxplayerbeta.exe" || exe == L"overwatch.exe" ||
                   exe == L"rocketleague.exe" || exe == L"eldenring.exe" ||
                   exe == L"helldivers2.exe" || exe == L"deadbydaylight-win64-shipping.exe" ||
                   exe == L"destiny2.exe" || exe == L"rainbowsix.exe" ||
                   exe == L"rainbowsix_vulkan.exe" || exe == L"rainbow6.exe" ||
                   exe == L"dota2.exe" || exe == L"tslgame.exe" || exe == L"pubg.exe" ||
                   exe == L"discovery.exe" || exe == L"thefinals.exe" ||
                   exe == L"marvel-win64-shipping.exe" || exe == L"bf2042.exe" ||
                   (exe.starts_with(L"fivem_") &&
                    exe.find(L"gtaprocess.exe") != std::wstring::npos))
            score = 70;
          else if (fullscreen_process && !IsIgnoredFullscreenExecutable(exe))
            score = 90;  // Any previously unknown fullscreen app gets its own profile.

          const bool has_visible_window =
              std::find(window_processes.visible.begin(), window_processes.visible.end(),
                        process.th32ProcessID) != window_processes.visible.end();
          if (score >= 0 && has_visible_window) score += 100;
          // Prefer the focused game itself, but do not let a launcher
          // outrank a visible game executable such as VALORANT-Win64-Shipping.exe.
          if (process.th32ProcessID == foreground_pid) {
            score += score >= 60 ? 40 : 5;
          }
          if (score > best.score) {
            best.process = process;
            best.score = score;
          }
        } while (Process32NextW(snapshot, &process));
      }
      if (best.score >= 0) consider_process(best.process);
      CloseHandle(snapshot);
    }

    // Keep the game-audio source synchronized independently of profile changes.
    // A previous ConfigureAudio failure must not permanently pin audio to an
    // old executable just because active_game_ already matches the detected game.
    if (!detected_executable.empty()) {
      state_.SetDetectedGameAudioApplication(detected_executable);
      const std::string desired_audio_selector = "::" + detected_executable;
      if (config_.game_audio_window != desired_audio_selector) {
        config_.game_audio_window = desired_audio_selector;
        game_audio_selector_resolved_ = false;
        next_game_audio_retry_ = now_for_game;
        if (game_audio_) {
          obs_set_output_source(1, nullptr);
          if (game_audio_meter_) {
            obs_volmeter_destroy(game_audio_meter_);
            game_audio_meter_ = nullptr;
          }
          game_audio_level_.store(0.0F, std::memory_order_relaxed);
          obs_source_release(game_audio_);
          game_audio_ = nullptr;
        }
      }

      // WASAPI may not list a newly opened process until it starts producing
      // audio. Retry the stable executable selector until the exact Track 3
      // source appears instead of leaving the game audio bound to an old app.
      if (!game_audio_selector_resolved_ && now_for_game >= next_game_audio_retry_) {
        next_game_audio_retry_ = now_for_game + std::chrono::seconds(2);
        try {
          ConfigureAudio(config_);
          if (game_audio_selector_resolved_) {
            logger_.Info("Automatic Track 3 game-audio selector resolved to " +
                         desired_audio_selector);
          } else {
            logger_.Info("Waiting for WASAPI process audio session: " + desired_audio_selector);
          }
        } catch (const std::exception& audio_exception) {
          logger_.Warning(std::string("Automatic game audio reconfiguration failed: ") +
                          audio_exception.what());
        }
      }
    }

    // Never switch a profile while a clip save or manual recording owns the output.
    bool output_busy = false;
    { std::scoped_lock lock(save_mutex_); output_busy = saving_ || queued_saves_; }
    if (!output_busy && !IsRecording() && detected_game != active_game_) {
      if (now_for_game >= next_source_refresh_) {
        RefreshSources();
        next_source_refresh_ = now_for_game + std::chrono::seconds(3);
      }
      const SourceChoice* target = nullptr;
      if (!detected_game.empty()) {
        for (const auto& choice : games_) {
          std::string value = choice.value;
          std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
          for (const auto& exe : executables) {
            if (value.find(exe) != std::string::npos) { target = &choice; break; }
          }
          if (target) break;
        }
      }

      // Minecraft is the only 240 FPS profile. Other supported games and the
      // no-game fallback use 60 FPS. Bitrate values are kbps, as expected by OBS.
      const bool minecraft_profile = detected_game == "Minecraft";
      const std::uint32_t profile_fps = minecraft_profile ? 240U : 60U;
      const int profile_bitrate_kbps = minecraft_profile ? 35'000 : 20'000;

      StopOutput(replay_);
      if (replay_) { obs_output_release(replay_); replay_ = nullptr; }
      auto release_encoder = [](obs_encoder_t*& encoder) {
        if (encoder) { obs_encoder_release(encoder); encoder = nullptr; }
      };
      release_encoder(video_encoder_);
      release_encoder(audio_encoder_);
      release_encoder(game_encoder_);
      release_encoder(microphone_encoder_);
      release_encoder(discord_encoder_);
      release_encoder(music_encoder_);

      active_game_ = detected_game;
      active_game_filename_prefix_ = filename_prefix;

      config_.target_fps = profile_fps;
      config_.obs_replay_bitrate_kbps = profile_bitrate_kbps;
      config_.obs_duration_priority = true;
      const double retained_seconds = std::max(1.0, config_.clip_duration_seconds) + 15.0;
      const double audio_kbps = static_cast<double>(config_.audio_bitrate) / 1000.0 *
          (config_.obs_separate_audio_tracks ? 5.0 : 1.0);
      const double required_bytes = std::ceil(
          (static_cast<double>(profile_bitrate_kbps) + audio_kbps) * 1000.0 / 8.0 *
          retained_seconds * 1.25);
      config_.rolling_buffer_bytes = static_cast<std::size_t>(
          std::max(32.0 * 1024.0 * 1024.0, required_bytes));

      const auto profile_folder = folder_name.empty()
          ? std::filesystem::path{}
          : std::filesystem::path(folder_name);
      config_.output_directory = clips_root_ / profile_folder;
      config_.obs_filename_format = filename_prefix.empty()
          ? "clip_%CCYY-%MM-%DD_%hh-%mm-%ss"
          : filename_prefix + "-%CCYY-%MM-%DD_%hh-%mm-%ss";
      config_.recording_directory = recordings_root_ / profile_folder;

      std::error_code directory_error;
      std::filesystem::create_directories(config_.output_directory, directory_error);
      if (directory_error) {
        logger_.Warning("Automatic game clip folder could not be created: " + directory_error.message());
      }
      std::error_code recording_directory_error;
      std::filesystem::create_directories(config_.recording_directory, recording_directory_error);
      if (recording_directory_error) {
        logger_.Warning("Automatic game recording folder could not be created: " +
                        recording_directory_error.message());
      }
      if (!directory_error) {
        if ((detected_game.empty() && selected_source_id_ != 0) ||
            (!detected_game.empty() &&
             (config_.target_mode != CaptureTargetMode::kGameWindow || !target ||
              selected_source_id_ != target->id))) {
          Error capture_error;
          if (!ConfigureCapture(CaptureTargetMode::kGameWindow,
                                target ? target->label : std::string{}, capture_error)) {
            state_.SetError(capture_error);
            logger_.ErrorMessage(capture_error);
          }
        }
        if (!detected_game.empty() && !target) {
          // OBS may not enumerate a game's window even while automatic
          // fullscreen capture is successfully hooking it. Reflect the
          // detected game in the card instead of the generic fallback label.
          state_.SetTarget(CaptureTargetMode::kGameWindow,
                           detected_game + " (automatic fullscreen capture)",
                           selected_source_id_);
        }

        // Reconfigure libobs's video clock only after stopping the replay output
        // and releasing its encoders; this changes clip FPS, not the game's FPS.
        obs_video_info video{};
        if (!obs_get_video_info(&video)) {
          const Error video_error{ErrorComponent::kGraphics, "read OBS video profile",
                                  "OBS video settings are unavailable during game profile switch"};
          state_.SetError(video_error);
          logger_.ErrorMessage(video_error);
        } else {
          const auto graphics_path = PathToUtf8(RuntimeRoot() / "bin/64bit/libobs-d3d11.dll");
          video.graphics_module = graphics_path.c_str();
          video.fps_num = profile_fps;
          video.fps_den = 1;
          if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
            const Error video_error{ErrorComponent::kGraphics, "apply game FPS profile",
                                    "OBS could not switch to the selected game's clip frame rate"};
            state_.SetError(video_error);
            logger_.ErrorMessage(video_error);
          } else {
            UpdateSceneLayout();
            Error output_error;
            if (!ConfigureEncoder(output_error) || !StartReplayBuffer(output_error)) {
              state_.SetError(output_error);
              logger_.ErrorMessage(output_error);
            } else {
              logger_.Info("Automatic game profile: " +
                  (active_game_.empty() ? std::string("default") : active_game_) +
                  " -> " + std::to_string(profile_fps) + " FPS, " +
                  std::to_string(profile_bitrate_kbps) + " kbps, " +
                  PathToUtf8(config_.output_directory));
            }
          }
        }
      }
    }
  }
  const bool refresh_pending_sources = (!pending_game_label_.empty() || !pending_overlay_label_.empty()) &&
                                      std::chrono::steady_clock::now() >= next_source_refresh_;
  if (refresh_pending_sources) RefreshSources();
  if (!pending_game_label_.empty() && refresh_pending_sources) {
    const auto label = pending_game_label_;
    const auto old_title = label.substr(0, label.find("  ["));
    if (std::count_if(games_.begin(), games_.end(), [&](const auto& choice) { return choice.label == label || choice.label.find(old_title) != std::string::npos; }) == 1) {
      Error error;
      if (!ConfigureCapture(CaptureTargetMode::kGameWindow, label, error)) { state_.SetError(error); logger_.ErrorMessage(error); }
    }
  }
  if (!pending_overlay_label_.empty() && refresh_pending_sources) {
    const auto old_title = pending_overlay_label_.substr(0, pending_overlay_label_.find("  ["));
    const SourceChoice* match = nullptr;
    for (const auto& choice : overlay_windows_) {
      if (choice.label != pending_overlay_label_ && choice.label.find(old_title) == std::string::npos) continue;
      if (match) { match = nullptr; break; }
      match = &choice;
    }
    if (match && overlay_) {
      Data settings(obs_source_get_settings(overlay_), obs_data_release);
      obs_data_set_string(settings.get(), "window", match->value.c_str());
      obs_source_update(overlay_, settings.get());
      logger_.Info("OBS live overlay acquired: " + match->label);
      pending_overlay_label_.clear();
      state_.SetSettingsStatus(false, "Live overlay source acquired");
    }
  }
  if (refresh_pending_sources) next_source_refresh_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  // Do not switch display-capture backends at runtime. WGC is selected when
  // the source is created; an unsupported WGC source remains visibly unready
  // instead of silently changing to DXGI.
  const bool source_ready = capture_ && obs_source_get_width(capture_) > 0;
  if (capture_was_ready_ && !source_ready && config_.target_mode == CaptureTargetMode::kGameWindow) {
    StopOutput(replay_);
    replay_waiting_for_source_ = true;
    logger_.Warning("OBS game capture closed or lost its target; replay history cleared while waiting for the game");
  }
  capture_was_ready_ = source_ready;
  if (native_resolution_pending_ && source_ready) {
    obs_video_info native{};
    obs_get_video_info(&native);
    const auto module = PathToUtf8(RuntimeRoot() / "bin/64bit/libobs-d3d11.dll");
    native.graphics_module = module.c_str();
    native.base_width = native.output_width = obs_source_get_width(capture_) & ~1U;
    native.base_height = native.output_height = obs_source_get_height(capture_) & ~1U;
    if (obs_reset_video(&native) != OBS_VIDEO_SUCCESS) {
      const Error error{ErrorComponent::kGraphics, "set native OBS resolution", "OBS could not configure the source resolution before encoding"};
      state_.SetError(error); logger_.ErrorMessage(error); replay_waiting_for_source_ = false; return;
    }
    obs_encoder_set_video(video_encoder_, obs_get_video());
    native_resolution_pending_ = false;
    logger_.Info("OBS native resolution locked: " + std::to_string(native.output_width) + "x" + std::to_string(native.output_height));
  }
  if (replay_waiting_for_source_ && source_ready) {
    replay_waiting_for_source_ = false;
    if (!obs_output_start(replay_)) { SetOutputError(replay_, "start OBS replay"); return; }
    replay_started_ = std::chrono::steady_clock::now();
  }
  UpdateSceneLayout();
  ProcessSaveQueue();
  state_.SetCaptureMetrics(obs_get_active_fps(), 0.0, obs_get_total_frames(),
                           obs_get_lagged_frames(), video_output_get_skipped_frames(obs_get_video()), 0, 0, 0.0);
  state_.SetAudio(desktop_audio_ && obs_source_audio_active(desktop_audio_),
                  microphone_ && obs_source_audio_active(microphone_),
                  desktop_audio_level_.load(std::memory_order_relaxed),
                  microphone_level_.load(std::memory_order_relaxed));
  state_.SetProcessAudioLevels(game_audio_level_.load(std::memory_order_relaxed), 0.0F, 0.0F);
  const auto buffered = obs_output_active(replay_) ? std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_started_).count() : 0.0;
  state_.SetRollingMetrics(std::min(buffered, config_.clip_duration_seconds), 0, 0);
  if (IsRecording()) state_.SetRecording(true, false, recording_path_,
      std::chrono::duration<double>(std::chrono::steady_clock::now() - recording_started_).count());
}

void ObsEngine::StopOutput(obs_output* output) noexcept {
  if (!output || !obs_output_active(output)) return;
  obs_output_stop(output);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (obs_output_active(output) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (obs_output_active(output)) obs_output_force_stop(output);
}

void ObsEngine::DrainAcceptedSaves() noexcept {
  // The UI has already stopped accepting commands. Keep capture/encoding live
  // long enough to finish accepted requests before releasing the OBS outputs.
  try {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
      ProcessOutputSignals();
      bool pending;
      { std::scoped_lock lock(save_mutex_); pending = saving_ || queued_saves_; }
      if (!pending || save_failed_) break;
      if (!replay_ || !obs_output_active(replay_) || !capture_ || !obs_source_get_width(capture_) ||
          std::chrono::steady_clock::now() >= deadline) {
        const Error error{ErrorComponent::kClipWriter, "finish queued clips on shutdown",
            "Capture became unavailable or the shutdown save deadline expired; unfinished requests could not be saved"};
        state_.SetError(error); logger_.ErrorMessage(error); break;
      }
      ProcessSaveQueue();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  } catch (...) { OutputDebugStringA("REVO: exception while draining OBS replay saves\n"); }
}

void ObsEngine::Shutdown() noexcept {
  shutting_down_ = true;
  DrainAcceptedSaves();
  StopOutput(recording_);
  StopOutput(replay_);
  try { ProcessOutputSignals(); } catch (...) {}
  if (recording_) { obs_output_release(recording_); recording_ = nullptr; }
  if (replay_) { obs_output_release(replay_); replay_ = nullptr; }
  // Output release joins stock OBS's replay mux thread. Consume any final signal
  // only after joining, and never hold save_mutex_ while calling release.
  try { ProcessOutputSignals(); } catch (...) {}
  replay_reservation_.RemoveEmpty(); replay_reservation_.Forget();
  recording_reservation_.RemoveEmpty(); recording_reservation_.Forget();
  if (video_encoder_) { obs_encoder_release(video_encoder_); video_encoder_ = nullptr; }
  if (audio_encoder_) { obs_encoder_release(audio_encoder_); audio_encoder_ = nullptr; }
  if (game_encoder_) { obs_encoder_release(game_encoder_); game_encoder_ = nullptr; }
  if (microphone_encoder_) { obs_encoder_release(microphone_encoder_); microphone_encoder_ = nullptr; }
  if (discord_encoder_) { obs_encoder_release(discord_encoder_); discord_encoder_ = nullptr; }
  if (music_encoder_) { obs_encoder_release(music_encoder_); music_encoder_ = nullptr; }
  if (desktop_audio_meter_) { obs_volmeter_destroy(desktop_audio_meter_); desktop_audio_meter_ = nullptr; }
  if (game_audio_meter_) { obs_volmeter_destroy(game_audio_meter_); game_audio_meter_ = nullptr; }
  if (microphone_meter_) { obs_volmeter_destroy(microphone_meter_); microphone_meter_ = nullptr; }
  desktop_audio_level_.store(0.0F, std::memory_order_relaxed);
  game_audio_level_.store(0.0F, std::memory_order_relaxed);
  microphone_level_.store(0.0F, std::memory_order_relaxed);
  if (initialized_) for (std::uint32_t i = 0; i < 6; ++i) obs_set_output_source(i, nullptr);
  if (scene_) { obs_scene_release(scene_); scene_ = nullptr; capture_item_ = nullptr; overlay_item_ = nullptr; }
  if (capture_) { obs_source_release(capture_); capture_ = nullptr; }
  if (overlay_) { obs_source_release(overlay_); overlay_ = nullptr; }
  if (desktop_audio_) { obs_source_release(desktop_audio_); desktop_audio_ = nullptr; }
  if (game_audio_) { obs_source_release(game_audio_); game_audio_ = nullptr; }
  if (discord_audio_) { obs_source_release(discord_audio_); discord_audio_ = nullptr; }
  if (music_audio_) { obs_source_release(music_audio_); music_audio_ = nullptr; }
  if (microphone_) { obs_source_release(microphone_); microphone_ = nullptr; }
  if (initialized_) obs_shutdown();
  if (!core_data_path_.empty()) { obs_remove_data_path(core_data_path_.c_str()); core_data_path_.clear(); }
  base_set_log_handler(nullptr, nullptr);
  initialized_ = false;
  replay_waiting_for_source_ = false;
  native_resolution_pending_ = false;
  capture_was_ready_ = false;
  pending_game_label_.clear();
  pending_overlay_label_.clear();
  layout_width_ = layout_height_ = 0;
  { std::scoped_lock lock(save_mutex_); queued_saves_ = 0; saving_ = false; }
}
}  // namespace klip
