#include "mpm/app/monitor_session.hpp"
#include "mpm/core/config.hpp"
#include "mpm/infra/config_loader.hpp"
#include "mpm/infra/event_json.hpp"
#include "mpm/infra/logging.hpp"
#include "mpm/infra/paths.hpp"
#include "mpm/infra/sqlite_event_store.hpp"
#include "mpm/infra/text.hpp"
#include "mpm/win32/idle_provider.hpp"
#include "mpm/win32/process_catalog.hpp"
#include "mpm/win32/system_sampler.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI ConsoleControlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

struct Arguments {
    std::string command;
    std::string configPath;
    std::string dataDir;
    int limit = 20;
    int durationSeconds = 60;
    bool json = false;
};

void PrintUsage() {
    std::cout
        << "MyProcessMonitor v0.1\n\n"
        << "用法:\n"
        << "  mpm-cli snapshot [--config <path>] [--json]\n"
        << "  mpm-cli watch [--config <path>] [--data-dir <path>] [--duration <seconds>]\n"
        << "  mpm-cli events [--data-dir <path>] [--limit <n>] [--json]\n"
        << "  mpm-cli doctor [--config <path>] [--data-dir <path>]\n\n"
        << "说明:\n"
        << "  watch 的 --duration 0 表示一直运行，按 Ctrl+C 退出。\n"
        << "  默认数据目录为 %LOCALAPPDATA%\\MyProcessMonitor。\n";
}

bool ParseArguments(int argc, char** argv, Arguments& arguments, bool& helpRequested) {
    if (argc < 2) {
        return false;
    }
    arguments.command = argv[1];
    if (arguments.command == "--help" || arguments.command == "-h") {
        helpRequested = true;
        return false;
    }
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        auto next = [&](std::string& target) {
            if (index + 1 >= argc) {
                return false;
            }
            target = argv[++index];
            return true;
        };
        if (option == "--config") {
            if (!next(arguments.configPath)) {
                return false;
            }
        } else if (option == "--data-dir") {
            if (!next(arguments.dataDir)) {
                return false;
            }
        } else if (option == "--limit") {
            std::string value;
            if (!next(value)) {
                return false;
            }
            arguments.limit = std::atoi(value.c_str());
        } else if (option == "--duration") {
            std::string value;
            if (!next(value)) {
                return false;
            }
            arguments.durationSeconds = std::atoi(value.c_str());
        } else if (option == "--json") {
            arguments.json = true;
        } else if (option == "--help" || option == "-h") {
            helpRequested = true;
            return false;
        } else {
            return false;
        }
    }
    return true;
}

std::filesystem::path ResolveDataDirectory(const Arguments& arguments) {
    if (!arguments.dataDir.empty()) {
        return std::filesystem::path(mpm::Utf16(arguments.dataDir));
    }
    return mpm::AppDataDirectory();
}

std::filesystem::path ResolveConfigPath(const Arguments& arguments) {
    if (!arguments.configPath.empty()) {
        return std::filesystem::path(mpm::Utf16(arguments.configPath));
    }
    return mpm::ConfigFile();
}

std::filesystem::path ResolveDatabasePath(const Arguments& arguments) {
    if (!arguments.dataDir.empty()) {
        return std::filesystem::path(mpm::Utf16(arguments.dataDir)) / L"monitor.db";
    }
    return mpm::DatabaseFile();
}

std::filesystem::path ResolveLogDirectory(const Arguments& arguments) {
    if (!arguments.dataDir.empty()) {
        return std::filesystem::path(mpm::Utf16(arguments.dataDir)) / L"logs";
    }
    return mpm::LogDirectory();
}

mpm::AppConfig LoadConfiguration(const Arguments& arguments) {
    std::wstring warning;
    const mpm::AppConfig config = mpm::LoadConfig(ResolveConfigPath(arguments), &warning);
    if (!warning.empty()) {
        std::cerr << mpm::Utf8(warning) << "\n";
    }
    return config;
}

int RunSnapshot(const Arguments& arguments) {
    (void)arguments;
    mpm::Win32SystemSampler sampler;
    mpm::Win32IdleProvider idle;
    mpm::Win32ProcessCatalog catalog;

    std::optional<mpm::SystemSample> sample;
    for (int attempt = 0; attempt < 4; ++attempt) {
        sample = sampler.Capture();
        if (sample) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!sample) {
        std::cerr << "采样失败: " << mpm::Utf8(sampler.LastError()) << "\n";
        return 1;
    }

    const mpm::ActivityState activity = idle.Query();
    std::vector<mpm::ProcessCpu> processes = sample->processes;
    std::sort(processes.begin(), processes.end(), [](const auto& left, const auto& right) {
        return left.systemPercent > right.systemPercent;
    });
    if (processes.size() > 25) {
        processes.resize(25);
    }

    nlohmann::json top = nlohmann::json::array();
    for (const auto& process : processes) {
        const mpm::ProcessInfo info = catalog.Describe(process.key);
        top.push_back(nlohmann::json{
            {"pid", process.key.pid},
            {"name", mpm::Utf8(process.imageName)},
            {"cpuPercent", process.systemPercent},
            {"oneCorePercent", process.oneCorePercent},
            {"parentPid", process.parentPid},
            {"sessionId", process.sessionId},
            {"path", mpm::Utf8(info.imagePath)},
            {"publisher", mpm::Utf8(info.publisher)},
            {"signature", mpm::Utf8(mpm::ToString(info.signature))},
            {"services", mpm::Utf8(info.services)},
        });
    }

    const nlohmann::json output{
        {"sampler", mpm::Utf8(sampler.Name())},
        {"systemCpuPercent", sample->systemCpuPercent},
        {"idleCpuPercent", sample->idleCpuPercent},
        {"logicalCpuCount", sample->logicalCpuCount},
        {"idleSeconds", activity.idleSeconds},
        {"locked", activity.locked},
        {"sessionActive", activity.sessionActive},
        {"fullscreen", activity.fullscreen},
        {"foregroundPid", activity.foregroundPid},
        {"processCount", sample->processes.size()},
        {"processes", std::move(top)},
    };
    std::cout << output.dump(2) << "\n";
    return 0;
}

void PrintEvent(const mpm::DetectionEvent& event) {
    std::cout << mpm::LocalTimeString(event.startedUnixMs) << "  " << mpm::ToString(event.severity)
              << "  score=" << event.score << "  “" << mpm::Utf8(event.culprit.displayName)
              << "”  " << std::fixed << std::setprecision(1) << event.culprit.cpuPercent
              << "% of system";
    if (event.knownMaintenance) {
        std::cout << "  [known maintenance]";
    }
    if (event.finished) {
        std::cout << "  (finished)";
    }
    std::cout << "\n";
}

int RunWatch(const Arguments& arguments) {
    const mpm::AppConfig config = LoadConfiguration(arguments);
    mpm::MonitorSession session(config, ResolveDatabasePath(arguments),
                                ResolveLogDirectory(arguments));

    std::wstring error;
    if (!session.Start(&error)) {
        std::cerr << "启动监控失败: " << mpm::Utf8(error) << "\n";
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    const auto started = std::chrono::steady_clock::now();
    int lastPrintedSecond = -1;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for (const auto& notification : session.DrainNotifications()) {
            PrintEvent(notification.event);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        if (arguments.durationSeconds > 0 && elapsed >= arguments.durationSeconds) {
            break;
        }

        if (static_cast<int>(elapsed) / 5 != lastPrintedSecond) {
            lastPrintedSecond = static_cast<int>(elapsed) / 5;
            const mpm::MonitorStatus status = session.Status();
            std::cout << "[" << elapsed << "s] cpu=" << std::fixed << std::setprecision(1)
                      << status.systemCpu << "% idle=" << status.idleSeconds
                      << "s processes=" << status.processCount
                      << " top=" << mpm::Utf8(status.topName) << " " << status.topCpu
                      << "% events=" << status.eventCount << "\n";
        }
    }

    session.Stop();
    mpm::ShutdownLogging();
    return 0;
}

int RunEvents(const Arguments& arguments) {
    mpm::SqliteEventStore store;
    std::wstring error;
    if (!store.Open(ResolveDatabasePath(arguments), &error)) {
        std::cerr << "打开数据库失败: " << mpm::Utf8(error) << "\n";
        return 1;
    }

    const auto events = store.Recent(arguments.limit);
    if (arguments.json) {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& event : events) {
            array.push_back(mpm::ToJson(event));
        }
        std::cout << array.dump(2) << "\n";
        return 0;
    }

    if (events.empty()) {
        std::cout << "没有事件记录。\n";
        return 0;
    }

    std::cout << std::left << std::setw(20) << "开始时间" << std::setw(10) << "级别"
              << std::setw(7) << "评分" << std::setw(28) << "进程" << std::setw(10) << "峰值CPU"
              << "说明\n";
    for (const auto& event : events) {
        std::cout << std::left << std::setw(20) << mpm::LocalTimeString(event.startedUnixMs)
                  << std::setw(10) << mpm::ToString(event.severity) << std::setw(7)
                  << event.score << std::setw(28) << mpm::Utf8(event.culprit.displayName)
                  << std::setw(10) << std::fixed << std::setprecision(1)
                  << event.systemCpuPeak;
        if (event.knownMaintenance) {
            std::cout << "已知维护";
        } else {
            std::cout << (event.finished ? "已结束" : "进行中");
        }
        std::cout << "\n";
    }
    return 0;
}

int RunDoctor(const Arguments& arguments) {
    const std::filesystem::path dataDirectory = ResolveDataDirectory(arguments);
    const std::filesystem::path databasePath = ResolveDatabasePath(arguments);
    const std::filesystem::path configPath = ResolveConfigPath(arguments);
    std::wstring warning;
    const mpm::AppConfig config = mpm::LoadConfig(configPath, &warning);
    std::cout << "配置: " << mpm::Utf8(configPath.wstring()) << "\n";
    if (!warning.empty()) {
        std::cout << "配置提示: " << mpm::Utf8(warning) << "\n";
    }
    std::cout << "数据目录: " << mpm::Utf8(dataDirectory.wstring()) << "\n";
    std::cout << "数据库: " << mpm::Utf8(databasePath.wstring()) << "\n";

    mpm::Win32SystemSampler sampler;
    std::optional<mpm::SystemSample> sample;
    for (int attempt = 0; attempt < 3; ++attempt) {
        sample = sampler.Capture();
        if (sample) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "采样器: " << mpm::Utf8(sampler.Name()) << "\n";
    if (sample) {
        std::cout << "系统 CPU: " << std::fixed << std::setprecision(1)
                  << sample->systemCpuPercent << "%，逻辑处理器: " << sample->logicalCpuCount
                  << "，进程数: " << sample->processes.size() << "\n";
    } else {
        std::cout << "采样失败: " << mpm::Utf8(sampler.LastError()) << "\n";
    }

    mpm::Win32IdleProvider idle;
    const mpm::ActivityState activity = idle.Query();
    std::cout << "空闲: " << activity.idleSeconds << "s，锁屏: " << (activity.locked ? "是" : "否")
              << "，会话活跃: " << (activity.sessionActive ? "是" : "否") << "\n";

    mpm::SqliteEventStore store;
    std::wstring error;
    const bool opened = store.Open(databasePath, &error);
    std::cout << "数据库可写: " << (opened ? "是" : "否") << "\n";
    if (!opened) {
        std::cout << "数据库错误: " << mpm::Utf8(error) << "\n";
    }
    std::cout << "建议: 普通用户权限即可运行；若签名/受保护进程信息不完整，属于预期限制。\n";
    return opened ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    Arguments arguments;
    bool helpRequested = false;
    if (!ParseArguments(argc, argv, arguments, helpRequested)) {
        PrintUsage();
        return helpRequested ? 0 : 1;
    }

    if (arguments.command == "snapshot") {
        return RunSnapshot(arguments);
    }
    if (arguments.command == "watch") {
        return RunWatch(arguments);
    }
    if (arguments.command == "events") {
        return RunEvents(arguments);
    }
    if (arguments.command == "doctor") {
        return RunDoctor(arguments);
    }

    PrintUsage();
    return 1;
}