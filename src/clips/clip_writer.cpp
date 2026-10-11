#include "klip/clips/clip_writer.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "klip/media/mp4_muxer.h"
#include "klip/core/path_text.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace klip {
namespace {

constexpr AVRational kHundredNanoseconds{1, 10'000'000};

}  // namespace

ClipWriter::ClipWriter(RollingMediaBuffer& buffer, ApplicationState& state, Logger& logger,
                       std::size_t queue_capacity)
    : buffer_(buffer), state_(state), logger_(logger), requests_(queue_capacity) {}

ClipWriter::~ClipWriter() noexcept { Stop(); }

bool ClipWriter::Start(AppConfig config, SnapshotProvider video, SnapshotProvider audio,
                       Error& error) {
  if (running_.exchange(true, std::memory_order_acq_rel)) return true;
  requests_.Reset();
  config_ = std::move(config);
  video_snapshot_ = std::move(video);
  audio_snapshot_ = std::move(audio);
  std::error_code directory_error;
  std::filesystem::create_directories(config_.output_directory, directory_error);
  if (directory_error) {
    running_.store(false, std::memory_order_release);
    error = Error{ErrorComponent::kClipWriter, "create output directory",
                  directory_error.message(),   directory_error.value(),
                  directory_error.message(),   PathToUtf8(config_.output_directory)};
    return false;
  }
  worker_ = std::jthread([this](std::stop_token token) { Worker(token); });
  return true;
}

void ClipWriter::Stop() noexcept {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  requests_.Close();
  worker_.request_stop();
  worker_ = {};
  video_snapshot_ = {};
  audio_snapshot_ = {};
}

bool ClipWriter::RequestClip() {
  if (!running_.load(std::memory_order_acquire)) return false;
  Request request{BuildOutputPath(), config_.clip_duration_seconds};
  const auto accepted = requests_.TryPush(std::move(request));
  if (accepted)
    logger_.Info("Clip save requested");
  else
    logger_.Warning("Clip save request rejected: writer queue is full or closed");
  return accepted;
}

void ClipWriter::Worker(std::stop_token stop_token) noexcept {
  try {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    Request request;
    while (requests_.WaitPop(request, stop_token)) {
      const auto previous_state = state_.Snapshot();
      const auto restore_status = [&] {
        if (state_.Snapshot().status == CaptureStatus::kSaving)
          state_.SetStatus(previous_state.status, previous_state.current_operation);
      };
      state_.SetStatus(CaptureStatus::kSaving, "Saving clip");
      const auto started = std::chrono::steady_clock::now();
      CodecSnapshot video;
      CodecSnapshot audio;
      Error error;
      if (!video_snapshot_ || !video_snapshot_(video)) {
        error = Error{ErrorComponent::kClipWriter, "snapshot video codec",
                      "video encoder is not ready"};
      } else {
        const auto has_audio = audio_snapshot_ && audio_snapshot_(audio);
        // A clip requested immediately after capture starts can arrive before the
        // encoder has emitted its first keyframe. Give the rolling buffer a short
        // warm-up window instead of failing the user-visible clip operation.
        std::vector<EncodedPacket> packets;
        auto snapshot_ok = buffer_.Snapshot(request.duration_seconds, packets, error);
        constexpr auto kKeyframeWarmup = std::chrono::seconds(5);
        const auto warmup_deadline = std::chrono::steady_clock::now() + kKeyframeWarmup;
        while (snapshot_ok && packets.empty() && !stop_token.stop_requested() &&
               std::chrono::steady_clock::now() < warmup_deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          snapshot_ok = buffer_.Snapshot(request.duration_seconds, packets, error);
        }
        if (!snapshot_ok) {
          // The buffer already supplied the precise failure and guarantees no partial snapshot.
        } else if (packets.empty()) {
          error = Error{ErrorComponent::kClipWriter, "select clip range",
                        "the rolling buffer does not yet contain a keyframe"};
        } else if (Write(request, packets, video, has_audio ? &audio : nullptr, error)) {
          const auto duration =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                  .count();
          state_.SetLastSavedClip(request.output_path, duration);
          state_.ClearError(ErrorComponent::kClipWriter);
          logger_.Info("Clip saved: " + PathToUtf8(request.output_path));
          restore_status();
          continue;
        }
      }
      state_.SetError(error);
      logger_.ErrorMessage(error);
      restore_status();
    }
  } catch (const std::exception& exception) {
    Error error{ErrorComponent::kClipWriter, "clip writer worker", exception.what()};
    state_.SetError(error);
    logger_.ErrorMessage(error);
  }
}

bool ClipWriter::Write(const Request& request, const std::vector<EncodedPacket>& packets,
                       const CodecSnapshot& video, const CodecSnapshot* audio, Error& error) {
  std::int64_t base = INT64_MAX;
  for (const auto& encoded : packets) {
    if (!encoded.packet) continue;
    const auto timestamp =
        encoded.packet->dts == AV_NOPTS_VALUE ? encoded.packet->pts : encoded.packet->dts;
    if (timestamp != AV_NOPTS_VALUE) {
      base = std::min(base, av_rescale_q(timestamp, encoded.time_base, kHundredNanoseconds));
    }
  }
  if (base == INT64_MAX) base = 0;

  Mp4Muxer muxer;
  if (!muxer.Open(request.output_path, video, audio, ErrorComponent::kClipWriter, error))
    return false;
  for (const auto& encoded : packets) {
    if (!muxer.Write(encoded, base, error)) return false;
  }
  return muxer.Finalize(error);
}

std::filesystem::path ClipWriter::BuildOutputPath() const {
  SYSTEMTIME time{};
  GetLocalTime(&time);

  const auto snapshot = state_.Snapshot();
  std::string game_folder;
  std::string filename_prefix = "clip";
  const auto target = snapshot.capture_target;
  std::string lower = target;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto set_game = [&](const char* folder, const char* prefix) {
    game_folder = folder;
    filename_prefix = prefix;
  };
  if (lower.find("javaw.exe") != std::string::npos ||
      lower.find("minecraft.windows.exe") != std::string::npos) {
    set_game("minecraft", "minecraft");
  } else if (lower.find("fortniteclient") != std::string::npos ||
             lower.find("fortnite.exe") != std::string::npos) {
    set_game("fortnite", "fortnite");
  } else if (lower.find("valorant") != std::string::npos) {
    set_game("valorant", "valorant");
  } else if (lower.find("gta5") != std::string::npos ||
             lower.find("fivem") != std::string::npos) {
    set_game("gta5", "gta5");
  } else if (lower.find("cs2.exe") != std::string::npos) {
    set_game("cs2", "cs2");
  } else if (lower.find("r5apex.exe") != std::string::npos) {
    set_game("apex", "apex");
  } else if (lower.find("robloxplayerbeta.exe") != std::string::npos) {
    set_game("roblox", "roblox");
  }

  auto directory = config_.output_directory;
  if (!game_folder.empty()) directory /= game_folder;

  std::string pattern = config_.obs_filename_format.empty()
                            ? "clip_%CCYY-%MM-%DD_%hh-%mm-%ss"
                            : config_.obs_filename_format;
  if (!game_folder.empty() && pattern.starts_with("clip_")) {
    pattern.replace(0, 4, filename_prefix);
  }
  const auto replace_all = [&](std::string_view token, const std::string& value) {
    std::size_t position = 0;
    while ((position = pattern.find(token, position)) != std::string::npos) {
      pattern.replace(position, token.size(), value);
      position += value.size();
    }
  };
  const auto number = [](unsigned value, int width) {
    std::ostringstream out;
    out << std::setw(width) << std::setfill('0') << value;
    return out.str();
  };
  replace_all("%CCYY", number(time.wYear, 4));
  replace_all("%YY", number(time.wYear % 100, 2));
  replace_all("%MM", number(time.wMonth, 2));
  replace_all("%DD", number(time.wDay, 2));
  replace_all("%hh", number(time.wHour, 2));
  replace_all("%mm", number(time.wMinute, 2));
  replace_all("%ss", number(time.wSecond, 2));
  replace_all("%ms", number(time.wMilliseconds, 3));

  std::string extension = config_.output_container;
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != "mp4" && extension != "mkv") extension = "mp4";

  std::error_code ignored;
  std::filesystem::create_directories(directory, ignored);
  const auto stem = std::filesystem::path(pattern).filename().string();
  for (unsigned suffix = 0; suffix < 1000; ++suffix) {
    const auto filename = stem + (suffix ? "_" + std::to_string(suffix + 1) : "") +
                          "." + extension;
    const auto candidate = directory / filename;
    if (!std::filesystem::exists(candidate, ignored)) return candidate;
    ignored.clear();
  }
  return directory / (stem + "_" + std::to_string(time.wMilliseconds) + "." + extension);
}

}  // namespace klip
