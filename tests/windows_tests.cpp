#include <Windows.h>
#include <ksmedia.h>

#include <cstring>
#include <iostream>
#include <vector>

#include "klip/audio/audio_pipeline.h"
#include "klip/core/path_text.h"
#include "klip/audio/audio_recovery_policy.h"
#include "klip/buffer/rolling_media_buffer.h"
#include "klip/media/mp4_muxer.h"
#include "klip/media/packet_router.h"
#include "klip/platform/hotkeys.h"
#include "klip/platform/win32_window.h"
#include "klip/recording/recording_writer.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace {
int failures = 0;
#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      ++failures;                            \
    }                                        \
  } while (false)

void TestNativeCaptureDefaults() {
  const klip::AppConfig config;
  CHECK(!config.capture_border);
  CHECK(config.capture_cursor);
  CHECK(config.output_container == "mkv");
  CHECK(config.obs_separate_audio_tracks);
  CHECK(config.desktop_audio_enabled);
  CHECK(config.microphone_enabled);
  CHECK(config.discord_audio_window == "::Discord.exe");
  CHECK(config.music_audio_window == "::Spotify.exe");
  CHECK(config.obs_display_method == 1);
  CHECK(config.obs_limit_game_capture_fps);
}

void TestWaveFormats() {
  CHECK(klip::ResolveWaveSampleFormat(nullptr) == AV_SAMPLE_FMT_NONE);
  WAVEFORMATEXTENSIBLE wave{};
  wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wave.Format.cbSize = sizeof(wave) - sizeof(WAVEFORMATEX);
  wave.Format.wBitsPerSample = 32;
  wave.Samples.wValidBitsPerSample = 24;
  wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_S32);
  wave.Samples.wValidBitsPerSample = 32;
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_S32);
  wave.Samples.wValidBitsPerSample = 33;
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_NONE);
  wave.Samples.wValidBitsPerSample = 32;
  wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_FLT);
  wave.Format.cbSize = 0;
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_NONE);
  wave.Format.wFormatTag = WAVE_FORMAT_PCM;
  wave.Format.wBitsPerSample = 16;
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_S16);
  wave.Format.wBitsPerSample = 24;
  wave.Format.nChannels = 2;
  wave.Format.nBlockAlign = 8;  // Include per-frame padding to exercise stride handling.
  CHECK(klip::ResolveWaveSampleFormat(&wave.Format) == AV_SAMPLE_FMT_S32);
  const std::uint8_t packed[] = {
      0x01, 0x00, 0x00,  // +1
      0xFF, 0xFF, 0xFF,  // -1
      0xAA, 0xBB,        // ignored frame padding
      0xFF, 0xFF, 0x7F,  // +8388607
      0x00, 0x00, 0x80,  // -8388608
      0xCC, 0xDD,        // ignored frame padding
  };
  std::vector<std::int32_t> converted;
  CHECK(klip::ConvertPackedPcm24ToS32(&wave.Format, packed, 2, converted));
  CHECK(converted.size() == 4);
  CHECK(converted[0] == 256);
  CHECK(converted[1] == -256);
  CHECK(converted[2] == 2147483392);
  CHECK(converted[3] == (-2147483647 - 1));
  CHECK(!klip::ConvertPackedPcm24ToS32(&wave.Format, nullptr, 2, converted));
}

void TestAudioRecoveryBackoff() {
  using clock = std::chrono::steady_clock;
  const auto now = clock::time_point{} + std::chrono::seconds(10);
  const auto retry_after = now + std::chrono::seconds(2);
  CHECK(!klip::audio_detail::ShouldRetryCapture(false, true, false, now, {}));
  CHECK(!klip::audio_detail::ShouldRetryCapture(true, false, true, now, {}));
  CHECK(!klip::audio_detail::ShouldRetryCapture(true, true, true, now, retry_after));
  CHECK(klip::audio_detail::ShouldRetryCapture(true, true, true, retry_after, retry_after));
  CHECK(klip::audio_detail::ShouldRetryCapture(true, false, false, retry_after, retry_after));
}

void TestModifierFreeHotkeyRegistration() {
  HWND app = CreateWindowExW(0, L"STATIC", L"Klip single-key test", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, nullptr, nullptr);
  CHECK(app != nullptr);
  if (!app) return;
  klip::HotkeyConfig config;
  config.save_modifiers = klip::HotkeyConfig::kNoRepeat;
  config.save_virtual_key = VK_F20;
  klip::Hotkeys hotkeys;
  klip::Error error;
  CHECK(hotkeys.Register(app, config, error));
  CHECK(hotkeys.SaveRegistered());
  hotkeys.Unregister();
  DestroyWindow(app);
}

void TestPartialHotkeyRegistration() {
  HWND app = CreateWindowExW(0, L"STATIC", L"Klip test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             nullptr, nullptr);
  HWND blocker = CreateWindowExW(0, L"STATIC", L"Klip blocker", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, nullptr, nullptr);
  CHECK(app && blocker);
  klip::HotkeyConfig config;
  config.save_virtual_key = VK_F22;
  config.record_virtual_key = VK_F23;
  config.toggle_ui_virtual_key = VK_F24;
  config.save_modifiers = config.record_modifiers = config.toggle_ui_modifiers =
      MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_NOREPEAT;
  CHECK(RegisterHotKey(blocker, 100, config.record_modifiers, config.record_virtual_key));
  klip::Hotkeys hotkeys;
  klip::Error error;
  CHECK(!hotkeys.Register(app, config, error));
  CHECK(hotkeys.SaveRegistered());
  CHECK(!hotkeys.RecordRegistered());
  CHECK(hotkeys.ToggleRegistered());
  CHECK(!hotkeys.Register(app, config, error));
  CHECK(hotkeys.SaveRegistered());
  UnregisterHotKey(blocker, 100);
  CHECK(hotkeys.Register(app, config, error));
  CHECK(hotkeys.RecordRegistered());
  hotkeys.Unregister();
  CHECK(RegisterHotKey(blocker, 101, config.save_modifiers, config.save_virtual_key));
  UnregisterHotKey(blocker, 101);
  DestroyWindow(app);
  DestroyWindow(blocker);
}

void TestHotkeyWindowDispatch() {
  klip::Win32Window window;
  klip::Error error;
  CHECK(window.Create(GetModuleHandleW(nullptr), SW_HIDE, error));
  if (window.Handle() == nullptr) return;

  // A popup should be visible on every connected display while the main Klip
  // window is hidden. This also leaves a notification visible on a secondary
  // monitor when a borderless-fullscreen game obscures overlays on its own display.
  std::vector<RECT> monitor_areas;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto& areas = *reinterpret_cast<std::vector<RECT>*>(parameter);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) areas.push_back(info.rcWork);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitor_areas));
  CHECK(!monitor_areas.empty());

  window.ShowToast(L"CLIP SAVED");
  std::vector<HWND> toasts;
  EnumWindows(
      [](HWND candidate, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(candidate)) return TRUE;
        wchar_t class_name[128]{};
        GetClassNameW(candidate, class_name, 128);
        if (lstrcmpW(class_name, L"KlipStatusToastClass") == 0)
          reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(candidate);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&toasts));
  CHECK(toasts.size() == monitor_areas.size());

  std::vector<bool> found_monitor_toast(monitor_areas.size(), false);
  for (const HWND toast : toasts) {
    CHECK(IsWindowVisible(toast));
    CHECK((GetWindowLongPtrW(toast, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    RECT toast_rect{};
    CHECK(GetWindowRect(toast, &toast_rect));
    bool placed_on_monitor = false;
    for (std::size_t index = 0; index < monitor_areas.size(); ++index) {
      const RECT& work = monitor_areas[index];
      const bool in_top_right = toast_rect.left >= work.left &&
                                toast_rect.right <= work.right &&
                                toast_rect.top >= work.top &&
                                toast_rect.top < work.top + 40 &&
                                toast_rect.right > work.right - 380;
      if (in_top_right) {
        CHECK(!found_monitor_toast[index]);
        found_monitor_toast[index] = true;
        placed_on_monitor = true;
      }
    }
    CHECK(placed_on_monitor);
  }
  for (const bool found : found_monitor_toast) CHECK(found);

  int save_clip_calls = 0;
  int recording_calls = 0;
  int toggle_ui_calls = 0;
  window.SetHotkeyCallback([&](int id) {
    if (id == klip::Hotkeys::kSaveClip)
      ++save_clip_calls;
    else if (id == klip::Hotkeys::kToggleRecording)
      ++recording_calls;
    else if (id == klip::Hotkeys::kToggleUi)
      ++toggle_ui_calls;
  });

  klip::HotkeyConfig config;
  config.save_virtual_key = VK_F22;
  config.record_virtual_key = VK_F23;
  config.toggle_ui_virtual_key = VK_F24;
  config.save_modifiers = config.record_modifiers = config.toggle_ui_modifiers =
      MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_NOREPEAT;
  klip::Hotkeys hotkeys;
  CHECK(hotkeys.Register(window.Handle(), config, error));
  if (hotkeys.SaveRegistered() && hotkeys.RecordRegistered() && hotkeys.ToggleRegistered()) {
    // Registration is tested by TestPartialHotkeyRegistration. Post WM_HOTKEY directly here to
    // test the actual Win32Window message-to-action routing without synthesizing desktop input.
    CHECK(PostMessageW(window.Handle(), WM_HOTKEY, klip::Hotkeys::kSaveClip, 0));
    CHECK(PostMessageW(window.Handle(), WM_HOTKEY, klip::Hotkeys::kToggleRecording, 0));
    CHECK(PostMessageW(window.Handle(), WM_HOTKEY, klip::Hotkeys::kToggleUi, 0));
    CHECK(window.PumpMessages());
    CHECK(save_clip_calls == 1);
    CHECK(recording_calls == 1);
    CHECK(toggle_ui_calls == 1);
  }
  hotkeys.Unregister();
  window.Destroy();
}

void TestRecordingOverloadStops() {
  klip::ApplicationState state;
  klip::Logger logger;
  klip::RecordingWriter writer(state, logger);
  klip::AppConfig config;
  config.recording_directory = std::filesystem::temp_directory_path() /
                               ("klip-overload-test-" + std::to_string(GetCurrentProcessId()));
  // Fault injection: a zero-capacity queue rejects the first packet regardless
  // of disk speed or worker scheduling.
  config.recording_packet_queue_capacity = 0;
  klip::Error error;
  auto snapshot = [](klip::CodecSnapshot& output) {
    output.parameters.reset(avcodec_parameters_alloc());
    if (!output.parameters) return false;
    output.parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    output.parameters->codec_id = AV_CODEC_ID_MPEG4;
    output.parameters->width = output.parameters->height = 16;
    output.time_base = AVRational{1, 60};
    return true;
  };
  CHECK(writer.Initialize(config, snapshot, {}, error));
  CHECK(writer.StartRecording(error));
  klip::PacketPtr packet(av_packet_alloc());
  CHECK(packet && av_new_packet(packet.get(), 16) == 0);
  if (packet) {
    packet->pts = packet->dts = 0;
    packet->flags = AV_PKT_FLAG_KEY;
    writer.Publish(packet.get(), klip::StreamKind::kVideo, {1, 60});
    CHECK(!writer.IsRecording());
    writer.Publish(packet.get(), klip::StreamKind::kVideo, {1, 60});
  }
  writer.StopRecording();
  CHECK(state.Snapshot().last_saved_recording.empty());
  CHECK(state.Snapshot().last_error.has_value());
  std::error_code ignored;
  std::filesystem::remove(config.recording_directory, ignored);
}

klip::CodecSnapshot MakeTestCodecSnapshot(const AVCodecContext* context) {
  klip::CodecSnapshot snapshot;
  snapshot.parameters.reset(avcodec_parameters_alloc());
  if (snapshot.parameters &&
      avcodec_parameters_from_context(snapshot.parameters.get(), context) >= 0)
    snapshot.time_base = context->time_base;
  else
    snapshot.parameters.reset();
  return snapshot;
}

klip::PacketPtr MakeTestKeyframe(AVCodecContext* encoder) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) return {};
  frame->format = encoder->pix_fmt;
  frame->width = encoder->width;
  frame->height = encoder->height;
  frame->pts = 0;
  if (av_frame_get_buffer(frame, 32) < 0 || av_frame_make_writable(frame) < 0) {
    av_frame_free(&frame);
    return {};
  }
  std::memset(frame->data[0], 96, static_cast<std::size_t>(frame->linesize[0]) * frame->height);
  std::memset(frame->data[1], 128,
              static_cast<std::size_t>(frame->linesize[1]) * (frame->height / 2));
  std::memset(frame->data[2], 128,
              static_cast<std::size_t>(frame->linesize[2]) * (frame->height / 2));
  if (avcodec_send_frame(encoder, frame) < 0) {
    av_frame_free(&frame);
    return {};
  }
  av_frame_free(&frame);
  klip::PacketPtr packet(av_packet_alloc());
  if (!packet || avcodec_receive_packet(encoder, packet.get()) < 0) return {};
  return packet;
}

int CountDecodedFrames(const std::filesystem::path& path) {
  AVFormatContext* format = nullptr;
  const auto wide = path.wstring();
  const auto required =
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) return -1;
  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), required, nullptr, nullptr);
  utf8.pop_back();
  if (avformat_open_input(&format, utf8.c_str(), nullptr, nullptr) < 0) return -1;
  if (avformat_find_stream_info(format, nullptr) < 0) {
    avformat_close_input(&format);
    return -1;
  }
  const auto stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    avformat_close_input(&format);
    return -1;
  }
  auto* decoder = avcodec_find_decoder(format->streams[stream_index]->codecpar->codec_id);
  AVCodecContext* context = decoder ? avcodec_alloc_context3(decoder) : nullptr;
  if (!context ||
      avcodec_parameters_to_context(context, format->streams[stream_index]->codecpar) < 0 ||
      avcodec_open2(context, decoder, nullptr) < 0) {
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return -1;
  }
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int decoded = 0;
  while (packet && frame && av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == stream_index && avcodec_send_packet(context, packet) >= 0) {
      while (avcodec_receive_frame(context, frame) >= 0) {
        ++decoded;
        av_frame_unref(frame);
      }
    }
    av_packet_unref(packet);
  }
  if (context) avcodec_send_packet(context, nullptr);
  while (context && frame && avcodec_receive_frame(context, frame) >= 0) {
    ++decoded;
    av_frame_unref(frame);
  }
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&context);
  avformat_close_input(&format);
  return decoded;
}

void TestAcceptedRecordingPrefixDecodes() {
  const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  CHECK(codec != nullptr);
  if (!codec) return;
  AVCodecContext* encoder = avcodec_alloc_context3(codec);
  CHECK(encoder != nullptr);
  if (!encoder) return;
  encoder->width = encoder->height = 64;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->time_base = AVRational{1, 30};
  encoder->framerate = AVRational{30, 1};
  encoder->gop_size = 1;
  encoder->max_b_frames = 0;
  encoder->bit_rate = 100'000;
  CHECK(avcodec_open2(encoder, codec, nullptr) >= 0);
  auto packet = MakeTestKeyframe(encoder);
  auto snapshot = MakeTestCodecSnapshot(encoder);
  CHECK(packet != nullptr && snapshot.parameters != nullptr);
  if (!packet || !snapshot.parameters) {
    avcodec_free_context(&encoder);
    return;
  }
  CHECK((packet->flags & AV_PKT_FLAG_KEY) != 0);
  if (packet->duration <= 0) packet->duration = 1;

  const auto directory = std::filesystem::temp_directory_path() /
                         ("klip-prefix-decode-" + std::to_string(GetCurrentProcessId()));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  klip::ApplicationState state;
  klip::Logger logger;
  klip::RecordingWriter writer(state, logger);
  klip::AppConfig config;
  config.recording_directory = directory;
  config.recording_packet_queue_capacity = 8;
  auto snapshot_provider = [&](klip::CodecSnapshot& output) {
    output.parameters.reset(avcodec_parameters_alloc());
    if (!output.parameters ||
        avcodec_parameters_copy(output.parameters.get(), snapshot.parameters.get()) < 0)
      return false;
    output.time_base = snapshot.time_base;
    return true;
  };
  klip::Error error;
  CHECK(writer.Initialize(config, snapshot_provider, {}, error));
  CHECK(writer.StartRecording(error));
  writer.Publish(packet.get(), klip::StreamKind::kVideo, encoder->time_base);
  writer.StopRecording();
  const auto saved = state.Snapshot().last_saved_recording;
  CHECK(!saved.empty());
  if (!saved.empty()) {
    const auto decoded = CountDecodedFrames(saved);
    CHECK(decoded == 1);
  }
  std::filesystem::remove_all(directory, ignored);
  avcodec_free_context(&encoder);
}

void TestRecordingOverloadSavesAcceptedPrefix() {
  const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  CHECK(codec != nullptr);
  if (!codec) return;
  AVCodecContext* encoder = avcodec_alloc_context3(codec);
  CHECK(encoder != nullptr);
  if (!encoder) return;
  encoder->width = encoder->height = 64;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->time_base = AVRational{1, 60};
  encoder->framerate = AVRational{60, 1};
  encoder->gop_size = 1;
  encoder->max_b_frames = 0;
  encoder->bit_rate = 100'000;
  CHECK(avcodec_open2(encoder, codec, nullptr) >= 0);
  auto packet = MakeTestKeyframe(encoder);
  auto snapshot = MakeTestCodecSnapshot(encoder);
  CHECK(packet != nullptr && snapshot.parameters != nullptr);
  if (!packet || !snapshot.parameters) {
    avcodec_free_context(&encoder);
    return;
  }
  if (packet->duration <= 0) packet->duration = 1;

  const auto directory = std::filesystem::temp_directory_path() /
                         ("klip-overload-prefix-" + std::to_string(GetCurrentProcessId()));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  klip::ApplicationState state;
  klip::Logger logger;
  klip::RecordingWriter writer(state, logger);
  klip::AppConfig config;
  config.recording_directory = directory;
  config.recording_packet_queue_capacity = 4;
  auto snapshot_provider = [&](klip::CodecSnapshot& output) {
    output.parameters.reset(avcodec_parameters_alloc());
    if (!output.parameters ||
        avcodec_parameters_copy(output.parameters.get(), snapshot.parameters.get()) < 0)
      return false;
    output.time_base = snapshot.time_base;
    return true;
  };
  klip::Error error;
  CHECK(writer.Initialize(config, snapshot_provider, {}, error));
  CHECK(writer.StartRecording(error));

  // Publish faster than the muxer can drain a deliberately tiny queue. Packets are
  // independent keyframes so any accepted prefix is valid even when overflow occurs.
  constexpr std::int64_t kMaxPackets = 100'000;
  for (std::int64_t index = 0; index < kMaxPackets && writer.IsRecording(); ++index) {
    klip::PacketPtr current(av_packet_clone(packet.get()));
    CHECK(current != nullptr);
    if (!current) break;
    current->pts = current->dts = index;
    writer.Publish(current.get(), klip::StreamKind::kVideo, encoder->time_base);
  }
  CHECK(!writer.IsRecording());
  writer.StopRecording();
  const auto status = state.Snapshot();
  CHECK(status.last_error.has_value());
  if (status.last_error)
    CHECK(status.last_error->message.find("saving the accepted portion") != std::string::npos);
  CHECK(!status.last_saved_recording.empty());
  if (!status.last_saved_recording.empty()) {
    CHECK(std::filesystem::exists(status.last_saved_recording));
    CHECK(CountDecodedFrames(status.last_saved_recording) > 0);
  }
  std::filesystem::remove_all(directory, ignored);
  avcodec_free_context(&encoder);
}

void TestRecordingSurvivesCaptureSourceReset() {
  const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  CHECK(codec != nullptr);
  if (!codec) return;
  AVCodecContext* encoder = avcodec_alloc_context3(codec);
  CHECK(encoder != nullptr);
  if (!encoder) return;
  encoder->width = encoder->height = 64;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->time_base = AVRational{1, 30};
  encoder->framerate = AVRational{30, 1};
  encoder->gop_size = 1;
  encoder->max_b_frames = 0;
  encoder->bit_rate = 100'000;
  CHECK(avcodec_open2(encoder, codec, nullptr) >= 0);
  auto first = MakeTestKeyframe(encoder);
  auto snapshot = MakeTestCodecSnapshot(encoder);
  CHECK(first != nullptr && snapshot.parameters != nullptr);
  if (!first || !snapshot.parameters) {
    avcodec_free_context(&encoder);
    return;
  }
  if (first->duration <= 0) first->duration = 1;
  auto second = klip::PacketPtr(av_packet_clone(first.get()));
  CHECK(second != nullptr);
  if (!second) {
    avcodec_free_context(&encoder);
    return;
  }
  second->pts = second->dts = 1;
  second->duration = 1;

  const auto directory = std::filesystem::temp_directory_path() /
                         ("klip-source-reset-" + std::to_string(GetCurrentProcessId()));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  klip::ApplicationState state;
  klip::Logger logger;
  klip::RecordingWriter writer(state, logger);
  klip::AppConfig config;
  config.recording_directory = directory;
  config.recording_packet_queue_capacity = 8;
  auto snapshot_provider = [&](klip::CodecSnapshot& output) {
    output.parameters.reset(avcodec_parameters_alloc());
    if (!output.parameters ||
        avcodec_parameters_copy(output.parameters.get(), snapshot.parameters.get()) < 0)
      return false;
    output.time_base = snapshot.time_base;
    return true;
  };
  klip::Error error;
  CHECK(writer.Initialize(config, snapshot_provider, {}, error));
  klip::RollingMediaBuffer buffer(10.0, 1024 * 1024);
  klip::PacketRouter router(buffer, writer, state, logger);
  CHECK(writer.StartRecording(error));
  router.Publish(first.get(), klip::StreamKind::kVideo, encoder->time_base);
  router.ResetTimeline(false);
  CHECK(writer.IsRecording());
  router.Publish(second.get(), klip::StreamKind::kVideo, encoder->time_base);
  writer.StopRecording();
  const auto saved = state.Snapshot().last_saved_recording;
  CHECK(!saved.empty());
  if (!saved.empty()) CHECK(CountDecodedFrames(saved) == 2);
  std::filesystem::remove_all(directory, ignored);
  avcodec_free_context(&encoder);
}

void TestFiveTrackMuxerRouting() {
  const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  CHECK(codec != nullptr);
  if (!codec) return;
  AVCodecContext* video = avcodec_alloc_context3(codec);
  CHECK(video != nullptr);
  if (!video) return;
  video->width = video->height = 64;
  video->pix_fmt = AV_PIX_FMT_YUV420P;
  video->time_base = AVRational{1, 30};
  video->framerate = AVRational{30, 1};
  video->gop_size = 1;
  video->max_b_frames = 0;
  video->bit_rate = 100'000;
  CHECK(avcodec_open2(video, codec, nullptr) >= 0);
  auto video_packet = MakeTestKeyframe(video);
  auto video_snapshot = MakeTestCodecSnapshot(video);
  CHECK(video_packet != nullptr && video_snapshot.parameters != nullptr);
  if (!video_packet || !video_snapshot.parameters) {
    avcodec_free_context(&video);
    return;
  }
  if (video_packet->duration <= 0) video_packet->duration = 1;

  std::vector<klip::CodecSnapshot> audio_tracks;
  for (int index = 0; index < 5; ++index) {
    klip::CodecSnapshot audio;
    audio.parameters.reset(avcodec_parameters_alloc());
    CHECK(audio.parameters != nullptr);
    if (!audio.parameters) {
      avcodec_free_context(&video);
      return;
    }
    audio.parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    audio.parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio.parameters->format = AV_SAMPLE_FMT_S16;
    audio.parameters->sample_rate = 48000;
    audio.parameters->bit_rate = 48000 * 2 * 16;
    audio.parameters->bits_per_coded_sample = 16;
    audio.parameters->block_align = 4;
    av_channel_layout_default(&audio.parameters->ch_layout, 2);
    audio.time_base = AVRational{1, 48000};
    audio_tracks.push_back(std::move(audio));
  }

  const auto path = std::filesystem::temp_directory_path() /
      ("klip-five-track-mux-" + std::to_string(GetCurrentProcessId()) + ".mkv");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.wstring() + L".partial", ignored);
  klip::Mp4Muxer muxer;
  klip::Error error;
  const bool opened = muxer.OpenMultiAudio(path, video_snapshot, audio_tracks,
                                           klip::ErrorComponent::kClipWriter, error);
  CHECK(opened);
  if (opened) {
    klip::EncodedPacket picture{std::move(video_packet), klip::StreamKind::kVideo, video->time_base};
    CHECK(muxer.Write(picture, 0, error));
    for (std::uint8_t track = 0; track < 5; ++track) {
      klip::PacketPtr packet(av_packet_alloc());
      CHECK(packet != nullptr);
      if (!packet) continue;
      CHECK(av_new_packet(packet.get(), 4096) == 0);
      if (packet->data == nullptr) continue;
      std::memset(packet->data, static_cast<int>(0x10 + track), 4096);
      packet->pts = packet->dts = 0;
      packet->duration = 1024;
      klip::EncodedPacket encoded{std::move(packet), klip::StreamKind::kAudio,
                                  AVRational{1, 48000}, track};
      CHECK(muxer.Write(encoded, 0, error));
    }
    CHECK(muxer.Finalize(error));
  }
  avcodec_free_context(&video);

  const auto utf8 = klip::PathToUtf8(path);
  AVFormatContext* format = nullptr;
  CHECK(avformat_open_input(&format, utf8.c_str(), nullptr, nullptr) >= 0);
  if (format) {
    CHECK(avformat_find_stream_info(format, nullptr) >= 0);
    CHECK(format->nb_streams == 6);
    static constexpr const char* expected_titles[] = {
        "Desktop audio", "Game audio", "Microphone", "Discord audio", "Music audio"};
    bool has_packet[5]{};
    int audio_index = 0;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
      const auto* stream = format->streams[index];
      if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      CHECK(audio_index < 5);
      if (audio_index < 5) {
        const auto* title = av_dict_get(stream->metadata, "title", nullptr, 0);
        CHECK(title != nullptr);
        if (title) CHECK(std::strcmp(title->value, expected_titles[audio_index]) == 0);
      }
      ++audio_index;
    }
    AVPacket* packet = av_packet_alloc();
    CHECK(packet != nullptr);
    while (packet && av_read_frame(format, packet) >= 0) {
      const auto stream_index = packet->stream_index;
      if (stream_index >= 1 && stream_index <= 5)
        has_packet[static_cast<std::size_t>(stream_index - 1)] = true;
      av_packet_unref(packet);
    }
    av_packet_free(&packet);
    CHECK(audio_index == 5);
    for (bool seen : has_packet) CHECK(seen);
    avformat_close_input(&format);
  }
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.wstring() + L".partial", ignored);
}

}  // namespace

int main() {
  TestNativeCaptureDefaults();
  TestWaveFormats();
  TestAudioRecoveryBackoff();
  TestModifierFreeHotkeyRegistration();
  TestPartialHotkeyRegistration();
  TestHotkeyWindowDispatch();
  TestRecordingOverloadStops();
  TestAcceptedRecordingPrefixDecodes();
  TestRecordingOverloadSavesAcceptedPrefix();
  TestRecordingSurvivesCaptureSourceReset();
  TestFiveTrackMuxerRouting();
  return failures ? 1 : 0;
}
