// Thread placement on Linux.
//
// Not compiled or run in the Windows environment Phase A2 was developed in; see
// docs/BENCHMARKS.md.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/cpu_info.hpp>

namespace eigenbook::platform {

unsigned logicalCpuCount() noexcept {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<unsigned>(n) : 1U;
}

std::optional<unsigned> currentCpu() noexcept {
    const int cpu = sched_getcpu();
    if (cpu < 0) {
        return std::nullopt;
    }
    return static_cast<unsigned>(cpu);
}

bool pinCurrentThread(const unsigned cpu) noexcept {
    if (cpu >= static_cast<unsigned>(CPU_SETSIZE)) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) {
        return false;
    }
    sched_yield();
    return true;
}

bool unpinCurrentThread() noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    const unsigned n = logicalCpuCount();
    for (unsigned i = 0; i < n && i < static_cast<unsigned>(CPU_SETSIZE); ++i) {
        CPU_SET(i, &set);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
}

const char* raiseThreadPriority(const bool realtime) noexcept {
    if (!realtime) {
        return "unchanged (SCHED_OTHER)";
    }
    sched_param param{};
    param.sched_priority = 1;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
        return "SCHED_FIFO priority 1";
    }
    return "unchanged (SCHED_FIFO not permitted)";
}

std::vector<LogicalCpu> enumerateCpus() {
    const unsigned count = logicalCpuCount();
    std::vector<LogicalCpu> cpus(count);
    for (unsigned i = 0; i < count; ++i) {
        cpus[i].id = i;
        std::ifstream coreId("/sys/devices/system/cpu/cpu" + std::to_string(i) +
                             "/topology/core_id");
        unsigned core = 0;
        if (coreId >> core) {
            cpus[i].coreIndex = core;
        }
        if (pinCurrentThread(i)) {
            cpus[i].coreType = currentCoreType();
        }
    }
    unpinCurrentThread();
    return cpus;
}

} // namespace eigenbook::platform
