#pragma once

#include <Windows.h>

#include <array>

#include "klip/core/config.h"
#include "klip/core/error.h"

namespace klip {

class Hotkeys {
 public:
  static constexpr int kSaveClip = 1;
  static constexpr int kToggleUi = 2;
  static constexpr int kToggleRecording = 3;
  static constexpr int kSaveClip2 = 4;
  static constexpr int kSaveClip3 = 5;
  static constexpr int kToggleRecording2 = 6;
  static constexpr int kToggleRecording3 = 7;

  static constexpr bool IsSaveClip(int id) noexcept {
    return id == kSaveClip || id == kSaveClip2 || id == kSaveClip3;
  }
  static constexpr bool IsToggleRecording(int id) noexcept {
    return id == kToggleRecording || id == kToggleRecording2 || id == kToggleRecording3;
  }

  Hotkeys() = default;
  ~Hotkeys() noexcept;
  Hotkeys(const Hotkeys&) = delete;
  Hotkeys& operator=(const Hotkeys&) = delete;

  bool Register(HWND window, const HotkeyConfig& config, Error& error);
  void Unregister() noexcept;
  [[nodiscard]] bool SaveRegistered() const noexcept {
    return save_registered_[0] && save_registered_[1] && save_registered_[2];
  }
  [[nodiscard]] bool ToggleRegistered() const noexcept { return toggle_registered_; }
  [[nodiscard]] bool RecordRegistered() const noexcept {
    return record_registered_[0] && record_registered_[1] && record_registered_[2];
  }

 private:
  HWND window_ = nullptr;
  HotkeyConfig config_;
  std::array<bool, 3> save_registered_{};
  bool toggle_registered_ = false;
  std::array<bool, 3> record_registered_{};
};

}  // namespace klip
