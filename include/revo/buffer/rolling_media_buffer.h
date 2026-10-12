#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "revo/core/error.h"
#include "revo/media/packet.h"

namespace revo {

struct RollingBufferStats {
  std::size_t packets = 0;
  std::size_t bytes = 0;
  double duration_seconds = 0.0;
};

class RollingMediaBuffer {
 public:
  RollingMediaBuffer(double maximum_seconds, std::size_t maximum_bytes);

  RollingMediaBuffer(const RollingMediaBuffer&) = delete;
  RollingMediaBuffer& operator=(const RollingMediaBuffer&) = delete;

  bool Push(const AVPacket* packet, StreamKind kind, AVRational time_base, Error& error);
  [[nodiscard]] bool Snapshot(double seconds, std::vector<EncodedPacket>& packets,
                              Error& error) const;
  [[nodiscard]] RollingBufferStats Stats() const;
  void Reconfigure(double maximum_seconds, std::size_t maximum_bytes);
  void Clear();

 private:
  void EvictLocked();
  void RefreshVideoBoundsLocked() noexcept;

  double maximum_seconds_;
  std::size_t maximum_bytes_;
  mutable std::mutex mutex_;
  std::deque<EncodedPacket> packets_;
  std::size_t bytes_ = 0;
  std::int64_t first_video_pts_100ns_ = 0;
  std::int64_t last_video_pts_100ns_ = 0;
  bool has_video_ = false;
};

}  // namespace revo
