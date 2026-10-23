#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace mpm {

bool InitLogging(const std::filesystem::path& logDirectory, const std::string& level,
                 std::wstring* error);
void ShutdownLogging();
std::shared_ptr<spdlog::logger> Log();

} // namespace mpm