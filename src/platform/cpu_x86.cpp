// CPUID-based CPU identification. Portable across MSVC, GCC and Clang on x86-64.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <eigenbook/platform/cpu_info.hpp>

#if defined(_M_X64) || defined(__x86_64__)
#define EIGENBOOK_X86_64 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define EIGENBOOK_X86_64 0
#endif

namespace eigenbook::platform {

std::string_view toString(const CoreType type) noexcept {
    switch (type) {
        case CoreType::PERFORMANCE:
            return "P-core";
        case CoreType::EFFICIENCY:
            return "E-core";
        case CoreType::UNKNOWN:
            break;
    }
    return "unknown";
}

#if EIGENBOOK_X86_64

namespace {

struct Registers {
    std::uint32_t eax{0};
    std::uint32_t ebx{0};
    std::uint32_t ecx{0};
    std::uint32_t edx{0};
};

Registers cpuid(const std::uint32_t leaf, const std::uint32_t subleaf = 0) noexcept {
    Registers r;
#if defined(_MSC_VER)
    std::array<int, 4> out{};
    __cpuidex(out.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
    r.eax = static_cast<std::uint32_t>(out[0]);
    r.ebx = static_cast<std::uint32_t>(out[1]);
    r.ecx = static_cast<std::uint32_t>(out[2]);
    r.edx = static_cast<std::uint32_t>(out[3]);
#else
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#endif
    return r;
}

constexpr bool bit(const std::uint32_t value, const unsigned n) noexcept {
    return ((value >> n) & 1U) != 0;
}

void append(std::string& out, const std::uint32_t reg) {
    std::array<char, 4> chars{};
    std::memcpy(chars.data(), &reg, chars.size());
    out.append(chars.data(), chars.size());
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) {
        s.pop_back();
    }
    const auto first = s.find_first_not_of(' ');
    return first == std::string::npos ? std::string{} : s.substr(first);
}

} // namespace

CpuInfo cpuInfo() {
    CpuInfo info;
    const Registers leaf0 = cpuid(0);
    const std::uint32_t maxLeaf = leaf0.eax;
    append(info.vendor, leaf0.ebx);
    append(info.vendor, leaf0.edx);
    append(info.vendor, leaf0.ecx);

    if (maxLeaf >= 7) {
        info.hybrid = bit(cpuid(7, 0).edx, 15);
    }

    const std::uint32_t maxExtended = cpuid(0x80000000U).eax;
    if (maxExtended >= 0x80000004U) {
        std::string brand;
        for (std::uint32_t leaf = 0x80000002U; leaf <= 0x80000004U; ++leaf) {
            const Registers r = cpuid(leaf);
            append(brand, r.eax);
            append(brand, r.ebx);
            append(brand, r.ecx);
            append(brand, r.edx);
        }
        info.brand = trim(std::move(brand));
    }
    if (maxExtended >= 0x80000001U) {
        info.rdtscp = bit(cpuid(0x80000001U).edx, 27);
    }
    if (maxExtended >= 0x80000007U) {
        info.invariantTsc = bit(cpuid(0x80000007U).edx, 8);
    }
    return info;
}

CoreType currentCoreType() noexcept {
    if (cpuid(0).eax < 0x1AU || !bit(cpuid(7, 0).edx, 15)) {
        return CoreType::UNKNOWN;
    }
    switch (cpuid(0x1AU).eax >> 24) {
        case 0x40:
            return CoreType::PERFORMANCE;
        case 0x20:
            return CoreType::EFFICIENCY;
        default:
            return CoreType::UNKNOWN;
    }
}

#else // not x86-64

CpuInfo cpuInfo() {
    return {};
}

CoreType currentCoreType() noexcept {
    return CoreType::UNKNOWN;
}

#endif

} // namespace eigenbook::platform
