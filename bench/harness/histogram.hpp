#pragma once

// Log-linear latency histogram (HdrHistogram-style), 3 significant digits.
//
// Values below 2048 are counted exactly. Above that, each power-of-two range [2^k, 2^(k+1)) is
// split into 1024 equal sub-buckets, so a bucket is at most 1/1024 (< 0.1 %) of its value wide.
// Values up to 2^40 - 1 are resolved; larger ones go to the last bucket (the exact maximum is
// kept separately). All storage is allocated by the constructor; record() never allocates.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace eigenbook::bench {

class LatencyHistogram {
public:
    static constexpr unsigned kSubBucketBits = 10; // 1024 per octave
    static constexpr std::uint64_t kLinearLimit = std::uint64_t{2} << kSubBucketBits; // 2048
    static constexpr unsigned kMaxValueBits = 40;
    static constexpr std::size_t kBucketCount = static_cast<std::size_t>(kLinearLimit) +
                                                (kMaxValueBits - kSubBucketBits - 1) *
                                                    (std::size_t{1} << kSubBucketBits);

    LatencyHistogram() : counts(std::make_unique<std::uint64_t[]>(kBucketCount)) {}

    LatencyHistogram(const LatencyHistogram& other)
        : counts(std::make_unique<std::uint64_t[]>(kBucketCount))
        , total(other.total)
        , sum(other.sum)
        , minValue(other.minValue)
        , maxValue(other.maxValue) {
        std::copy_n(other.counts.get(), kBucketCount, counts.get());
    }

    LatencyHistogram& operator=(const LatencyHistogram& other) {
        if (this != &other) {
            std::copy_n(other.counts.get(), kBucketCount, counts.get());
            total = other.total;
            sum = other.sum;
            minValue = other.minValue;
            maxValue = other.maxValue;
        }
        return *this;
    }

    LatencyHistogram(LatencyHistogram&&) noexcept = default;
    LatencyHistogram& operator=(LatencyHistogram&&) noexcept = default;
    ~LatencyHistogram() = default;

    void record(const std::uint64_t value) noexcept {
        ++counts[indexOf(value)];
        ++total;
        sum += value;
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }

    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            counts[i] += other.counts[i];
        }
        total += other.total;
        sum += other.sum;
        minValue = std::min(minValue, other.minValue);
        maxValue = std::max(maxValue, other.maxValue);
    }

    void reset() noexcept {
        std::fill_n(counts.get(), kBucketCount, std::uint64_t{0});
        total = 0;
        sum = 0;
        minValue = std::numeric_limits<std::uint64_t>::max();
        maxValue = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total; }

    [[nodiscard]] std::uint64_t min() const noexcept { return total == 0 ? 0 : minValue; }

    [[nodiscard]] std::uint64_t max() const noexcept { return maxValue; }

    // Exact mean of the recorded values. The sum is a 64-bit integer; it would overflow only after
    // more than 2^24 values of 2^40 ticks each, far beyond any benchmark run.
    [[nodiscard]] double mean() const noexcept {
        return total == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(total);
    }

    // Smallest bucket upper bound such that at least `percentile` % of the values are <= it,
    // clamped to the exact maximum. percentile in [0, 100]; 0 returns the exact minimum.
    [[nodiscard]] std::uint64_t valueAtPercentile(const double percentile) const noexcept {
        if (total == 0) {
            return 0;
        }
        if (percentile <= 0.0) {
            return minValue;
        }
        const double clamped = std::min(percentile, 100.0);
        auto rank =
            static_cast<std::uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(total)));
        rank = std::clamp<std::uint64_t>(rank, 1, total);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            seen += counts[i];
            if (seen >= rank) {
                // The last bucket also holds every value beyond the resolved range.
                return i == kBucketCount - 1 ? maxValue : std::min(highestEquivalent(i), maxValue);
            }
        }
        return maxValue;
    }

    // Visits every non-empty bucket in increasing order: cb(lowest, highest, count).
    template <typename Callback>
    void forEachBucket(Callback&& cb) const {
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            if (counts[i] != 0) {
                cb(lowestEquivalent(i), highestEquivalent(i), counts[i]);
            }
        }
    }

    [[nodiscard]] static constexpr std::size_t indexOf(const std::uint64_t value) noexcept {
        if (value < kLinearLimit) {
            return static_cast<std::size_t>(value);
        }
        const auto width = static_cast<unsigned>(std::bit_width(value)); // >= 12
        if (width > kMaxValueBits) {
            return kBucketCount - 1;
        }
        const unsigned shift = width - kSubBucketBits - 1;
        const std::uint64_t sub = value >> shift; // in [1024, 2047]
        return static_cast<std::size_t>(kLinearLimit) +
               static_cast<std::size_t>(width - kSubBucketBits - 2) *
                   (std::size_t{1} << kSubBucketBits) +
               static_cast<std::size_t>(sub - (std::uint64_t{1} << kSubBucketBits));
    }

    [[nodiscard]] static constexpr std::uint64_t
    lowestEquivalent(const std::size_t index) noexcept {
        if (index < kLinearLimit) {
            return index;
        }
        const std::size_t rel = index - static_cast<std::size_t>(kLinearLimit);
        const unsigned shift = static_cast<unsigned>(rel >> kSubBucketBits) + 1;
        const std::uint64_t sub = (std::uint64_t{1} << kSubBucketBits) +
                                  (rel & ((std::size_t{1} << kSubBucketBits) - 1));
        return sub << shift;
    }

    [[nodiscard]] static constexpr std::uint64_t
    highestEquivalent(const std::size_t index) noexcept {
        if (index < kLinearLimit) {
            return index;
        }
        const std::size_t rel = index - static_cast<std::size_t>(kLinearLimit);
        const unsigned shift = static_cast<unsigned>(rel >> kSubBucketBits) + 1;
        return lowestEquivalent(index) + (std::uint64_t{1} << shift) - 1;
    }

private:
    std::unique_ptr<std::uint64_t[]> counts;
    std::uint64_t total{0};
    std::uint64_t sum{0};
    std::uint64_t minValue{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t maxValue{0};
};

} // namespace eigenbook::bench
