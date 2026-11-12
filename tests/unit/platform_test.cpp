//
// Eigenbook::platform: CPU identification, TSC calibration, pinning, CPU choice, pre-faulting.
//
// These tests run on whatever machine executes them, so they check properties that hold on any
// x86-64 machine (and are skipped where a feature is missing), not this developer's CPU.
//

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <eigenbook/platform/affinity.hpp>
#include <eigenbook/platform/cpu_info.hpp>
#include <eigenbook/platform/system_info.hpp>
#include <eigenbook/platform/tsc.hpp>

namespace eigenbook::platform {

TEST(Platform, CpuInfoIsPopulated) {
    if (!kHasTsc) {
        GTEST_SKIP() << "not x86-64";
    }
    const CpuInfo info = cpuInfo();
    EXPECT_EQ(info.vendor.size(), 12u);
    EXPECT_FALSE(info.brand.empty());
}

#if EIGENBOOK_PLATFORM_HAS_TSC
TEST(Platform, TscCalibrationIsPlausibleAndStable) {
    if (!cpuInfo().invariantTsc) {
        GTEST_SKIP() << "no invariant TSC";
    }
    const TscCalibration a = calibrateTsc(std::chrono::milliseconds(200));
    const TscCalibration b = calibrateTsc(std::chrono::milliseconds(200));
    EXPECT_GT(a.ticksPerSecond, 0.5e9);
    EXPECT_LT(a.ticksPerSecond, 10e9);
    EXPECT_GE(a.duration, std::chrono::milliseconds(200));
    EXPECT_LT(std::abs(a.ticksPerSecond - b.ticksPerSecond) / a.ticksPerSecond, 0.01);
}

TEST(Platform, TscIsMonotonicOnOneCpu) {
    ASSERT_TRUE(pinCurrentThread(0));
    std::uint64_t last = tscStart();
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t now = tscStop();
        ASSERT_GE(now, last);
        last = now;
    }
    unpinCurrentThread();
}
#endif

TEST(Platform, PinningMovesTheThreadThere) {
    const unsigned n = logicalCpuCount();
    ASSERT_GE(n, 1u);
    for (const unsigned cpu : {0U, n - 1, n / 2}) {
        ASSERT_TRUE(pinCurrentThread(cpu)) << cpu;
        EXPECT_EQ(currentCpu(), cpu);
    }
    EXPECT_FALSE(pinCurrentThread(n + 64));
    EXPECT_TRUE(unpinCurrentThread());
}

TEST(Platform, EnumerationCoversEveryLogicalCpu) {
    const std::vector<LogicalCpu> cpus = enumerateCpus();
    ASSERT_EQ(cpus.size(), logicalCpuCount());
    for (unsigned i = 0; i < cpus.size(); ++i) {
        EXPECT_EQ(cpus[i].id, i);
    }
    if (cpuInfo().hybrid) {
        // Every core reports its type, both types exist, and where Windows reports efficiency
        // classes, every P-core is in a higher class than every E-core.
        unsigned p = 0;
        unsigned e = 0;
        for (const LogicalCpu& c : cpus) {
            ASSERT_NE(c.coreType, CoreType::UNKNOWN) << "CPU " << c.id;
            (c.coreType == CoreType::PERFORMANCE ? p : e) += 1;
        }
        EXPECT_GT(p, 0u);
        EXPECT_GT(e, 0u);
        for (const LogicalCpu& a : cpus) {
            for (const LogicalCpu& b : cpus) {
                if (a.efficiencyClass && b.efficiencyClass && a.coreType == CoreType::PERFORMANCE &&
                    b.coreType == CoreType::EFFICIENCY) {
                    EXPECT_GT(*a.efficiencyClass, *b.efficiencyClass) << a.id << " vs " << b.id;
                }
            }
        }
    }
}

TEST(Platform, PickBenchmarkCpuPrefersAPCoreAwayFromCpuZero) {
    auto cpu = [](unsigned id, CoreType t, unsigned core, std::uint8_t cls) {
        return LogicalCpu{.id = id, .coreType = t, .coreIndex = core, .efficiencyClass = cls};
    };
    using enum CoreType;
    // CPUs 0,1 = P-core 0 (hyper-threads), 2,3 = P-core 1, 4,5 = E-cores.
    const std::vector<LogicalCpu> hybrid{cpu(0, PERFORMANCE, 0, 1),
                                         cpu(1, PERFORMANCE, 0, 1),
                                         cpu(2, PERFORMANCE, 1, 1),
                                         cpu(3, PERFORMANCE, 1, 1),
                                         cpu(4, EFFICIENCY, 2, 0),
                                         cpu(5, EFFICIENCY, 3, 0)};
    EXPECT_EQ(pickBenchmarkCpu(hybrid), 2u);

    // No CPUID core type: fall back to the highest efficiency class.
    const std::vector<LogicalCpu> classesOnly{cpu(0, UNKNOWN, 0, 0),
                                              cpu(1, UNKNOWN, 1, 0),
                                              cpu(2, UNKNOWN, 2, 1),
                                              cpu(3, UNKNOWN, 3, 1)};
    EXPECT_EQ(pickBenchmarkCpu(classesOnly), 2u);

    // Uniform machine: anything but CPU 0 and its sibling.
    const std::vector<LogicalCpu> uniform{cpu(0, UNKNOWN, 0, 0),
                                          cpu(1, UNKNOWN, 0, 0),
                                          cpu(2, UNKNOWN, 1, 0)};
    EXPECT_EQ(pickBenchmarkCpu(uniform), 2u);

    EXPECT_EQ(pickBenchmarkCpu({cpu(0, UNKNOWN, 0, 0)}), 0u);
    EXPECT_FALSE(pickBenchmarkCpu({}).has_value());
}

TEST(Platform, PrefaultLeavesContentsUnchanged) {
    std::vector<unsigned char> buffer(3 * pageSize() + 17);
    std::iota(buffer.begin(), buffer.end(), static_cast<unsigned char>(0));
    const std::vector<unsigned char> copy = buffer;
    prefault(buffer.data(), buffer.size());
    EXPECT_EQ(buffer, copy);
    prefault(nullptr, 0);
}

TEST(Platform, SystemFactsAreReported) {
    EXPECT_FALSE(osDescription().empty());
    EXPECT_FALSE(powerPolicy().empty());
    const std::string source = powerSource();
    EXPECT_TRUE(source == "AC" || source == "battery" || source == "unknown") << source;
    EXPECT_GE(pageSize(), 4096u);
    (void)largePagesAvailable();
}

} // namespace eigenbook::platform
