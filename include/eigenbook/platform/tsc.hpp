#pragma once

// Time-stamp counter access for latency measurement (x86-64 only).
//
// tscStart() / tscStop() follow the usual serialisation pattern for timing a short code region:
//   start: lfence; rdtsc        (earlier instructions have completed before the read)
//   stop:  rdtscp; lfence       (the measured instructions have completed; later ones wait)
// Convert ticks to time only if the TSC is invariant (cpuInfo().invariantTsc) and calibrated.

#include <chrono>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#define EIGENBOOK_PLATFORM_HAS_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#else
#define EIGENBOOK_PLATFORM_HAS_TSC 0
#endif

namespace eigenbook::platform {

inline constexpr bool kHasTsc = EIGENBOOK_PLATFORM_HAS_TSC != 0;

#if EIGENBOOK_PLATFORM_HAS_TSC
[[nodiscard]] inline std::uint64_t tscStart() noexcept {
    _mm_lfence();
    return __rdtsc();
}

[[nodiscard]] inline std::uint64_t tscStop() noexcept {
    unsigned int aux = 0;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// Plain read, no fences: for pacing loops, not for measuring.
[[nodiscard]] inline std::uint64_t tscNow() noexcept {
    return __rdtsc();
}

inline void cpuRelax() noexcept {
    _mm_pause();
}
#endif

struct TscCalibration {
    double ticksPerSecond{0.0};
    std::chrono::nanoseconds duration{0}; // steady_clock time the calibration covered
    std::uint64_t ticks{0};               // TSC ticks over the same interval

    [[nodiscard]] double ticksPerNanosecond() const noexcept { return ticksPerSecond / 1e9; }

    [[nodiscard]] double toNanoseconds(const double ticksValue) const noexcept {
        return ticksValue / ticksPerNanosecond();
    }
};

// Busy-waits for `duration` (at least 1 s is recommended) and compares TSC ticks with
// steady_clock. Returns ticksPerSecond == 0 where there is no TSC.
[[nodiscard]] TscCalibration calibrateTsc(std::chrono::nanoseconds duration);

} // namespace eigenbook::platform
