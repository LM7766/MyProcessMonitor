#pragma once

#include "mpm/core/config.hpp"

#include <filesystem>
#include <string>

namespace mpm {

AppConfig LoadConfig(const std::filesystem::path& path, std::wstring* warning);
bool SaveDefaultConfig(const std::filesystem::path& path, std::wstring* error);

} // namespace mpm