#include "mpm/app/monitor_session.hpp"

#include "mpm/core/detector.hpp"
#include "mpm/infra/logging.hpp"
#include "mpm/infra/paths.hpp"
#include "mpm/infra/text.hpp"
#include "mpm/win32/idle_provider.hpp"
#include "mpm/win32/process_catalog.hpp"
#include "mpm/win32/system_sampler.hpp"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <utility>

namespace mpm {
namespace {

std::uint64_t NowUnixMilliseconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

MonitorSession::MonitorSession(AppConfig config, std::filesystem::path databasePath,
                               std::filesystem::path logDirectory)
    : config_(std::move(config)),
      databasePath_(std::move(databasePath)),
      logDirectory_(std::move(logDirectory)) {}

MonitorSession::~MonitorSession() {
    Stop();
}

bool MonitorSession::Start(std::wstring* error) {
    if (started_) {
        return true;
    }

    std::wstring directoryError;
    if (!EnsureAppDirectories(&directoryError)) {
        if (error != nullptr) {
            *error = directoryError;
        }
        return false;
    }

    std::wstring logError;
    if (!InitLogging(logDirectory_, config_.logLevel, &logError)) {
        if (error != nullptr) {
            *error = logError;
        }
        return false;
    }

    store_ = std::make_unique<SqliteEventStore>();
    std::wstring storeError;
    if (!store_->Open(databasePath_, &storeError)) {
        Log()->error("数据库打开失败: {}", Utf8(storeError));
        status_.lastError = storeError;
    } else {
        const std::uint64_t retentionMs =
            static_cast<std::uint64_t>(std::max(1, config_.retentionDays)) * 24ULL * 60ULL * 60ULL *
            1000ULL;
        const std::uint64_t now = NowUnixMilliseconds();
        store_->PruneBefore(now > retentionMs ? now - retentionMs : 0);
    }

    sampler_ = std::make_unique<Win32SystemSampler>();
    idle_ = std::make_unique<Win32IdleProvider>();
    catalog_ = std::make_unique<Win32ProcessCatalog>();
    detector_ = std::make_unique<Detector>(config_, catalog_.get(), GetCurrentProcessId());

    stop_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = MonitorStatus{};
        status_.running = true;
        status_.warmup = true;
        status_.sampler = sampler_->Name();
    }

    started_ = true;
    thread_ = std::thread(&MonitorSession::ThreadMain, this);
    Log()->info("监控已启动，采样器={}，数据库={}", Utf8(sampler_->Name()),
                Utf8(databasePath_.wstring()));
    return true;
}

void MonitorSession::Stop() {
    if (!started_) {
        return;
    }
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    started_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.running = false;
    }
    if (store_) {
        store_->Close();
    }
    Log()->info("监控已停止");
}

void MonitorSession::PauseFor(std::chrono::seconds duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = true;
    pauseUntil_ = std::chrono::steady_clock::now() + duration;
    wake_.notify_all();
}

void MonitorSession::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = false;
    wake_.notify_all();
}

void MonitorSession::WaitForNextSample(int milliseconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(std::max(1, milliseconds)));
}

void MonitorSession::SetLastError(const std::wstring& error) {
    if (error.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.lastError = error;
}

void MonitorSession::ThreadMain() {
    bool wasPaused = false;
    std::uint64_t sampleCount = 0;
    auto logger = Log();

    while (!stop_.load()) {
        bool pausedNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (paused_ && std::chrono::steady_clock::now() >= pauseUntil_) {
                paused_ = false;
            }
            pausedNow = paused_;
        }

        if (pausedNow) {
            if (!wasPaused) {
                detector_->Reset();
                wasPaused = true;
            }
            WaitForNextSample(200);
            continue;
        }
        if (wasPaused) {
            detector_->Reset();
            wasPaused = false;
        }

        std::optional<SystemSample> sample = sampler_->Capture();
        if (!sample) {
            const std::wstring samplerError = sampler_->LastError();
            if (!samplerError.empty()) {
                SetLastError(samplerError);
            }
            WaitForNextSample(config_.samplingIntervalMs);
            continue;
        }

        const ActivityState activity = idle_->Query();
        const DetectionUpdate update = detector_->Evaluate(*sample, activity);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.warmup = false;
            status_.systemCpu = sample->systemCpuPercent;
            status_.idleSeconds = activity.idleSeconds;
            status_.locked = activity.locked;
            status_.mediaPlaying = activity.mediaPlaying;
            status_.fullscreen = activity.fullscreen;
            status_.processCount = static_cast<std::uint32_t>(sample->processes.size());
            status_.sampler = sampler_->Name();
            const std::wstring samplerError = sampler_->LastError();
            if (!samplerError.empty()) {
                status_.lastError = samplerError;
            }

            const auto top = std::max_element(
                sample->processes.begin(), sample->processes.end(),
                [](const ProcessCpu& left, const ProcessCpu& right) {
                    return left.systemPercent < right.systemPercent;
                });
            if (top != sample->processes.end()) {
                status_.topName = top->imageName;
                status_.topCpu = top->systemPercent;
            }

            if (update.kind == DetectionUpdateKind::Started) {
                ++status_.eventCount;
            }
        }

        if (update.kind != DetectionUpdateKind::None) {
            if (store_ && !store_->Upsert(update.event)) {
                SetLastError(store_->LastError());
            }
            if (update.kind == DetectionUpdateKind::Started ||
                update.kind == DetectionUpdateKind::Finished || update.notify) {
                logger->info("event {} {} score={} culprit={} cpu={:.1f}%", update.event.id,
                             ToString(update.event.severity), update.event.score,
                             Utf8(update.event.culprit.displayName),
                             update.event.systemCpuPeak);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (notifications_.size() >= 64) {
                notifications_.pop_front();
            }
            notifications_.push_back(MonitorNotification{update.event, update.notify});
        }

        ++sampleCount;
        (void)sampleCount;
        WaitForNextSample(config_.samplingIntervalMs);
    }
}

MonitorStatus MonitorSession::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::vector<MonitorNotification> MonitorSession::DrainNotifications() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MonitorNotification> result;
    result.reserve(notifications_.size());
    while (!notifications_.empty()) {
        result.push_back(std::move(notifications_.front()));
        notifications_.pop_front();
    }
    return result;
}

SqliteEventStore* MonitorSession::Store() {
    return store_.get();
}

} // namespace mpm