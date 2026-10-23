#include "mpm/core/detector.hpp"

#include "mpm/core/cpu_math.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <map>
#include <sstream>

namespace mpm {
namespace {

std::wstring ToLowerAscii(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& ch : result) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return result;
}

bool ContainsNoCase(const std::wstring& haystack, std::wstring_view needle) {
    if (needle.empty()) {
        return true;
    }
    const std::wstring lowerHaystack = ToLowerAscii(haystack);
    const std::wstring lowerNeedle = ToLowerAscii(needle);
    return lowerHaystack.find(lowerNeedle) != std::wstring::npos;
}

bool IsExcludedImage(const std::wstring& lowerName) {
    return lowerName == L"system idle process" || lowerName == L"system" ||
           lowerName == L"registry" || lowerName == L"memory compression" ||
           lowerName == L"secure system" || lowerName.empty();
}

bool HasMember(const AttributionGroup& group, std::uint32_t pid) {
    if (pid == 0) {
        return false;
    }
    for (const auto& member : group.members) {
        if (member.pid == pid) {
            return true;
        }
    }
    return false;
}

std::wstring JoinGroups(const std::vector<AttributionGroup>& groups) {
    std::wstring result;
    for (const auto& group : groups) {
        if (!result.empty()) {
            result += L", ";
        }
        result += group.displayName;
    }
    return result;
}

} // namespace

Detector::Detector(AppConfig config, IProcessCatalog* catalog, std::uint32_t selfPid)
    : config_(config), catalog_(catalog), selfPid_(selfPid) {}

void Detector::Reset() {
    episodeActive_ = false;
    firstSampleMs_.reset();
    armSinceMs_.reset();
    belowSinceMs_ = 0;
    cooldownUntilMs_ = 0;
    episodeSamples_ = 0;
    episodeCpuSum_ = 0.0;
    episodeCpuPeak_ = 0.0;
    culpritPeakCpu_ = 0.0;
    episodeIdlePeak_ = 0;
    maxScore_ = 0;
    lastNotifiedSeverity_ = Severity::Info;
    event_ = DetectionEvent{};
    infoCache_.clear();
}

std::vector<AttributionGroup> Detector::BuildGroups(const SystemSample& sample) {
    std::map<std::wstring, AttributionGroup> byName;
    double processSum = 0.0;

    for (const auto& process : sample.processes) {
        if (process.key.pid == 0 || process.key.pid == 4 || process.key.pid == selfPid_) {
            continue;
        }
        if (process.systemPercent < 0.01) {
            continue;
        }
        const std::wstring lower = ToLowerAscii(process.imageName);
        if (IsExcludedImage(lower)) {
            continue;
        }

        processSum += process.systemPercent;
        auto [it, inserted] = byName.try_emplace(lower);
        AttributionGroup& group = it->second;
        if (inserted) {
            group.key = lower;
            group.displayName = process.imageName;
        }
        group.cpuPercent += process.systemPercent;
        group.members.push_back(process.key);
    }

    std::vector<AttributionGroup> groups;
    groups.reserve(byName.size());
    for (auto& [key, group] : byName) {
        (void)key;
        if (sample.systemCpuPercent > 0.01) {
            group.shareOfSystem = group.cpuPercent / sample.systemCpuPercent * 100.0;
        }
        groups.push_back(std::move(group));
    }

    std::sort(groups.begin(), groups.end(), [](const auto& left, const auto& right) {
        return left.cpuPercent > right.cpuPercent;
    });

    const std::size_t resolveCount = std::min<std::size_t>(groups.size(), 3);
    for (std::size_t index = 0; index < resolveCount; ++index) {
        ResolveGroup(groups[index]);
    }

    (void)processSum;
    return groups;
}

void Detector::ResolveGroup(AttributionGroup& group) {
    if (!catalog_ || group.members.empty()) {
        return;
    }

    auto cached = infoCache_.find(group.key);
    if (cached == infoCache_.end()) {
        ProcessInfo info = catalog_->Describe(group.members.front());
        if (info.accessible || !info.imagePath.empty()) {
            cached = infoCache_.emplace(group.key, std::move(info)).first;
        } else {
            return;
        }
    }

    const ProcessInfo& info = cached->second;
    group.imagePath = info.imagePath;
    if (group.displayName.empty()) {
        group.displayName = info.imageName;
    }
    group.publisher = info.publisher;
    group.signature = info.signature;
    group.services = info.services;
    group.knownMaintenance = IsKnownMaintenance(group);
}

bool Detector::IsKnownMaintenance(const AttributionGroup& group) const {
    const std::wstring name = ToLowerAscii(group.displayName);
    const bool trustedPath =
        group.signature == SignatureState::Microsoft ||
        group.signature == SignatureState::Trusted ||
        ContainsNoCase(group.imagePath, L"\\windows\\") ||
        ContainsNoCase(group.publisher, L"microsoft");

    if (!trustedPath) {
        return false;
    }

    static const wchar_t* const knownNames[] = {
        L"msmpeng.exe",
        L"mpdefendercoreservice.exe",
        L"nissrv.exe",
        L"tiworker.exe",
        L"trustedinstaller.exe",
        L"mousocoreworker.exe",
        L"usoclient.exe",
        L"searchindexer.exe",
        L"mscorsvw.exe",
        L"compattelrunner.exe",
        L"defrag.exe",
        L"dismhost.exe",
        L"wmiprvse.exe",
    };

    for (const wchar_t* known : knownNames) {
        if (name == known) {
            return true;
        }
    }

    if (name == L"svchost.exe" && ContainsNoCase(group.services, L"sysmain")) {
        return true;
    }

    return false;
}

double Detector::TrustFactor(const AttributionGroup& group) const {
    double trust = 0.6;
    switch (group.signature) {
        case SignatureState::Microsoft:
            trust = 0.0;
            break;
        case SignatureState::Trusted:
            trust = 0.3;
            break;
        case SignatureState::Untrusted:
            trust = 0.6;
            break;
        case SignatureState::Unsigned:
            trust = 1.0;
            break;
        case SignatureState::Unknown:
        case SignatureState::AccessDenied:
            trust = 0.6;
            break;
    }

    const std::wstring path = ToLowerAscii(group.imagePath);
    if (path.find(L"\\appdata\\local\\temp\\") != std::wstring::npos ||
        path.find(L"\\downloads\\") != std::wstring::npos ||
        path.find(L"\\users\\public\\") != std::wstring::npos) {
        trust = std::max(trust, 0.9);
    }
    return trust;
}

int Detector::ComputeScore(const SystemSample& sample, const AttributionGroup& top,
                           std::uint64_t durationSeconds) const {
    const double systemFactor = Clamp01(
        (sample.systemCpuPercent - config_.systemCpuTriggerPercent) / 25.0 + 0.5);
    const double persistence =
        Clamp01((static_cast<double>(durationSeconds) - 30.0) / 270.0);
    const double share = Clamp01(top.cpuPercent / std::max(sample.systemCpuPercent, 0.1));
    const double dominance = Clamp01((share - 0.20) / 0.80);
    const double novelty = 0.35;
    const double trust = TrustFactor(top);

    double score = 100.0 *
                   (0.35 * systemFactor + 0.20 * persistence + 0.20 * dominance +
                    0.15 * novelty + 0.10 * trust);
    if (top.knownMaintenance) {
        score = std::min(score, static_cast<double>(config_.knownMaintenanceCap));
    }
    return static_cast<int>(std::lround(std::max(0.0, std::min(100.0, score))));
}

Severity Detector::SeverityForScore(int score) const {
    if (score >= config_.criticalScore) {
        return Severity::Critical;
    }
    if (score >= config_.warningScore) {
        return Severity::Warning;
    }
    return Severity::Info;
}

std::vector<std::string> Detector::BuildReasons(const SystemSample& sample,
                                                const AttributionGroup& top,
                                                std::uint64_t durationSeconds,
                                                bool unattributed) const {
    std::vector<std::string> reasons;
    reasons.emplace_back("system_cpu_above_trigger");
    reasons.emplace_back("sustained_" + std::to_string(durationSeconds) + "s");
    if (top.shareOfSystem >= 50.0) {
        reasons.emplace_back("top_group_dominant");
    }
    if (top.knownMaintenance) {
        reasons.emplace_back("known_maintenance_process");
    }
    if (top.signature == SignatureState::Unsigned) {
        reasons.emplace_back("unsigned_executable");
    } else if (top.signature == SignatureState::Unknown ||
               top.signature == SignatureState::AccessDenied) {
        reasons.emplace_back("unknown_publisher");
    }
    if (unattributed) {
        reasons.emplace_back("unattributed_cpu");
    }
    if (sample.systemCpuPercent >= 40.0) {
        reasons.emplace_back("high_system_cpu");
    }
    return reasons;
}

DetectionUpdate Detector::StartEpisode(const SystemSample& sample, const ActivityState& activity,
                                       std::vector<AttributionGroup> groups) {
    if (groups.empty()) {
        return {};
    }

    // Make sure the top group has metadata before it is written to the DB.
    ResolveGroup(groups.front());

    episodeActive_ = true;
    episodeSamples_ = 1;
    episodeCpuSum_ = sample.systemCpuPercent;
    episodeCpuPeak_ = sample.systemCpuPercent;
    culpritPeakCpu_ = groups.front().cpuPercent;
    episodeIdlePeak_ = activity.idleSeconds;
    belowSinceMs_ = 0;
    armSinceMs_.reset();

    event_ = DetectionEvent{};
    event_.startedUnixMs = sample.unixTimeMs;
    event_.endedUnixMs = 0;
    event_.episodeKey = "idle-" + std::to_string(sample.unixTimeMs);
    event_.id = event_.episodeKey + "-" + std::to_string(++eventSequence_);
    event_.trigger = "idle_cpu_sustained";
    event_.systemCpuAvg = sample.systemCpuPercent;
    event_.systemCpuPeak = sample.systemCpuPercent;
    event_.idleSeconds = activity.idleSeconds;
    event_.durationSeconds = 0;
    event_.culprit = groups.front();
    event_.knownMaintenance = groups.front().knownMaintenance;
    event_.mediaPlaying = activity.mediaPlaying;
    event_.fullscreenApp = activity.fullscreen;
    event_.foregroundPid = activity.foregroundPid;

    for (std::size_t index = 1; index < groups.size() && event_.contributors.size() < 5;
         ++index) {
        if (groups[index].cpuPercent >= 1.0) {
            event_.contributors.push_back(groups[index]);
        }
    }

    const bool unattributed =
        sample.systemCpuPercent - (groups.front().cpuPercent) > sample.systemCpuPercent * 0.5 &&
        sample.systemCpuPercent > 2.0;
    event_.reasons =
        BuildReasons(sample, groups.front(), 0, unattributed);

    maxScore_ = ComputeScore(sample, groups.front(), 0);
    if (HasMember(groups.front(), activity.foregroundPid)) {
        maxScore_ = std::min(maxScore_, config_.warningScore - 1);
        event_.reasons.emplace_back("foreground_process");
    }
    event_.score = maxScore_;
    event_.severity = SeverityForScore(maxScore_);

    DetectionUpdate update;
    update.kind = DetectionUpdateKind::Started;
    update.event = event_;
    if (event_.severity != Severity::Info) {
        update.notify = true;
        lastNotifiedSeverity_ = event_.severity;
    }
    return update;
}

DetectionUpdate Detector::UpdateEpisode(const SystemSample& sample,
                                        const ActivityState& activity,
                                        std::vector<AttributionGroup> groups, bool above) {
    if (groups.empty()) {
        return FinishEpisode(sample, "no_attributable_process");
    }

    ResolveGroup(groups.front());
    const AttributionGroup& top = groups.front();

    ++episodeSamples_;
    episodeCpuSum_ += sample.systemCpuPercent;
    episodeCpuPeak_ = std::max(episodeCpuPeak_, sample.systemCpuPercent);
    episodeIdlePeak_ = std::max(episodeIdlePeak_, activity.idleSeconds);
    if (top.cpuPercent > culpritPeakCpu_) {
        culpritPeakCpu_ = top.cpuPercent;
        event_.culprit = top;
        event_.knownMaintenance = top.knownMaintenance;
    }

    const std::uint64_t durationSeconds =
        sample.unixTimeMs > event_.startedUnixMs
            ? (sample.unixTimeMs - event_.startedUnixMs) / 1000
            : 0;
    event_.systemCpuAvg = episodeCpuSum_ / static_cast<double>(episodeSamples_);
    event_.systemCpuPeak = episodeCpuPeak_;
    event_.idleSeconds = episodeIdlePeak_;
    event_.durationSeconds = durationSeconds;
    event_.mediaPlaying = activity.mediaPlaying;
    event_.fullscreenApp = activity.fullscreen;
    event_.foregroundPid = activity.foregroundPid;

    event_.contributors.clear();
    for (std::size_t index = 1; index < groups.size() && event_.contributors.size() < 5;
         ++index) {
        if (groups[index].cpuPercent >= 1.0) {
            event_.contributors.push_back(groups[index]);
        }
    }

    const bool unattributed =
        sample.systemCpuPercent - top.cpuPercent > sample.systemCpuPercent * 0.5 &&
        sample.systemCpuPercent > 2.0;
    event_.reasons = BuildReasons(sample, top, durationSeconds, unattributed);

    int score = ComputeScore(sample, top, durationSeconds);
    if (HasMember(top, activity.foregroundPid)) {
        score = std::min(score, config_.warningScore - 1);
        event_.reasons.emplace_back("foreground_process");
    }
    maxScore_ = std::max(maxScore_, score);
    event_.score = maxScore_;
    event_.severity = SeverityForScore(maxScore_);

    DetectionUpdate update;
    update.kind = DetectionUpdateKind::Updated;
    update.event = event_;
    if (event_.severity > lastNotifiedSeverity_) {
        update.notify = true;
        lastNotifiedSeverity_ = event_.severity;
    }

    if (!above) {
        if (belowSinceMs_ == 0) {
            belowSinceMs_ = sample.unixTimeMs;
        }
        const std::uint64_t belowMs =
            sample.unixTimeMs > belowSinceMs_ ? sample.unixTimeMs - belowSinceMs_ : 0;
        if (belowMs >= static_cast<std::uint64_t>(std::max(0, config_.exitSustainSeconds)) *
                           1000ULL) {
            return FinishEpisode(sample, "cpu_below_exit_threshold");
        }
    } else {
        belowSinceMs_ = 0;
    }

    // While below the exit threshold we still keep the event open, but avoid
    // writing an update every second when nothing material changed.
    if (!above) {
        update.kind = DetectionUpdateKind::None;
        update.notify = false;
    }
    return update;
}

DetectionUpdate Detector::FinishEpisode(const SystemSample& sample, const char* reason) {
    if (!episodeActive_) {
        return {};
    }

    event_.endedUnixMs = sample.unixTimeMs;
    event_.durationSeconds =
        sample.unixTimeMs > event_.startedUnixMs
            ? (sample.unixTimeMs - event_.startedUnixMs) / 1000
            : event_.durationSeconds;
    event_.finished = true;
    event_.score = maxScore_;
    event_.severity = SeverityForScore(maxScore_);
    if (reason != nullptr && reason[0] != '\0') {
        event_.reasons.emplace_back(reason);
    }

    DetectionUpdate update;
    update.kind = DetectionUpdateKind::Finished;
    update.event = event_;
    update.notify = false;

    cooldownUntilMs_ = sample.unixTimeMs +
                       static_cast<std::uint64_t>(std::max(0, config_.cooldownSeconds)) * 1000ULL;
    episodeActive_ = false;
    armSinceMs_.reset();
    belowSinceMs_ = 0;
    episodeSamples_ = 0;
    episodeCpuSum_ = 0.0;
    episodeCpuPeak_ = 0.0;
    culpritPeakCpu_ = 0.0;
    episodeIdlePeak_ = 0;
    maxScore_ = 0;
    lastNotifiedSeverity_ = Severity::Info;
    return update;
}

DetectionUpdate Detector::Evaluate(const SystemSample& sample, const ActivityState& activity) {
    if (!sample.valid || sample.logicalCpuCount == 0) {
        return {};
    }

    if (!firstSampleMs_) {
        firstSampleMs_ = sample.unixTimeMs;
    } else if (sample.unixTimeMs < *firstSampleMs_) {
        firstSampleMs_ = sample.unixTimeMs;
        Reset();
    }

    if (!episodeActive_ && config_.startupGraceSeconds > 0 &&
        sample.unixTimeMs >= *firstSampleMs_ &&
        sample.unixTimeMs - *firstSampleMs_ <
            static_cast<std::uint64_t>(config_.startupGraceSeconds) * 1000ULL) {
        return {};
    }

    const bool lockedIdle = activity.locked && config_.treatLockedAsIdle;
    const bool displayOffIdle = activity.displayOff && config_.treatDisplayOffAsIdle;
    bool idleEligible = activity.idleSeconds >=
                            static_cast<std::uint64_t>(std::max(0, config_.idleThresholdSeconds)) ||
                        lockedIdle || displayOffIdle;
    if (!activity.sessionActive) {
        idleEligible = false;
    }

    const bool mediaBlocked =
        (config_.suppressWhenMediaPlaying && activity.mediaPlaying) ||
        (config_.suppressWhenFullscreen && activity.fullscreen);

    if (!idleEligible || mediaBlocked) {
        if (episodeActive_) {
            return FinishEpisode(sample, mediaBlocked ? "user_media_active" : "input_resumed");
        }
        armSinceMs_.reset();
        return {};
    }

    if (!episodeActive_ && sample.unixTimeMs < cooldownUntilMs_) {
        return {};
    }

    std::vector<AttributionGroup> groups = BuildGroups(sample);
    if (groups.empty()) {
        if (episodeActive_) {
            return FinishEpisode(sample, "no_attributable_process");
        }
        armSinceMs_.reset();
        return {};
    }

    const AttributionGroup& top = groups.front();
    const bool hasCandidate = top.cpuPercent >= 1.0 || top.shareOfSystem >= 20.0;
    const double busyCores = sample.systemCpuPercent / 100.0 * sample.logicalCpuCount;
    const double requiredCores = std::max(config_.minBusyCores, 0.06 * sample.logicalCpuCount);
    const bool above = hasCandidate &&
                       sample.systemCpuPercent >= config_.systemCpuTriggerPercent &&
                       busyCores >= requiredCores;

    if (!episodeActive_) {
        if (!above) {
            armSinceMs_.reset();
            return {};
        }
        if (!armSinceMs_) {
            armSinceMs_ = sample.unixTimeMs;
        }
        const std::uint64_t armedMs =
            sample.unixTimeMs > *armSinceMs_ ? sample.unixTimeMs - *armSinceMs_ : 0;
        if (armedMs < static_cast<std::uint64_t>(std::max(1, config_.sustainSeconds)) * 1000ULL) {
            return {};
        }
        return StartEpisode(sample, activity, std::move(groups));
    }

    return UpdateEpisode(sample, activity, std::move(groups), above);
}

} // namespace mpm