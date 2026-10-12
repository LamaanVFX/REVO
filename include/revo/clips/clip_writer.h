#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

#include "revo/buffer/rolling_media_buffer.h"
#include "revo/core/application_state.h"
#include "revo/core/bounded_queue.h"
#include "revo/core/config.h"
#include "revo/core/logger.h"
#include "revo/media/packet.h"

namespace revo {

class ClipWriter {
 public:
  using SnapshotProvider = std::function<bool(CodecSnapshot&)>;

  ClipWriter(RollingMediaBuffer& buffer, ApplicationState& state, Logger& logger,
             std::size_t queue_capacity);
  ~ClipWriter() noexcept;

  ClipWriter(const ClipWriter&) = delete;
  ClipWriter& operator=(const ClipWriter&) = delete;

  bool Start(AppConfig config, SnapshotProvider video, SnapshotProvider audio, Error& error);
  void Stop() noexcept;
  bool RequestClip();

 private:
  struct Request {
    std::filesystem::path output_path;
    double duration_seconds = 60.0;
  };

  void Worker(std::stop_token stop_token) noexcept;
  bool Write(const Request& request, const std::vector<EncodedPacket>& packets,
             const CodecSnapshot& video, const CodecSnapshot* audio, Error& error);
  static std::filesystem::path BuildOutputPath(const std::filesystem::path& directory);

  RollingMediaBuffer& buffer_;
  ApplicationState& state_;
  Logger& logger_;
  BlockingBoundedQueue<Request> requests_;
  AppConfig config_;
  SnapshotProvider video_snapshot_;
  SnapshotProvider audio_snapshot_;
  std::jthread worker_;
  std::atomic<bool> running_{false};
};

}  // namespace revo
