#pragma once

#include <cstdint>

namespace mpm {

// delta100ns is the CPU time consumed during elapsedSeconds.
// Returns the percentage of the whole machine (0..100).
double ComputeSystemCpuPercent(std::uint64_t delta100ns, double elapsedSeconds,
                               std::uint32_t logicalCpuCount) noexcept;

// Returns the percentage of one logical CPU (0..100*N).
double ComputeOneCoreCpuPercent(std::uint64_t delta100ns, double elapsedSeconds) noexcept;

double Clamp01(double value) noexcept;

} // namespace mpm