#pragma once

#include "mpm/core/config.hpp"
#include "mpm/core/types.hpp"
#include "mpm/infra/sqlite_event_store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mpm {

class Win32SystemSampler;
class Win32IdleProvider;
class Win32ProcessCatalog;
class Detector;

struct MonitorStatus {
    bool running = false;
    bool paused = false;
    bool warmup = true;
    double systemCpu = 0.0;
    std::uint64_t idleSeconds = 0;
    bool locked = false;
    bool mediaPlaying = false;
    bool fullscreen = false;
    std::uint32_t processCount = 0;
    std::wstring topName;
    double topCpu = 0.0;
    std::wstring sampler;
    std::wstring lastError;
    std::uint64_t eventCount = 0;
};

struct MonitorNotification {
    DetectionEvent event;
    bool notify = false;
};

class MonitorSession {
public:
    MonitorSession(AppConfig config, std::filesystem::path databasePath,
                   std::filesystem::path logDirectory);
    ~MonitorSession();

    MonitorSession(const MonitorSession&) = delete;
    MonitorSession& operator=(const MonitorSession&) = delete;

    bool Start(std::wstring* error);
    void Stop();

    void PauseFor(std::chrono::seconds duration);
    void Resume();

    MonitorStatus Status() const;
    std::vector<MonitorNotification> DrainNotifications();
    SqliteEventStore* Store();

private:
    void ThreadMain();
    void WaitForNextSample(int milliseconds);
    void SetLastError(const std::wstring& error);

    AppConfig config_;
    std::filesystem::path databasePath_;
    std::filesystem::path logDirectory_;

    std::unique_ptr<SqliteEventStore> store_;
    std::unique_ptr<Win32SystemSampler> sampler_;
    std::unique_ptr<Win32IdleProvider> idle_;
    std::unique_ptr<Win32ProcessCatalog> catalog_;
    std::unique_ptr<Detector> detector_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<MonitorNotification> notifications_;
    MonitorStatus status_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    bool paused_ = false;
    std::chrono::steady_clock::time_point pauseUntil_{};
};

} // namespace mpm