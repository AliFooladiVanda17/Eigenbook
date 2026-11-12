// Platform pieces with one implementation for every OS: TSC calibration, CPU choice, pre-faulting.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/system_info.hpp>
#include <eigenbook/platform/tsc.hpp>

namespace eigenbook::platform {

TscCalibration calibrateTsc(const std::chrono::nanoseconds duration) {
#if EIGENBOOK_PLATFORM_HAS_TSC
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const std::uint64_t c0 = tscStart();
    auto t1 = t0;
    while ((t1 = Clock::now()) - t0 < duration) {
        cpuRelax();
    }
    const std::uint64_t c1 = tscStop();
    TscCalibration result;
    result.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
    result.ticks = c1 - c0;
    result.ticksPerSecond = static_cast<double>(result.ticks) * 1e9 /
                            static_cast<double>(result.duration.count());
    return result;
#else
    (void)duration;
    return {};
#endif
}

std::optional<unsigned> pickBenchmarkCpu(const std::vector<LogicalCpu>& cpus) {
    if (cpus.empty()) {
        return std::nullopt;
    }
    // Prefer P-cores; if CPUID gives no core type, fall back to the highest efficiency class.
    const bool anyP = std::ranges::any_of(cpus, [](const LogicalCpu& c) {
        return c.coreType == CoreType::PERFORMANCE;
    });
    std::uint8_t bestClass = 0;
    for (const LogicalCpu& c : cpus) {
        bestClass = std::max(bestClass, c.efficiencyClass.value_or(0));
    }
    const auto fast = [&](const LogicalCpu& c) {
        if (anyP) {
            return c.coreType == CoreType::PERFORMANCE;
        }
        return c.efficiencyClass.value_or(0) == bestClass;
    };
    // Avoid CPU 0 and its hyper-thread sibling.
    const std::optional<unsigned> core0 = cpus.front().id == 0 ? cpus.front().coreIndex
                                                               : std::nullopt;
    for (const LogicalCpu& c : cpus) {
        if (!fast(c) || c.id == 0 || (core0 && c.coreIndex == core0)) {
            continue;
        }
        return c.id;
    }
    for (const LogicalCpu& c : cpus) {
        if (fast(c)) {
            return c.id;
        }
    }
    return cpus.front().id;
}

void prefault(void* data, const std::size_t bytes) noexcept {
    if (data == nullptr || bytes == 0) {
        return;
    }
    auto* p = static_cast<volatile unsigned char*>(data);
    const std::size_t step = pageSize();
    for (std::size_t offset = 0; offset < bytes; offset += step) {
        p[offset] = p[offset];
    }
    p[bytes - 1] = p[bytes - 1];
}

} // namespace eigenbook::platform
