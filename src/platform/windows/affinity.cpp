// Thread placement on Windows (processor group 0 only).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <windows.h>

#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/cpu_info.hpp>

namespace eigenbook::platform {

unsigned logicalCpuCount() noexcept {
    const DWORD n = GetActiveProcessorCount(0);
    return n > 64 ? 64U : static_cast<unsigned>(n);
}

std::optional<unsigned> currentCpu() noexcept {
    PROCESSOR_NUMBER pn{};
    GetCurrentProcessorNumberEx(&pn);
    if (pn.Group != 0) {
        return std::nullopt;
    }
    return static_cast<unsigned>(pn.Number);
}

bool pinCurrentThread(const unsigned cpu) noexcept {
    if (cpu >= logicalCpuCount()) {
        return false;
    }
    const DWORD_PTR mask = DWORD_PTR{1} << cpu;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        return false;
    }
    // The new mask takes effect at the next reschedule: yield until we are actually there, so a
    // CPUID right after this call describes the requested core.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (currentCpu() == cpu) {
            return true;
        }
        SwitchToThread();
    }
    return currentCpu() == cpu;
}

bool unpinCurrentThread() noexcept {
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) == 0) {
        return false;
    }
    return SetThreadAffinityMask(GetCurrentThread(), processMask) != 0;
}

const char* raiseThreadPriority(bool /*realtime*/) noexcept {
    const bool process = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
    const bool thread = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
    if (process && thread) {
        return "HIGH_PRIORITY_CLASS + THREAD_PRIORITY_TIME_CRITICAL";
    }
    if (thread) {
        return "THREAD_PRIORITY_TIME_CRITICAL (priority class unchanged)";
    }
    return "unchanged (SetThreadPriority failed)";
}

std::vector<LogicalCpu> enumerateCpus() {
    const unsigned count = logicalCpuCount();
    std::vector<LogicalCpu> cpus(count);
    for (unsigned i = 0; i < count; ++i) {
        cpus[i].id = i;
    }

    // Core index and efficiency class from the CPU-set API (Windows 10+).
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length > 0) {
        std::vector<unsigned char> buffer(length);
        if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                       length,
                                       &length,
                                       GetCurrentProcess(),
                                       0) != 0) {
            for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
                SYSTEM_CPU_SET_INFORMATION entry{};
                std::memcpy(&entry,
                            buffer.data() + offset,
                            std::min<std::size_t>(sizeof entry, length - offset));
                if (entry.Size == 0) {
                    break;
                }
                if (entry.Type == CpuSetInformation && entry.CpuSet.Group == 0 &&
                    entry.CpuSet.LogicalProcessorIndex < count) {
                    LogicalCpu& cpu = cpus[entry.CpuSet.LogicalProcessorIndex];
                    cpu.coreIndex = entry.CpuSet.CoreIndex;
                    cpu.efficiencyClass = entry.CpuSet.EfficiencyClass;
                }
                offset += entry.Size;
            }
        }
    }

    // Core type: CPUID reports the core it runs on, so visit each CPU.
    for (LogicalCpu& cpu : cpus) {
        if (pinCurrentThread(cpu.id)) {
            cpu.coreType = currentCoreType();
        }
    }
    unpinCurrentThread();
    return cpus;
}

} // namespace eigenbook::platform
