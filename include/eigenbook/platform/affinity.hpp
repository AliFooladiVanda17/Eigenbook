#pragma once

// Thread placement: CPU pinning, scheduling priority and the list of logical CPUs.
//
// Logical CPU numbers are the operating system's (0 .. logicalCpuCount() - 1). On Windows only
// processor group 0 is used (up to 64 logical CPUs).

#include <cstdint>
#include <optional>
#include <vector>

#include <eigenbook/platform/cpu_info.hpp>

namespace eigenbook::platform {

struct LogicalCpu {
    unsigned id{0};
    CoreType coreType{CoreType::UNKNOWN}; // from CPUID 0x1A, read while pinned to this CPU
    // Physical core this logical CPU belongs to (hyper-thread siblings share it), if known.
    // Windows: CoreIndex from GetSystemCpuSetInformation; Linux: topology/core_id.
    std::optional<unsigned> coreIndex;
    // Windows: EfficiencyClass from GetSystemCpuSetInformation (higher = faster core type).
    std::optional<std::uint8_t> efficiencyClass;
};

[[nodiscard]] unsigned logicalCpuCount() noexcept;

// CPU the calling thread is running on, if the OS can tell.
[[nodiscard]] std::optional<unsigned> currentCpu() noexcept;

// Restricts the calling thread to one logical CPU. Returns false on failure.
bool pinCurrentThread(unsigned cpu) noexcept;

// Removes any affinity restriction of the calling thread (all CPUs of the process).
bool unpinCurrentThread() noexcept;

// Raises the calling thread's priority as far as the OS allows without special rights:
// Windows THREAD_PRIORITY_TIME_CRITICAL + HIGH_PRIORITY_CLASS for the process; Linux
// SCHED_FIFO (priority 1) if `realtime` is set and permitted, otherwise nothing.
// Returns a short description of what was applied.
[[nodiscard]] const char* raiseThreadPriority(bool realtime) noexcept;

// Every logical CPU with its core type. Temporarily pins the calling thread to each CPU in turn
// (CPUID reports only the current core) and unpins it afterwards.
[[nodiscard]] std::vector<LogicalCpu> enumerateCpus();

// A good CPU for a single-threaded benchmark: a P-core on hybrid CPUs, avoiding CPU 0 (which
// usually takes most interrupts) and, where it can be told apart, the first sibling of each core.
[[nodiscard]] std::optional<unsigned> pickBenchmarkCpu(const std::vector<LogicalCpu>& cpus);

} // namespace eigenbook::platform
