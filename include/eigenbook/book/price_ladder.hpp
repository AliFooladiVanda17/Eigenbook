#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>
#include <eigenbook/util/level_bitmap.hpp>

namespace eigenbook {

// Price ordering of one side of the book: "better" is higher for bids, lower for asks.
template <OrderSide Side>
struct SideTraits;

template <>
struct SideTraits<OrderSide::BUY> {
    using Compare = std::greater<Price>;

    [[nodiscard]] static constexpr bool isBetter(const Price a, const Price b) noexcept {
        return a > b;
    }
};

template <>
struct SideTraits<OrderSide::SELL> {
    using Compare = std::less<Price>;

    [[nodiscard]] static constexpr bool isBetter(const Price a, const Price b) noexcept {
        return a < b;
    }
};

// What OrderBook needs from one side of the book. Both strategies from the README satisfy it.
template <typename L>
concept PriceLadder = requires(L ladder, const L cladder, Order& order, PriceLevel& level, Price p) {
    typename L::Config;
    { L::side } -> std::convertible_to<OrderSide>;
    { cladder.accepts(p) } noexcept -> std::same_as<bool>;
    ladder.addOrder(order);
    { ladder.removeOrder(order) } noexcept;
    { ladder.removeLevel(level) } noexcept;
    { ladder.bestLevel() } noexcept -> std::same_as<PriceLevel*>;
    { cladder.bestLevel() } noexcept -> std::same_as<const PriceLevel*>;
    { cladder.bestPrice() } noexcept -> std::same_as<std::optional<Price>>;
    { cladder.findLevel(p) } noexcept -> std::same_as<const PriceLevel*>;
    { cladder.levelCount() } noexcept -> std::same_as<std::size_t>;
    { cladder.empty() } noexcept -> std::same_as<bool>;
};

// ---------------------------------------------------------------------------------------------
// Balanced tree ladder: for sparse or unbounded price spaces.
//
// Add on a new price is O(log P); add on an existing price, best level, and removal of the best
// level are O(1) amortised. Tree nodes come from a ladder-local pool, so a level that empties and
// refills does not go back to the global allocator.
// ---------------------------------------------------------------------------------------------
struct MapLadderConfig {};

template <OrderSide Side>
class MapPriceLadder {
public:
    static constexpr OrderSide side = Side;

    using Config = MapLadderConfig;

    explicit MapPriceLadder(Config = {})
        : nodePool(std::make_unique<std::pmr::unsynchronized_pool_resource>())
        , levels(nodePool.get()) {}

    // Orders and the book hold pointers into the levels.
    MapPriceLadder(const MapPriceLadder&) = delete;
    MapPriceLadder& operator=(const MapPriceLadder&) = delete;
    MapPriceLadder(MapPriceLadder&&) = delete;
    MapPriceLadder& operator=(MapPriceLadder&&) = delete;

    ~MapPriceLadder() = default;

    [[nodiscard]] bool accepts(Price) const noexcept { return true; }

    void addOrder(Order& order) {
        const Price price = order.getPrice();
        auto [it, inserted] = levels.try_emplace(price, price);
        it->second.pushBack(order);
    }

    // Unlinks a resting order and drops its level if that leaves it empty.
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
        // The matcher always empties the best level, which is begin(): erase it without a search.
        if (!levels.empty() && &levels.begin()->second == &level) {
            levels.erase(levels.begin());
        }
        else {
            levels.erase(level.price());
        }
    }

    [[nodiscard]] PriceLevel* bestLevel() noexcept {
        return levels.empty() ? nullptr : &levels.begin()->second;
    }

    [[nodiscard]] const PriceLevel* bestLevel() const noexcept {
        return levels.empty() ? nullptr : &levels.begin()->second;
    }

    [[nodiscard]] std::optional<Price> bestPrice() const noexcept {
        if (levels.empty()) {
            return std::nullopt;
        }
        return levels.begin()->first;
    }

    [[nodiscard]] const PriceLevel* findLevel(const Price price) const noexcept {
        const auto it = levels.find(price);
        return it == levels.end() ? nullptr : &it->second;
    }

    // Visits non-empty levels from best to worst: cb(const PriceLevel&).
    template <typename Callback>
    void forEachLevel(Callback&& cb) const {
        for (const auto& [price, level] : levels) {
            cb(level);
        }
    }

    [[nodiscard]] std::size_t levelCount() const noexcept { return levels.size(); }

    [[nodiscard]] bool empty() const noexcept { return levels.empty(); }

private:
    using Compare = typename SideTraits<Side>::Compare;

    // Heap-allocated so its address survives for the map's allocator.
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> nodePool;
    std::pmr::map<Price, PriceLevel, Compare> levels;
};

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
        // Unsigned wrap folds the "below minPrice" check into the upper-bound check.
        return static_cast<std::uint64_t>(price - minPrice) < tickCount;
    }

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
            }
            else {
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
        }
        else {
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
    [[nodiscard]] std::size_t indexOf(const Price price) const noexcept {
        return static_cast<std::size_t>(price - minPrice);
    }

    [[nodiscard]] static constexpr bool isBetterIndex(const std::size_t a,
                                                      const std::size_t b) noexcept {
        if constexpr (Side == OrderSide::BUY) {
            return a > b;
        }
        else {
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

static_assert(PriceLadder<MapPriceLadder<OrderSide::BUY>>);
static_assert(PriceLadder<MapPriceLadder<OrderSide::SELL>>);
static_assert(PriceLadder<ArrayPriceLadder<OrderSide::BUY>>);
static_assert(PriceLadder<ArrayPriceLadder<OrderSide::SELL>>);

} // namespace eigenbook
