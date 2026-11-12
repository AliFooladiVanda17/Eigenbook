#pragma once

// Everything a benchmark result has to state about where it was measured.

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/cpu_info.hpp>
#include <eigenbook/platform/system_info.hpp>
#include <eigenbook/platform/tsc.hpp>

namespace eigenbook::bench {

struct BuildInfo {
    std::string compiler;  // e.g. "MSVC 19.44.35229.0"
    std::string buildType; // CMake configuration
    std::string flags;     // CMAKE_CXX_FLAGS + per-configuration flags
    std::string gitSha;    // at configure time; "-dirty" if the work tree had changes
};

struct Environment {
    platform::CpuInfo cpu;
    unsigned logicalCpus{0};
    unsigned physicalCores{0};
    unsigned performanceCpus{0}; // logical CPUs on P-cores (hybrid CPUs)
    std::optional<unsigned> pinnedCpu;
    platform::CoreType pinnedCoreType{platform::CoreType::UNKNOWN};
    std::string priority;
    std::string os;
    std::string power;
    std::string powerSource;
    bool largePages{false};
    platform::TscCalibration tsc;
    double tscSpreadPercent{0.0}; // max/min - 1 over the calibration repetitions
    BuildInfo build;
    // Timer overhead (empty tscStart/tscStop pair) in TSC ticks, at the start of the session and
    // again at the end. The TSC runs at a fixed rate, so a change means the core clock changed.
    std::uint64_t timerOverheadMin{0};
    std::uint64_t timerOverheadP50{0};
    std::uint64_t timerOverheadP99{0};
    std::uint64_t timerOverheadEndP50{0}; // 0 until measured

    [[nodiscard]] double clockDriftPercent() const noexcept {
        if (timerOverheadP50 == 0 || timerOverheadEndP50 == 0) {
            return 0.0;
        }
        return (static_cast<double>(timerOverheadEndP50) / static_cast<double>(timerOverheadP50) -
                1.0) *
               100.0;
    }

    [[nodiscard]] bool canReportNanoseconds() const noexcept {
        return cpu.invariantTsc && tsc.ticksPerSecond > 0.0;
    }
};

inline void print(std::ostream& out, const Environment& e) {
    out << "CPU            : " << e.cpu.brand << " (" << e.cpu.vendor << ")\n"
        << "Logical CPUs   : " << e.logicalCpus << " (" << e.physicalCores << " cores";
    if (e.cpu.hybrid) {
        out << ", hybrid: " << e.performanceCpus << " logical CPUs on P-cores";
    }
    out << ")\n"
        << "Pinned to      : ";
    if (e.pinnedCpu) {
        out << "CPU " << *e.pinnedCpu << " (" << platform::toString(e.pinnedCoreType) << ")";
    } else {
        out << "not pinned";
    }
    out << "\n"
        << "Priority       : " << e.priority << "\n"
        << "OS             : " << e.os << "\n"
        << "Power policy   : " << e.power << "\n"
        << "Power source   : " << e.powerSource << "\n"
        << "Large pages    : " << (e.largePages ? "available" : "not available") << "\n"
        << "Invariant TSC  : " << (e.cpu.invariantTsc ? "yes" : "NO (nanoseconds not reported)")
        << "\n"
        << "TSC frequency  : " << e.tsc.ticksPerSecond / 1e9 << " GHz (calibrated over "
        << static_cast<double>(e.tsc.duration.count()) / 1e9 << " s, spread " << e.tscSpreadPercent
        << " %)\n"
        << "Timer overhead : min " << e.timerOverheadMin << ", p50 " << e.timerOverheadP50
        << ", p99 " << e.timerOverheadP99
        << " TSC ticks (empty tscStart/tscStop pair, not subtracted)\n";
    if (e.timerOverheadEndP50 != 0) {
        out << "Clock check    : timer overhead p50 " << e.timerOverheadP50 << " at start, "
            << e.timerOverheadEndP50 << " at end (" << e.clockDriftPercent() << " %)"
            << (e.clockDriftPercent() > 10.0 || e.clockDriftPercent() < -10.0
                    ? "  WARNING: the core clock changed during the session"
                    : "")
            << "\n";
    }
    out << "Compiler       : " << e.build.compiler << "\n"
        << "Build type     : " << e.build.buildType << "\n"
        << "Flags          : " << e.build.flags << "\n"
        << "Git            : " << e.build.gitSha << "\n";
}

} // namespace eigenbook::bench
