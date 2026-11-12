#pragma once

#include <cassert>
#include <cstddef>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>

#include <eigenbook/book/price_ladder.hpp>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

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
        } else {
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

static_assert(PriceLadder<MapPriceLadder<OrderSide::BUY>>);
static_assert(PriceLadder<MapPriceLadder<OrderSide::SELL>>);

} // namespace eigenbook
