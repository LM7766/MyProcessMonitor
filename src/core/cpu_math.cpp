#include "mpm/core/cpu_math.hpp"

#include <algorithm>

namespace mpm {

double ComputeSystemCpuPercent(std::uint64_t delta100ns, double elapsedSeconds,
                               std::uint32_t logicalCpuCount) noexcept {
    if (elapsedSeconds <= 0.0 || logicalCpuCount == 0) {
        return 0.0;
    }
    const double capacity = elapsedSeconds * 10000000.0 * static_cast<double>(logicalCpuCount);
    if (capacity <= 0.0) {
        return 0.0;
    }
    const double percent = static_cast<double>(delta100ns) / capacity * 100.0;
    return std::clamp(percent, 0.0, 100.0);
}

double ComputeOneCoreCpuPercent(std::uint64_t delta100ns, double elapsedSeconds) noexcept {
    if (elapsedSeconds <= 0.0) {
        return 0.0;
    }
    const double percent = static_cast<double>(delta100ns) / (elapsedSeconds * 10000000.0) * 100.0;
    return std::max(0.0, percent);
}

double Clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

} // namespace mpm