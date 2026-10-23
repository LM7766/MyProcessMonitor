#pragma once

#include <filesystem>
#include <string>

namespace mpm {

std::filesystem::path LocalAppDataDirectory();
std::filesystem::path AppDataDirectory();
std::filesystem::path ConfigFile();
std::filesystem::path DatabaseFile();
std::filesystem::path LogDirectory();
std::filesystem::path LogFile();

bool EnsureAppDirectories(std::wstring* error);

} // namespace mpm