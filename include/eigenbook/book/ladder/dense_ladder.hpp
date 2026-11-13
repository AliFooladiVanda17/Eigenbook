#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <eigenbook/book/price_ladder.hpp>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>
#include <eigenbook/util/level_bitmap.hpp>

namespace eigenbook {

// ---------------------------------------------------------------------------------------------
// Fixed array + bitset ladder: for dense, bounded tick ranges.
//
// Every tick in [minPrice, minPrice + tickCount) has a preallocated level, so lookups are a
// subtraction plus an index. A two-level occupancy bitmap gives the next best level when the best
// one empties, and the best index is cached, so bestLevel() is O(1).
// ---------------------------------------------------------------------------------------------
// Both sides of a dense book cover the same tick range.
struct ArrayLadderConfig {
    Price minPrice{0};
    std::size_t tickCount{std::size_t{1} << 16};
};

template <OrderSide Side>
class ArrayPriceLadder {
public:
    static constexpr OrderSide side = Side;

    using Config = ArrayLadderConfig;

    explicit ArrayPriceLadder(const Config config = {})
        : minPrice(config.minPrice)
        , tickCount(config.tickCount)
        , levels(std::make_unique<PriceLevel[]>(config.tickCount))
        , occupied(config.tickCount) {
        assert(tickCount > 0);
        for (std::size_t i = 0; i < tickCount; ++i) {
            levels[i].levelPrice = minPrice + static_cast<std::int64_t>(i);
        }
    }

    ArrayPriceLadder(const ArrayPriceLadder&) = delete;
    ArrayPriceLadder& operator=(const ArrayPriceLadder&) = delete;
    ArrayPriceLadder(ArrayPriceLadder&&) = delete;
    ArrayPriceLadder& operator=(ArrayPriceLadder&&) = delete;

    ~ArrayPriceLadder() = default;

    [[nodiscard]] bool accepts(const Price price) const noexcept {
        // Offset in unsigned (modular) arithmetic: defined for every int64 price, and a price below
        // minPrice wraps to a huge offset, so one comparison checks both ends of the range.
        return offsetOf(price) < tickCount;
    }

    // Every tick in range has a preallocated level, so capacity is the range itself.
    [[nodiscard]] bool canRestAt(const Price price) const noexcept { return accepts(price); }

    // Precondition: accepts(order.getPrice()).
    void addOrder(Order& order) noexcept {
        assert(accepts(order.getPrice()));
        const std::size_t idx = indexOf(order.getPrice());
        PriceLevel& level = levels[idx];
        if (level.empty()) {
            occupied.set(idx);
            ++activeLevels;
            if (best == LevelBitmap::npos || isBetterIndex(idx, best)) {
                best = idx;
            }
        }
        level.pushBack(order);
    }

    void removeOrder(Order& order) noexcept {
        PriceLevel* level = PriceLevel::ownerOf(order);
        assert(level != nullptr);
        level->erase(order);
        if (level->empty()) {
            removeLevel(*level);
        }
    }

    // Precondition: the level is empty.
    void removeLevel(PriceLevel& level) noexcept {
        assert(level.empty());
        const auto idx = static_cast<std::size_t>(&level - levels.get());
        occupied.reset(idx);
        --activeLevels;
        if (idx == best) {
            // Bids get worse downwards, asks upwards; search from the old best in that direction.
            if constexpr (Side == OrderSide::BUY) {
                best = idx == 0 ? LevelBitmap::npos : occupied.findPrev(idx - 1);
            } else {
                best = occupied.findNext(idx + 1);
            }
        }
    }

    [[nodiscard]] PriceLevel* bestLevel() noexcept {
        return best == LevelBitmap::npos ? nullptr : &levels[best];
    }

    [[nodiscard]] const PriceLevel* bestLevel() const noexcept {
        return best == LevelBitmap::npos ? nullptr : &levels[best];
    }

    [[nodiscard]] std::optional<Price> bestPrice() const noexcept {
        if (best == LevelBitmap::npos) {
            return std::nullopt;
        }
        return levels[best].price();
    }

    [[nodiscard]] const PriceLevel* findLevel(const Price price) const noexcept {
        if (!accepts(price)) {
            return nullptr;
        }
        const PriceLevel& level = levels[indexOf(price)];
        return level.empty() ? nullptr : &level;
    }

    // Visits non-empty levels from best to worst: cb(const PriceLevel&).
    template <typename Callback>
    void forEachLevel(Callback&& cb) const {
        if constexpr (Side == OrderSide::BUY) {
            for (std::size_t i = best; i != LevelBitmap::npos;
                 i = i == 0 ? LevelBitmap::npos : occupied.findPrev(i - 1)) {
                cb(static_cast<const PriceLevel&>(levels[i]));
            }
        } else {
            for (std::size_t i = best; i != LevelBitmap::npos; i = occupied.findNext(i + 1)) {
                cb(static_cast<const PriceLevel&>(levels[i]));
            }
        }
    }

    [[nodiscard]] std::size_t levelCount() const noexcept { return activeLevels; }

    [[nodiscard]] bool empty() const noexcept { return activeLevels == 0; }

    [[nodiscard]] Price lowestPrice() const noexcept { return minPrice; }

    [[nodiscard]] Price highestPrice() const noexcept {
        return minPrice + static_cast<std::int64_t>(tickCount - 1);
    }

private:
    [[nodiscard]] std::uint64_t offsetOf(const Price price) const noexcept {
        return static_cast<std::uint64_t>(price.value) - static_cast<std::uint64_t>(minPrice.value);
    }

    [[nodiscard]] std::size_t indexOf(const Price price) const noexcept {
        return static_cast<std::size_t>(offsetOf(price));
    }

    [[nodiscard]] static constexpr bool isBetterIndex(const std::size_t a,
                                                      const std::size_t b) noexcept {
        if constexpr (Side == OrderSide::BUY) {
            return a > b;
        } else {
            return a < b;
        }
    }

    Price minPrice;
    std::size_t tickCount;
    std::unique_ptr<PriceLevel[]> levels;
    LevelBitmap occupied;
    std::size_t best{LevelBitmap::npos};
    std::size_t activeLevels{0};
};

static_assert(PriceLadder<ArrayPriceLadder<OrderSide::BUY>>);
static_assert(PriceLadder<ArrayPriceLadder<OrderSide::SELL>>);

} // namespace eigenbook
