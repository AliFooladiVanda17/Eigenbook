// Operating-system information on Linux.
//
// Not compiled or run in the Windows environment Phase A2 was developed in; see
// docs/BENCHMARKS.md.

#include <cstddef>
#include <fstream>
#include <string>

#include <sys/utsname.h>
#include <unistd.h>

#include <eigenbook/platform/system_info.hpp>

namespace eigenbook::platform {

namespace {

std::string firstLine(const char* path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

} // namespace

std::string osDescription() {
    utsname info{};
    if (uname(&info) != 0) {
        return "Linux (version unknown)";
    }
    return std::string(info.sysname) + " " + info.release;
}

std::string powerPolicy() {
    const std::string governor = firstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    return governor.empty() ? "unknown" : "governor " + governor;
}

std::string powerSource() {
    for (const char* path : {"/sys/class/power_supply/AC/online",
                             "/sys/class/power_supply/ACAD/online",
                             "/sys/class/power_supply/ADP1/online"}) {
        const std::string online = firstLine(path);
        if (online == "1") {
            return "AC";
        }
        if (online == "0") {
            return "battery";
        }
    }
    return "unknown"; // desktops and servers usually expose no mains supply here
}

bool largePagesAvailable() {
    const std::string thp = firstLine("/sys/kernel/mm/transparent_hugepage/enabled");
    return thp.find("[always]") != std::string::npos || thp.find("[madvise]") != std::string::npos;
}

std::size_t pageSize() noexcept {
    const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

} // namespace eigenbook::platform
