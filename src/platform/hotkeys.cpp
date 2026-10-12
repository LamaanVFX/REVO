#include "klip/platform/hotkeys.h"

#include <array>
#include <string>

namespace klip {

Hotkeys::~Hotkeys() noexcept { Unregister(); }

bool Hotkeys::Register(HWND window, const HotkeyConfig& config, Error& error) {
  if (window_ != window || !(config_ == config)) {
    Unregister();
    window_ = window;
    config_ = config;
  }

  struct Binding {
    int id;
    unsigned int modifiers;
    unsigned int key;
    bool* registered;
    const char* label;
  };
  std::array<Binding, 7> bindings{{
      {kSaveClip, config.save_modifiers, config.save_virtual_key, &save_registered_[0], "save clip 1"},
      {kSaveClip2, config.save2_modifiers, config.save2_virtual_key, &save_registered_[1], "save clip 2"},
      {kSaveClip3, config.save3_modifiers, config.save3_virtual_key, &save_registered_[2], "save clip 3"},
      {kToggleRecording, config.record_modifiers, config.record_virtual_key, &record_registered_[0], "record 1"},
      {kToggleRecording2, config.record2_modifiers, config.record2_virtual_key, &record_registered_[1], "record 2"},
      {kToggleRecording3, config.record3_modifiers, config.record3_virtual_key, &record_registered_[2], "record 3"},
      {kToggleUi, config.toggle_ui_modifiers, config.toggle_ui_virtual_key, &toggle_registered_, "show/hide Klip"},
  }};

  DWORD first_error = ERROR_SUCCESS;
  std::string unavailable;
  for (const auto& binding : bindings) {
    if (*binding.registered) continue;
    if (RegisterHotKey(window_, binding.id, binding.modifiers, binding.key) == TRUE) {
      *binding.registered = true;
    } else {
      const DWORD registration_error = GetLastError();
      if (first_error == ERROR_SUCCESS) first_error = registration_error;
      if (!unavailable.empty()) unavailable += ", ";
      unavailable += std::string(binding.label) + " (" +
                     FormatHotkey(binding.modifiers, binding.key) + ")";
    }
  }

  if (!unavailable.empty()) {
    error = MakeWin32Error(ErrorComponent::kHotkeys, "register global hotkeys", first_error,
                           "Unavailable shortcuts: " + unavailable +
                               "; other registered shortcuts remain available");
    return false;
  }
  return true;
}

void Hotkeys::Unregister() noexcept {
  if (window_ != nullptr) {
    if (save_registered_[0]) UnregisterHotKey(window_, kSaveClip);
    if (save_registered_[1]) UnregisterHotKey(window_, kSaveClip2);
    if (save_registered_[2]) UnregisterHotKey(window_, kSaveClip3);
    if (record_registered_[0]) UnregisterHotKey(window_, kToggleRecording);
    if (record_registered_[1]) UnregisterHotKey(window_, kToggleRecording2);
    if (record_registered_[2]) UnregisterHotKey(window_, kToggleRecording3);
    if (toggle_registered_) UnregisterHotKey(window_, kToggleUi);
  }
  save_registered_.fill(false);
  record_registered_.fill(false);
  toggle_registered_ = false;
  window_ = nullptr;
}

}  // namespace klip
