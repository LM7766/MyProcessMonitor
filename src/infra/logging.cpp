#include "mpm/infra/logging.hpp"

#include "mpm/infra/text.hpp"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <exception>

namespace mpm {
namespace {

std::shared_ptr<spdlog::logger> g_logger;

} // namespace

bool InitLogging(const std::filesystem::path& logDirectory, const std::string& level,
                 std::wstring* error) {
    try {
        std::filesystem::create_directories(logDirectory);
        const std::string file = Utf8((logDirectory / L"mpm.log").wstring());
        auto logger = spdlog::rotating_logger_mt("mpm", file, 5 * 1024 * 1024, 3);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        logger->set_level(spdlog::level::from_str(level));
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
        g_logger = std::move(logger);
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = L"初始化日志失败: " + Utf16(ex.what());
        }
        return false;
    }
}

void ShutdownLogging() {
    if (g_logger) {
        g_logger->flush();
        spdlog::drop("mpm");
        g_logger.reset();
    }
}

std::shared_ptr<spdlog::logger> Log() {
    if (g_logger) {
        return g_logger;
    }
    return spdlog::default_logger();
}

} // namespace mpm