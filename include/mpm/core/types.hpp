#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mpm {

struct ProcessKey {
    std::uint32_t pid = 0;
    std::uint64_t createTime100ns = 0;

    bool operator==(const ProcessKey& other) const noexcept {
        return pid == other.pid && createTime100ns == other.createTime100ns;
    }
    bool operator!=(const ProcessKey& other) const noexcept { return !(*this == other); }
};

struct ProcessCpu {
    ProcessKey key;
    std::wstring imageName;
    std::uint32_t parentPid = 0;
    std::uint32_t sessionId = 0;
    double systemPercent = 0.0;   // 0..100 of all logical CPUs
    double oneCorePercent = 0.0;  // 0..100*N
    std::uint64_t workingSetBytes = 0;
};

struct SystemSample {
    std::uint64_t qpc = 0;
    std::uint64_t unixTimeMs = 0;
    double elapsedSeconds = 0.0;
    double systemCpuPercent = 0.0;
    double idleCpuPercent = 0.0;
    std::uint32_t logicalCpuCount = 0;
    std::vector<ProcessCpu> processes;
    bool valid = false;
};

struct ActivityState {
    std::uint64_t idleSeconds = 0;
    bool locked = false;
    bool displayOff = false;
    bool sessionActive = true;
    bool mediaPlaying = false;
    bool fullscreen = false;
    std::uint32_t foregroundPid = 0;
};

enum class SignatureState {
    Unknown = 0,
    Microsoft,
    Trusted,
    Untrusted,
    Unsigned,
    AccessDenied,
};

const wchar_t* ToString(SignatureState state) noexcept;

struct ProcessInfo {
    ProcessKey key;
    std::wstring imagePath;
    std::wstring imageName;
    std::wstring publisher;
    SignatureState signature = SignatureState::Unknown;
    std::wstring services;
    bool accessible = false;
};

struct AttributionGroup {
    std::wstring key;
    std::wstring displayName;
    std::wstring imagePath;
    std::wstring publisher;
    SignatureState signature = SignatureState::Unknown;
    std::wstring services;
    double cpuPercent = 0.0;
    double shareOfSystem = 0.0;
    std::vector<ProcessKey> members;
    bool knownMaintenance = false;
};

enum class Severity {
    Info = 0,
    Warning = 1,
    Critical = 2,
};

const char* ToString(Severity severity) noexcept;

struct DetectionEvent {
    std::string id;
    std::string episodeKey;
    std::uint64_t startedUnixMs = 0;
    std::uint64_t endedUnixMs = 0;
    Severity severity = Severity::Info;
    int score = 0;
    std::string trigger;
    double systemCpuAvg = 0.0;
    double systemCpuPeak = 0.0;
    double baselineP95 = 0.0;
    std::uint64_t idleSeconds = 0;
    std::uint64_t durationSeconds = 0;
    AttributionGroup culprit;
    std::vector<AttributionGroup> contributors;
    std::vector<std::string> reasons;
    bool knownMaintenance = false;
    bool mediaPlaying = false;
    bool fullscreenApp = false;
    std::uint32_t foregroundPid = 0;
    bool finished = false;
};

enum class DetectionUpdateKind {
    None = 0,
    Started,
    Updated,
    Finished,
};

struct DetectionUpdate {
    DetectionUpdateKind kind = DetectionUpdateKind::None;
    DetectionEvent event;
    bool notify = false;
};

} // namespace mpm