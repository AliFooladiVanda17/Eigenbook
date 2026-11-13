#pragma once

// The book types the benchmarks compare, and how each is sized for a workload. Shared by the
// latency harness and the zero-allocation test, so both build exactly the same books.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

#include <workloads/workload.hpp>

#include <eigenbook/book/order_book.hpp>

namespace eigenbook::bench {

// Level bound for the bounded ladders: the workload's peak per side + 25 % + 64.
[[nodiscard]] inline std::size_t maxLevelsFor(const Workload& w) noexcept {
    return w.peakLevelsPerSide + w.peakLevelsPerSide / 4 + 64;
}

// Settings the harness may change from the command line (cold path; read when a book is built).
struct BookOptions {
    std::size_t hybridWindowTicks{4096};
};

inline BookOptions& bookOptions() noexcept {
    static BookOptions options;
    return options;
}

// A hybrid window (bookOptions().hybridWindowTicks) centred on the workload's first price.
[[nodiscard]] inline HybridLadderConfig hybridConfigFor(const Workload& w) noexcept {
    const std::size_t window = bookOptions().hybridWindowTicks;
    const std::int64_t centre = w.referencePrice.value;
    const std::int64_t base = centre < std::numeric_limits<std::int64_t>::min() +
                                           static_cast<std::int64_t>(window / 2)
                                  ? centre
                                  : centre - static_cast<std::int64_t>(window / 2);
    return {.windowTicks = window,
            .initialBase = {base},
            .maxLevels = maxLevelsFor(w),
            .maxShiftsPerOp = 64,
            .maxMovesPerOp = 8};
}

template <typename Book>
struct BookTraits;

template <>
struct BookTraits<OrderBook> {
    static constexpr std::string_view name = "map";

    static std::unique_ptr<OrderBook> make(const Workload& w) {
        return std::make_unique<OrderBook>(InstrumentId{1}, MapLadderConfig{}, w.expectedOrders);
    }
};

template <>
struct BookTraits<OrderBookM1> {
    static constexpr std::string_view name = "map-m1";

    static std::unique_ptr<OrderBookM1> make(const Workload& w) {
        return std::make_unique<OrderBookM1>(InstrumentId{1},
                                             MapLadderConfig{.maxLevels = maxLevelsFor(w)},
                                             w.expectedOrders);
    }
};

template <>
struct BookTraits<OrderBookM2> {
    static constexpr std::string_view name = "map-m2";

    static std::unique_ptr<OrderBookM2> make(const Workload& w) {
        return std::make_unique<OrderBookM2>(InstrumentId{1},
                                             MapLadderConfig{.maxLevels = maxLevelsFor(w)},
                                             w.expectedOrders);
    }
};

template <>
struct BookTraits<DenseOrderBook> {
    static constexpr std::string_view name = "dense";

    static std::unique_ptr<DenseOrderBook> make(const Workload& w) {
        return std::make_unique<DenseOrderBook>(
            InstrumentId{1},
            ArrayLadderConfig{.minPrice = w.minPrice, .tickCount = w.tickCount()},
            w.expectedOrders);
    }
};

template <>
struct BookTraits<HybridOrderBook> {
    static constexpr std::string_view name = "hybrid";

    static std::unique_ptr<HybridOrderBook> make(const Workload& w) {
        return std::make_unique<HybridOrderBook>(InstrumentId{1},
                                                 hybridConfigFor(w),
                                                 w.expectedOrders);
    }
};

// Calls f.template operator()<Book>() for every book type, in a fixed order.
template <typename F>
void forEachBookType(F&& f) {
    f.template operator()<OrderBook>();
    f.template operator()<OrderBookM1>();
    f.template operator()<OrderBookM2>();
    f.template operator()<DenseOrderBook>();
    f.template operator()<HybridOrderBook>();
}

} // namespace eigenbook::bench
