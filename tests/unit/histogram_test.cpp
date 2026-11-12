//
// LatencyHistogram: bucket mapping, percentile accuracy against an exact reference, merge.
//

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <harness/histogram.hpp>

namespace eigenbook::bench {

namespace {

// Exact percentile with the same rank rule as LatencyHistogram (smallest value with at least
// p % of the samples at or below it).
std::uint64_t exactPercentile(const std::vector<std::uint64_t>& sorted, const double p) {
    if (p <= 0.0) {
        return sorted.front();
    }
    const auto rank =
        static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

} // namespace

TEST(LatencyHistogram, BucketsCoverEveryValueWithinOneThousandth) {
    std::mt19937_64 rng(11);
    std::size_t previous = 0;
    for (std::uint64_t v = 0; v < 100'000; ++v) {
        const std::size_t i = LatencyHistogram::indexOf(v);
        ASSERT_GE(i, previous) << v; // monotonic
        previous = i;
        ASSERT_LE(LatencyHistogram::lowestEquivalent(i), v);
        ASSERT_GE(LatencyHistogram::highestEquivalent(i), v);
    }
    for (int k = 0; k < 200'000; ++k) {
        const unsigned bits = 1 + static_cast<unsigned>(rng() % 39);
        const std::uint64_t v = rng() >> (64 - bits);
        const std::size_t i = LatencyHistogram::indexOf(v);
        const std::uint64_t lo = LatencyHistogram::lowestEquivalent(i);
        const std::uint64_t hi = LatencyHistogram::highestEquivalent(i);
        ASSERT_LE(lo, v);
        ASSERT_GE(hi, v);
        if (v < LatencyHistogram::kLinearLimit) {
            ASSERT_EQ(lo, v);
            ASSERT_EQ(hi, v);
        } else {
            ASSERT_LE(hi - lo + 1, lo / 1024) << v; // relative width <= 2^-10
        }
    }
    EXPECT_LT(LatencyHistogram::indexOf((std::uint64_t{1} << 40) - 1),
              LatencyHistogram::kBucketCount);
}

TEST(LatencyHistogram, PercentilesMatchSortedReferenceToThreeSignificantDigits) {
    std::mt19937_64 rng(7);
    LatencyHistogram h;
    std::vector<std::uint64_t> values;
    for (int k = 0; k < 300'000; ++k) {
        // Log-uniform over [1, 2^36): every range of magnitudes is exercised.
        const double e = std::uniform_real_distribution<double>(0.0, 36.0)(rng);
        const auto v = static_cast<std::uint64_t>(std::exp2(e));
        values.push_back(v);
        h.record(v);
    }
    std::ranges::sort(values);
    for (const double p : {0.0, 1.0, 25.0, 50.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0}) {
        const std::uint64_t exact = exactPercentile(values, p);
        const std::uint64_t got = h.valueAtPercentile(p);
        EXPECT_GE(got, exact) << "p" << p;
        EXPECT_LE(got, exact + exact / 1024) << "p" << p;
    }
}

TEST(LatencyHistogram, CountMinMaxAndMeanAreExact) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.valueAtPercentile(50.0), 0u);
    const std::vector<std::uint64_t> v{5, 1'000'003, 77, 123'456'789, 2'048};
    std::uint64_t sum = 0;
    for (const std::uint64_t x : v) {
        h.record(x);
        sum += x;
    }
    EXPECT_EQ(h.count(), v.size());
    EXPECT_EQ(h.min(), 5u);
    EXPECT_EQ(h.max(), 123'456'789u);
    EXPECT_DOUBLE_EQ(h.mean(), static_cast<double>(sum) / static_cast<double>(v.size()));
    EXPECT_EQ(h.valueAtPercentile(100.0), 123'456'789u); // clamped to the exact maximum
    EXPECT_EQ(h.valueAtPercentile(0.0), 5u);
}

TEST(LatencyHistogram, HugeValuesGoToTheLastBucketButKeepTheExactMax) {
    LatencyHistogram h;
    const std::uint64_t huge = std::uint64_t{1} << 50;
    h.record(huge);
    EXPECT_EQ(h.max(), huge);
    EXPECT_EQ(h.valueAtPercentile(100.0), huge);
}

TEST(LatencyHistogram, MergeEqualsRecordingIntoOne) {
    std::mt19937_64 rng(3);
    LatencyHistogram a;
    LatencyHistogram b;
    LatencyHistogram both;
    for (int k = 0; k < 50'000; ++k) {
        const std::uint64_t v = rng() % 1'000'000;
        (k % 3 == 0 ? a : b).record(v);
        both.record(v);
    }
    a.merge(b);
    EXPECT_EQ(a.count(), both.count());
    EXPECT_EQ(a.min(), both.min());
    EXPECT_EQ(a.max(), both.max());
    for (const double p : {50.0, 99.0, 99.99}) {
        EXPECT_EQ(a.valueAtPercentile(p), both.valueAtPercentile(p));
    }
    LatencyHistogram copy = a;
    a.reset();
    EXPECT_EQ(a.count(), 0u);
    EXPECT_EQ(copy.count(), both.count());
}

} // namespace eigenbook::bench
