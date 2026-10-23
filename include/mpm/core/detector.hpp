#pragma once

#include "mpm/core/config.hpp"
#include "mpm/core/interfaces.hpp"
#include "mpm/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mpm {

class Detector {
public:
    Detector(AppConfig config, IProcessCatalog* catalog, std::uint32_t selfPid);

    DetectionUpdate Evaluate(const SystemSample& sample, const ActivityState& activity);
    void Reset();

private:
    std::vector<AttributionGroup> BuildGroups(const SystemSample& sample);
    void ResolveGroup(AttributionGroup& group);
    bool IsKnownMaintenance(const AttributionGroup& group) const;
    double TrustFactor(const AttributionGroup& group) const;
    int ComputeScore(const SystemSample& sample, const AttributionGroup& top,
                     std::uint64_t durationSeconds) const;
    Severity SeverityForScore(int score) const;
    std::vector<std::string> BuildReasons(const SystemSample& sample,
                                          const AttributionGroup& top,
                                          std::uint64_t durationSeconds,
                                          bool unattributed) const;

    DetectionUpdate StartEpisode(const SystemSample& sample, const ActivityState& activity,
                                 std::vector<AttributionGroup> groups);
    DetectionUpdate UpdateEpisode(const SystemSample& sample, const ActivityState& activity,
                                  std::vector<AttributionGroup> groups, bool above);
    DetectionUpdate FinishEpisode(const SystemSample& sample, const char* reason);

    AppConfig config_;
    IProcessCatalog* catalog_ = nullptr;
    std::uint32_t selfPid_ = 0;

    bool episodeActive_ = false;
    std::optional<std::uint64_t> firstSampleMs_;
    std::optional<std::uint64_t> armSinceMs_;
    std::uint64_t belowSinceMs_ = 0;
    std::uint64_t cooldownUntilMs_ = 0;
    std::uint64_t episodeSamples_ = 0;
    double episodeCpuSum_ = 0.0;
    double episodeCpuPeak_ = 0.0;
    double culpritPeakCpu_ = 0.0;
    std::uint64_t episodeIdlePeak_ = 0;
    int maxScore_ = 0;
    Severity lastNotifiedSeverity_ = Severity::Info;
    std::uint64_t eventSequence_ = 0;

    DetectionEvent event_;
    std::unordered_map<std::wstring, ProcessInfo> infoCache_;
};

} // namespace mpm