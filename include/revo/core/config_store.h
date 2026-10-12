#pragma once

#include <filesystem>
#include <string>

#include "revo/core/config.h"

namespace revo {

bool LoadConfig(const std::filesystem::path& path, AppConfig& config, std::string& diagnostic);
bool SaveConfig(const std::filesystem::path& path, const AppConfig& config,
                std::string& diagnostic);

}  // namespace revo
