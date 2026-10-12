#pragma once

#include <algorithm>
#include <cmath>
#include "revo/core/config.h"

namespace revo {
// OBS evicts whole GOPs. Retain an extra two-second GOP so a mature replay
// covers the requested time instead of losing its first partial GOP. Keep
// 25% of the packet budget free for VBV bursts, packet overhead and audio.
inline int ObsReplayRetentionSeconds(const AppConfig& config) {
  if (!std::isfinite(config.clip_duration_seconds) || config.clip_duration_seconds <= 0 ||
      config.clip_duration_seconds > 600) return 0;
  return static_cast<int>(std::ceil(config.clip_duration_seconds)) + 2;
}
inline int ObsReplayBitrateKbps(const AppConfig& config) {
  if (!ObsReplayRetentionSeconds(config) || config.obs_replay_bitrate_kbps <= 0) return 0;
  const auto seconds = std::ceil(config.clip_duration_seconds) + 4.0;
  const auto budget_kbps = static_cast<double>(config.rolling_buffer_bytes) * 8.0 * 0.75 / seconds / 1000.0;
  const auto audio_kbps = static_cast<double>(config.audio_bitrate) / 1000.0 *
                          (config.obs_separate_audio_tracks ? 3.0 : 1.0);
  return static_cast<int>(std::clamp(std::floor(budget_kbps - audio_kbps), 0.0,
                                   static_cast<double>(config.obs_replay_bitrate_kbps)));
}
} // namespace revo
