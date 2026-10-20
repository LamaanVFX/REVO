#include "klip/platform/user_paths.h"

#include <Windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace klip {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id,
                                  const std::filesystem::path& fallback) {
  PWSTR value = nullptr;
  const auto result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value);
  if (FAILED(result) || value == nullptr) return fallback;
  std::filesystem::path path(value);
  CoTaskMemFree(value);
  return path;
}

}  // namespace

UserPaths ResolveUserPaths() {
  // REVO_DATA_ROOT is the current override. Keep KLIP_DATA_ROOT as a legacy
  // alias for existing users and automated test fixtures.
  std::wstring override_root(32768, L'\0');
  auto override_length = GetEnvironmentVariableW(
      L"REVO_DATA_ROOT", override_root.data(), static_cast<DWORD>(override_root.size()));
  const bool legacy_override = override_length == 0;
  if (legacy_override) {
    override_length = GetEnvironmentVariableW(
        L"KLIP_DATA_ROOT", override_root.data(), static_cast<DWORD>(override_root.size()));
  }
  if (override_length > 0 && override_length < override_root.size()) {
    override_root.resize(override_length);
    const std::filesystem::path root(override_root);
    // Tests and existing scripts expect klip.log under an explicit root.
    const auto log_name = legacy_override ? "klip.log" : "revo.log";
    return UserPaths{root / "settings.ini", root / "Clips", root / "Recordings",
                     root / log_name};
  }

  const auto local = KnownFolder(FOLDERID_LocalAppData, std::filesystem::current_path());
  const auto videos = KnownFolder(FOLDERID_Videos, local);
  const auto settings_root = local / "REVO";
  const auto legacy_settings_root = local / "Klip";
  const auto new_settings = settings_root / "settings.ini";
  const auto old_settings = legacy_settings_root / "settings.ini";

  // Preserve the user's existing shortcuts, keybinds, folders, audio routing,
  // and encoder settings by copying the old INI only when REVO has no settings yet.
  std::error_code error;
  if (!std::filesystem::exists(new_settings, error) &&
      std::filesystem::exists(old_settings, error)) {
    std::filesystem::create_directories(settings_root, error);
    if (!error) {
      std::filesystem::copy_file(old_settings, new_settings,
                                 std::filesystem::copy_options::skip_existing, error);
    }
    if (error) {
      // If migration is blocked by permissions/locking, keep reading the old
      // file rather than falling back to fresh defaults and losing settings.
      return UserPaths{old_settings, videos / "REVO" / "Clips",
                       videos / "REVO" / "Recordings", legacy_settings_root / "klip.log"};
    }
  }

  const auto media_root = videos / "REVO";
  return UserPaths{new_settings, media_root / "Clips",
                   media_root / "Recordings", settings_root / "revo.log"};
}

void ApplyUserPathDefaults(AppConfig& config, const UserPaths& paths) {
  config.output_directory = paths.clips_directory;
  config.recording_directory = paths.recordings_directory;
  config.log_path = paths.log_file;
}

}  // namespace klip
