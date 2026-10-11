#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

#include "klip/buffer/rolling_media_buffer.h"
#include "klip/core/application_state.h"
#include "klip/core/bounded_queue.h"
#include "klip/core/config.h"
#include "klip/core/logger.h"
#include "klip/media/packet.h"

namespace klip {

class ClipWriter {
 public:
  using SnapshotProvider = std::function<bool(CodecSnapshot&)>;
  using AudioTracksSnapshotProvider = std::function<bool(std::vector<CodecSnapshot>&)>;

  ClipWriter(RollingMediaBuffer& buffer, ApplicationState& state, Logger& logger,
             std::size_t queue_capacity);
  ~ClipWriter() noexcept;

  ClipWriter(const ClipWriter&) = delete;
  ClipWriter& operator=(const ClipWriter&) = delete;

  bool Start(AppConfig config, SnapshotProvider video, SnapshotProvider audio, Error& error);
  bool StartWithAudioTracks(AppConfig config, SnapshotProvider video,
                            AudioTracksSnapshotProvider audio, Error& error);
  void Stop() noexcept;
  bool RequestClip();

 private:
  struct Request {
    std::filesystem::path output_path;
    double duration_seconds = 60.0;
  };

  void Worker(std::stop_token stop_token) noexcept;
  bool Write(const Request& request, const std::vector<EncodedPacket>& packets,
             const CodecSnapshot& video, const std::vector<CodecSnapshot>& audio_tracks,
             Error& error);
  std::filesystem::path BuildOutputPath() const;

  RollingMediaBuffer& buffer_;
  ApplicationState& state_;
  Logger& logger_;
  BlockingBoundedQueue<Request> requests_;
  AppConfig config_;
  SnapshotProvider video_snapshot_;
  SnapshotProvider audio_snapshot_;
  AudioTracksSnapshotProvider audio_tracks_snapshot_;
  std::jthread worker_;
  std::atomic<bool> running_{false};
};

}  // namespace klip
