#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#if defined(REVO_USE_LIBOBS)
#include "revo/obs/obs_engine.h"
#else
#include "revo/audio/audio_pipeline.h"
#include "revo/buffer/rolling_media_buffer.h"
#include "revo/capture/graphics_capture.h"
#include "revo/clips/clip_writer.h"
#include "revo/core/application_state.h"
#include "revo/core/config.h"
#include "revo/core/logger.h"
#include "revo/encoding/audio_encoder.h"
#include "revo/encoding/video_encoder.h"
#include "revo/media/packet_router.h"
#include "revo/recording/recording_writer.h"
#endif
#include "revo/core/application_state.h"
#include "revo/core/config.h"
#include "revo/core/logger.h"
#include "revo/graphics/d3d_device.h"
#include "revo/platform/hotkeys.h"
#include "revo/platform/win32_window.h"
#include "revo/ui/imgui_host.h"
#include "revo/ui/main_panel.h"

namespace revo {

class REVOApplication {
 public:
  REVOApplication(AppConfig config, std::filesystem::path settings_path);
  ~REVOApplication() noexcept;

  REVOApplication(const REVOApplication&) = delete;
  REVOApplication& operator=(const REVOApplication&) = delete;

  bool Initialize(HINSTANCE instance, int show_command, Error& error, bool register_hotkeys = true,
                  std::optional<std::uint32_t> graphics_vendor_id = std::nullopt,
                  std::optional<D3D_FEATURE_LEVEL> graphics_feature_level = std::nullopt,
                  bool force_event_query_sync = false,
                  bool inject_runtime_encoder_failure = false);
  int Run();
  int RunCaptureAcceptanceTest(std::string required_encoder = {},
                               std::uint32_t recording_duration_seconds = 10,
                               bool exercise_source_switch = false,
                               std::string alternate_window_title = {},
                               bool allow_encoder_recovery = false,
                               bool exercise_hotkey_actions = false,
                               bool exercise_global_hotkey_input = false);
  int RunSettingsAcceptanceTest();
  int RunIdleAcceptanceTest();
#if defined(REVO_USE_LIBOBS)
  int RunSaveShutdownAcceptanceTest();
  int RunSaveFailureRecoveryAcceptanceTest();
#if defined(REVO_ENABLE_TEST_HOOKS)
  int RunBackgroundBenchmark();
#endif
#endif
  void Shutdown() noexcept;

 private:
  void HandleHotkey(int id);
  UiCommands BuildUiCommands();
  void UpdateRollingMetrics();
  bool PersistSettings(const AppConfig& config, bool restart_required);
  bool ApplySettings(const AppConfig& config);
  bool StartRecording(Error& error);
  bool StartMediaPipeline(const AppConfig& config, Error& error);
  void StopMediaPipeline() noexcept;
  bool RestartMediaPipeline(const AppConfig& config, Error& error);

  AppConfig config_;
  std::filesystem::path settings_path_;
  ApplicationState state_;
  Logger logger_;
#if defined(REVO_USE_LIBOBS)
  ObsEngine engine_;
#else
  RollingMediaBuffer media_buffer_;
  RecordingWriter recording_writer_;
  PacketRouter packet_router_;
  AudioEncoder audio_encoder_;
  VideoEncoder video_encoder_;
  GraphicsCapture capture_;
  AudioPipeline audio_;
  ClipWriter clip_writer_;
#endif
  Win32Window window_;
  D3dDevice graphics_;
  ImGuiHost imgui_;
  Hotkeys hotkeys_;
  MainPanel panel_;
  std::atomic<bool> initialized_{false};
#if defined(REVO_USE_LIBOBS) && defined(REVO_ENABLE_TEST_HOOKS)
  bool background_benchmark_ = false;
#endif
#if !defined(REVO_USE_LIBOBS)
  bool force_event_query_sync_ = false;
#if defined(REVO_ENABLE_TEST_HOOKS)
  bool inject_runtime_encoder_failure_ = false;
#endif
#endif
};

}  // namespace revo
