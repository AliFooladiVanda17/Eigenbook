#pragma once

// CPU identification through CPUID (x86-64). On other architectures every query returns
// "unknown" / false.

#include <cstdint>
#include <string>
#include <string_view>

namespace eigenbook::platform {

// Core type of a hybrid Intel CPU (CPUID leaf 0x1A, EAX[31:24]).
enum class CoreType : std::uint8_t {
    UNKNOWN,     // not a hybrid CPU, or leaf 0x1A not available
    PERFORMANCE, // 0x40: "Core" (P-core)
    EFFICIENCY,  // 0x20: "Atom" (E-core)
};

[[nodiscard]] std::string_view toString(CoreType type) noexcept;

struct CpuInfo {
    std::string vendor;       // e.g. "GenuineIntel"
    std::string brand;        // e.g. "13th Gen Intel(R) Core(TM) i7-13650HX"
    bool invariantTsc{false}; // CPUID 0x80000007 EDX[8]: TSC rate independent of P/C-states
    bool rdtscp{false};       // CPUID 0x80000001 EDX[27]
    bool hybrid{false};       // CPUID 0x07 EDX[15]
};

// Properties shared by all cores (vendor, brand, feature bits).
[[nodiscard]] CpuInfo cpuInfo();

// Type of the core the calling thread is running on right now. Pin the thread first
// (pinCurrentThread) for a stable answer.
[[nodiscard]] CoreType currentCoreType() noexcept;

} // namespace eigenbook::platform
