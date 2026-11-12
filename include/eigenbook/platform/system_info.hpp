#pragma once

// Operating-system facts a benchmark report needs, and memory pre-faulting.

#include <cstddef>
#include <string>

namespace eigenbook::platform {

// e.g. "Windows 10.0.26200" or "Linux 6.8.0-45-generic".
[[nodiscard]] std::string osDescription();

// Windows: active power plan name; Linux: cpufreq scaling governor of cpu0. "unknown" otherwise.
[[nodiscard]] std::string powerPolicy();

// "AC", "battery" or "unknown". On battery the OS may lower the core clock during a run.
[[nodiscard]] std::string powerSource();

// Windows: whether the process can enable SeLockMemoryPrivilege (needed for large pages).
// Linux: whether transparent huge pages are enabled ("always" or "madvise").
[[nodiscard]] bool largePagesAvailable();

// Size of one virtual-memory page in bytes.
[[nodiscard]] std::size_t pageSize() noexcept;

// Touches every page of [data, data + bytes) (read and write back one byte per page) so the
// pages are resident before a measurement starts. The contents are unchanged.
void prefault(void* data, std::size_t bytes) noexcept;

} // namespace eigenbook::platform
