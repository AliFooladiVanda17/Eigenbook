#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <eigenbook/book/ladder/map_ladder.hpp>
#include <eigenbook/book/price_ladder.hpp>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>
#include <eigenbook/util/fixed_block_resource.hpp>
#include <eigenbook/util/level_bitmap.hpp>

namespace eigenbook {

// ---------------------------------------------------------------------------------------------
// Hybrid ladder (H1): a dense window around the touch, ordered maps for everything else.
//
// The window is a ring of `windowTicks` preallocated levels covering [base, base + windowTicks),
// indexed by price & (windowTicks - 1), with a two-level occupancy bitmap; inside it everything
// costs what the dense ladder costs. Prices above or below the window live in two ordered maps
// whose nodes come from one preallocated FixedBlockResource, so nothing allocates after
// construction and any price is accepted.
//
// Recentering is incremental: after each add, if the touch has left the middle half of the
// window, the window slides towards it one tick at a time, at most `maxShiftsPerOp` ticks and at
// most `maxMovesPerOp` level moves per add (a tick evicts at most one level to a map and imports
// at most one from the other). Moving a level re-points the level pointer of each of its orders,
// so a move costs O(orders at that level). After a large price jump the touch is served from a
// map until the window has caught up; results are identical either way.
//
// maxLevels bounds the total number of levels (window + maps); beyond it canRestAt() is false.
// ---------------------------------------------------------------------------------------------
struct HybridLadderConfig {
    std::size_t windowTicks{4096}; // power of two
    Price initialBase{0};          // first window: [initialBase, initialBase + windowTicks)
    std::size_t maxLevels{1 << 16};
    std::size_t maxShiftsPerOp{64};
    std::size_t maxMovesPerOp{8}; // >= 2
};

template <OrderSide Side>
class HybridPriceLadder {
public:
    static constexpr OrderSide side = Side;

    using Config = HybridLadderConfig;

    explicit HybridPriceLadder(const Config config = {})
        : width(checkedWidth(config))
        , mask(width - 1)
        , maxLevels(config.maxLevels)
        , maxShifts(config.maxShiftsPerOp)
        , maxMoves(config.maxMovesPerOp)
        , base(config.initialBase)
        , window(std::make_unique<PriceLevel[]>(width))
        , occupied(width)
        , overflowNodes(detail::levelMapNodeLayout<Side>().bytes, config.maxLevels + 8)
        , above(&overflowNodes)
        , below(&overflowNodes) {
        for (std::size_t offset = 0; offset < width; ++offset) {
            const Price p = priceAt(offset);
            window[slotOf(p)].levelPrice = p;
        }
    }

    HybridPriceLadder(const HybridPriceLadder&) = delete;
    HybridPriceLadder& operator=(const HybridPriceLadder&) = delete;
    HybridPriceLadder(HybridPriceLadder&&) = delete;
    HybridPriceLadder& operator=(HybridPriceLadder&&) = delete;

    ~HybridPriceLadder() = default;

    [[nodiscard]] bool accepts(Price) const noexcept { return true; }

    [[nodiscard]] bool canRestAt(const Price price) const noexcept {
        return levelCount() < maxLevels || findLevel(price) != nullptr;
    }

    // Precondition: canRestAt(order.getPrice()).
    void addOrder(Order& order) {
        assert(canRestAt(order.getPrice()));
        const Price price = order.getPrice();
        if (inWindow(price)) {
            PriceLevel& level = window[slotOf(price)];
            if (level.empty()) {
                markOccupied(price);
            }
            level.pushBack(order);
        } else {
            auto& map = price > top() ? above : below;
            map.try_emplace(price, price).first->second.pushBack(order);
        }
        recenter();
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
        if (isWindowLevel(level)) {
            markEmpty(level.price());
            return;
        }
        auto& map = level.price() > top() ? above : below;
        if (&map.begin()->second == &level) {
            map.erase(map.begin());
        } else {
            map.erase(level.price());
        }
    }

    [[nodiscard]] PriceLevel* bestLevel() noexcept {
        return const_cast<PriceLevel*>(std::as_const(*this).bestLevel());
    }

    [[nodiscard]] const PriceLevel* bestLevel() const noexcept {
        if (!betterMap().empty()) {
            return &betterMap().begin()->second;
        }
        if (windowActive != 0) {
            return &window[slotOf(windowBest)];
        }
        if (!worseMap().empty()) {
            return &worseMap().begin()->second;
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<Price> bestPrice() const noexcept {
        const PriceLevel* level = bestLevel();
        return level == nullptr ? std::nullopt : std::optional<Price>(level->price());
    }

    [[nodiscard]] const PriceLevel* findLevel(const Price price) const noexcept {
        if (inWindow(price)) {
            const PriceLevel& level = window[slotOf(price)];
            return level.empty() ? nullptr : &level;
        }
        const auto& map = price > top() ? above : below;
        const auto it = map.find(price);
        return it == map.end() ? nullptr : &it->second;
    }

    // Visits non-empty levels from best to worst: cb(const PriceLevel&).
    template <typename Callback>
    void forEachLevel(Callback&& cb) const {
        forEachLevelWhile([&](const PriceLevel& level) {
            cb(level);
            return true;
        });
    }

    // Like forEachLevel, but stops as soon as cb(const PriceLevel&) returns false. Read-only.
    template <typename Callback>
    void forEachLevelWhile(Callback&& cb) const {
        for (const auto& [price, level] : betterMap()) {
            if (!cb(level)) {
                return;
            }
        }
        if (windowActive != 0) {
            for (std::optional<Price> p = windowBest; p; p = nextWorseInWindow(*p)) {
                if (!cb(static_cast<const PriceLevel&>(window[slotOf(*p)]))) {
                    return;
                }
            }
        }
        for (const auto& [price, level] : worseMap()) {
            if (!cb(level)) {
                return;
            }
        }
    }

    [[nodiscard]] std::size_t levelCount() const noexcept {
        return windowActive + above.size() + below.size();
    }

    [[nodiscard]] bool empty() const noexcept { return levelCount() == 0; }

    // --- Introspection (tests and benchmarks) ------------------------------------------------

    [[nodiscard]] Price windowBase() const noexcept { return base; }

    [[nodiscard]] std::size_t windowTicks() const noexcept { return width; }

    [[nodiscard]] std::size_t windowLevels() const noexcept { return windowActive; }

    [[nodiscard]] std::size_t overflowLevels() const noexcept {
        return above.size() + below.size();
    }

    [[nodiscard]] std::uint64_t levelsMoved() const noexcept { return moves; }

    [[nodiscard]] const FixedBlockResource& overflowResource() const noexcept {
        return overflowNodes;
    }

    // Checks every structural invariant. O(window + levels + orders): tests only.
    [[nodiscard]] bool checkInvariants() const noexcept {
        std::size_t active = 0;
        std::optional<Price> best;
        for (std::size_t offset = 0; offset < width; ++offset) {
            const Price p = priceAt(offset);
            const PriceLevel& level = window[slotOf(p)];
            if (level.price() != p || occupied.test(slotOf(p)) == level.empty() ||
                !queueConsistent(level)) {
                return false;
            }
            if (!level.empty()) {
                ++active;
                if (!best || SideTraits<Side>::isBetter(p, *best)) {
                    best = p;
                }
            }
        }
        if (active != windowActive || (active != 0 && best != windowBest)) {
            return false;
        }
        for (const auto& [p, level] : above) {
            if (!(p > top()) || level.empty() || level.price() != p || !queueConsistent(level)) {
                return false;
            }
        }
        for (const auto& [p, level] : below) {
            if (!(p < base) || level.empty() || level.price() != p || !queueConsistent(level)) {
                return false;
            }
        }
        return levelCount() <= maxLevels;
    }

private:
    using LevelMap = detail::LevelMap<Side>;

    static std::size_t checkedWidth(const Config& c) {
        if (c.windowTicks < 64 || (c.windowTicks & (c.windowTicks - 1)) != 0) {
            throw std::invalid_argument("HybridLadderConfig::windowTicks must be a power of two >= "
                                        "64");
        }
        if (c.maxMovesPerOp < 2 || c.maxLevels == 0) {
            throw std::invalid_argument("HybridLadderConfig: maxMovesPerOp >= 2 and maxLevels > 0 "
                                        "required");
        }
        if (clampBase(c.initialBase.value, c.windowTicks) != c.initialBase) {
            throw std::invalid_argument("HybridLadderConfig: the window must fit below INT64_MAX");
        }
        return c.windowTicks;
    }

    // Largest base whose window [base, base + width) still ends at or below INT64_MAX, so the
    // window never wraps around the int64 range.
    [[nodiscard]] static Price clampBase(const std::int64_t wanted, const std::size_t w) noexcept {
        const std::int64_t highest = std::numeric_limits<std::int64_t>::max() -
                                     static_cast<std::int64_t>(w - 1);
        return Price{wanted > highest ? highest : wanted};
    }

    // --- Window geometry. Unsigned arithmetic: defined for every int64 price. -----------------

    [[nodiscard]] static std::uint64_t raw(const Price p) noexcept {
        return static_cast<std::uint64_t>(p.value);
    }

    [[nodiscard]] std::uint64_t offsetOf(const Price p) const noexcept {
        return raw(p) - raw(base);
    }

    [[nodiscard]] bool inWindow(const Price p) const noexcept { return offsetOf(p) < width; }

    [[nodiscard]] std::size_t slotOf(const Price p) const noexcept {
        return static_cast<std::size_t>(raw(p) & mask);
    }

    [[nodiscard]] Price priceAt(const std::size_t offset) const noexcept {
        return Price{static_cast<std::int64_t>(raw(base) + offset)};
    }

    [[nodiscard]] Price top() const noexcept { return priceAt(width - 1); }

    [[nodiscard]] bool isWindowLevel(const PriceLevel& level) const noexcept {
        return &level >= window.get() && &level < window.get() + width;
    }

    // Maps on the better / worse side of the window. Both use the side's ordering, so begin() is
    // the best price of each.
    [[nodiscard]] const LevelMap& betterMap() const noexcept {
        if constexpr (Side == OrderSide::BUY) {
            return above;
        } else {
            return below;
        }
    }

    [[nodiscard]] const LevelMap& worseMap() const noexcept {
        if constexpr (Side == OrderSide::BUY) {
            return below;
        } else {
            return above;
        }
    }

    // --- Ring search over the occupancy bitmap ------------------------------------------------

    // Highest occupied offset <= `offset`, as a price.
    [[nodiscard]] std::optional<Price> highestAtOrBelow(const std::size_t offset) const noexcept {
        const std::size_t b0 = slotOf(base);
        const std::size_t s = (b0 + offset) & mask;
        std::size_t idx = occupied.findPrev(s);
        if (s >= b0) {
            if (idx == LevelBitmap::npos || idx < b0) {
                return std::nullopt;
            }
        } else if (idx == LevelBitmap::npos) {
            idx = occupied.findPrev(width - 1);
            if (idx == LevelBitmap::npos || idx < b0) {
                return std::nullopt;
            }
        }
        return priceAt((idx - b0) & mask);
    }

    // Lowest occupied offset >= `offset`, as a price.
    [[nodiscard]] std::optional<Price> lowestAtOrAbove(const std::size_t offset) const noexcept {
        const std::size_t b0 = slotOf(base);
        const std::size_t s = (b0 + offset) & mask;
        std::size_t idx = occupied.findNext(s);
        if (s >= b0) {
            if (idx == LevelBitmap::npos) {
                idx = occupied.findNext(0);
                if (idx == LevelBitmap::npos || idx >= b0) {
                    return std::nullopt;
                }
            }
        } else if (idx == LevelBitmap::npos || idx >= b0) {
            return std::nullopt;
        }
        return priceAt((idx - b0) & mask);
    }

    // Next level after `p` in best-to-worst order, inside the window.
    [[nodiscard]] std::optional<Price> nextWorseInWindow(const Price p) const noexcept {
        const std::uint64_t off = offsetOf(p);
        if constexpr (Side == OrderSide::BUY) {
            return off == 0 ? std::nullopt : highestAtOrBelow(off - 1);
        } else {
            return off + 1 >= width ? std::nullopt : lowestAtOrAbove(off + 1);
        }
    }

    [[nodiscard]] std::optional<Price> bestInWindow() const noexcept {
        if constexpr (Side == OrderSide::BUY) {
            return highestAtOrBelow(width - 1);
        } else {
            return lowestAtOrAbove(0);
        }
    }

    void markOccupied(const Price p) noexcept {
        occupied.set(slotOf(p));
        if (windowActive++ == 0 || SideTraits<Side>::isBetter(p, windowBest)) {
            windowBest = p;
        }
    }

    void markEmpty(const Price p) noexcept {
        occupied.reset(slotOf(p));
        if (--windowActive != 0 && p == windowBest) {
            windowBest = *nextWorseInWindow(p);
        }
    }

    // --- Recentering -------------------------------------------------------------------------

    void recenter() {
        const std::optional<Price> touch = bestPrice();
        if (!touch) {
            return;
        }
        // Keep the touch in the middle half of the window.
        const std::uint64_t off = offsetOf(*touch);
        if (off >= width / 4 && off < width - width / 4) {
            return;
        }
        const Price target = clampBase(static_cast<std::int64_t>(raw(*touch) - width / 2), width);
        std::size_t movesLeft = maxMoves;
        for (std::size_t shifts = 0; shifts < maxShifts && movesLeft >= 2 && base != target;
             ++shifts) {
            movesLeft -= base < target ? shiftUp() : shiftDown();
        }
    }

    // Moves the window one tick up: `base` leaves (to `below`), base + width enters (from
    // `above`). Returns the number of levels moved (0..2).
    std::size_t shiftUp() {
        std::size_t moved = 0;
        const Price leaving = base;
        PriceLevel& slot = window[slotOf(leaving)];
        bool recompute = false;
        if (!slot.empty()) {
            evict(slot, below);
            recompute = leaving == windowBest;
            ++moved;
        }
        base = Price{static_cast<std::int64_t>(raw(base) + 1)};
        const Price entering = top();
        slot.levelPrice = entering;
        if (!above.empty()) {
            const auto closest = Side == OrderSide::BUY ? std::prev(above.end()) : above.begin();
            if (closest->first == entering) {
                import(slot, above, closest);
                recompute = true;
                ++moved;
            }
        }
        if (recompute) {
            refreshWindowBest();
        }
        return moved;
    }

    // Moves the window one tick down: base + width - 1 leaves (to `above`), base - 1 enters (from
    // `below`).
    std::size_t shiftDown() {
        std::size_t moved = 0;
        const Price leaving = top();
        PriceLevel& slot = window[slotOf(leaving)];
        bool recompute = false;
        if (!slot.empty()) {
            evict(slot, above);
            recompute = leaving == windowBest;
            ++moved;
        }
        base = Price{static_cast<std::int64_t>(raw(base) - 1)};
        const Price entering = base;
        slot.levelPrice = entering;
        if (!below.empty()) {
            const auto closest = Side == OrderSide::BUY ? below.begin() : std::prev(below.end());
            if (closest->first == entering) {
                import(slot, below, closest);
                recompute = true;
                ++moved;
            }
        }
        if (recompute) {
            refreshWindowBest();
        }
        return moved;
    }

    void evict(PriceLevel& slot, LevelMap& to) {
        const Price p = slot.price();
        to.try_emplace(p, p).first->second.adoptFrom(slot);
        occupied.reset(slotOf(p));
        --windowActive;
        ++moves;
    }

    void import(PriceLevel& slot, LevelMap& from, const typename LevelMap::iterator it) {
        slot.adoptFrom(it->second);
        from.erase(it);
        occupied.set(slotOf(slot.price()));
        ++windowActive;
        ++moves;
    }

    void refreshWindowBest() noexcept {
        if (windowActive != 0) {
            windowBest = *bestInWindow();
        }
    }

    [[nodiscard]] static bool queueConsistent(const PriceLevel& level) noexcept {
        std::uint32_t n = 0;
        Quantity sum{0};
        for (const Order* o = level.front(); o != nullptr; o = o->nextInLevel()) {
            if (o->getPrice() != level.price() ||
                PriceLevel::ownerOf(const_cast<Order&>(*o)) != &level) {
                return false;
            }
            ++n;
            sum += o->getQuantity();
        }
        return n == level.orderCount() && sum == level.quantity();
    }

    std::size_t width;
    std::size_t mask;
    std::size_t maxLevels;
    std::size_t maxShifts;
    std::size_t maxMoves;
    Price base;
    std::unique_ptr<PriceLevel[]> window;
    LevelBitmap occupied;
    std::size_t windowActive{0};
    Price windowBest{0}; // valid while windowActive != 0
    std::uint64_t moves{0};
    FixedBlockResource overflowNodes; // before the maps: they keep a pointer to it
    LevelMap above;                   // prices > base + width - 1
    LevelMap below;                   // prices < base
};

static_assert(PriceLadder<HybridPriceLadder<OrderSide::BUY>>);
static_assert(PriceLadder<HybridPriceLadder<OrderSide::SELL>>);

} // namespace eigenbook
