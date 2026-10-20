#pragma once

#include <string_view>

#include "klip/core/config.h"

namespace klip {
struct ObsNvencQualityPolicy {
  const char* preset;
  const char* multipass;
  bool adaptive_quantization;
};

// Performance is an explicit efficiency/quality tradeoff. Keep Balanced's
// OBS-quality configuration unchanged; never apply this based on GPU load alone.
constexpr ObsNvencQualityPolicy ObsNvencPolicyFor(EncoderQuality quality) noexcept {
  switch (quality) {
    case EncoderQuality::kPerformance: return {"p3", "disabled", false};
    case EncoderQuality::kQuality: return {"p7", "fullres", true};
    case EncoderQuality::kBalanced: return {"p5", "qres", true};
  }
  return {"p5", "qres", true};
}

inline constexpr std::string_view kObsNvencH264EncoderId = "obs_nvenc_h264_tex";
inline constexpr std::string_view kObsNvencHevcEncoderId = "obs_nvenc_hevc_tex";

// Klip uses H.264 for every normal game/application and switches to native
// NVIDIA NVENC HEVC only while the Minecraft process profile is active.
constexpr std::string_view ObsNvencEncoderForGame(std::string_view game) noexcept {
  return game == "Minecraft" ? kObsNvencHevcEncoderId : kObsNvencH264EncoderId;
}
}  // namespace klip
