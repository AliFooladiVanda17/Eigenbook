#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <vector>

#include <eigenbook/book/price_ladder.hpp>
#include <eigenbook/book/price_level.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>
#include <eigenbook/util/fixed_block_resource.hpp>

namespace eigenbook {

// ---------------------------------------------------------------------------------------------
// Balanced tree ladder: for sparse or unbounded price spaces.
//
// Adding at a price is O(log P) (the level is looked up even when it exists); best level and
// removal of the best level are O(1) amortised. Tree nodes come from a ladder-local memory
// resource chosen by NodePolicy at compile time:
//
//   M0 MapNodesOnHeap             pmr::unsynchronized_pool_resource with the default upstream.
//                                 Freed nodes are reused, but every new high in the number of live
//                                 levels makes the pool fetch a chunk from the heap.
//   M1 MapNodesOnMonotonicBuffer  the same pool over a monotonic_buffer_resource on a buffer
//                                 preallocated for maxLevels, whose upstream is
//                                 null_memory_resource(); the pool is pre-warmed with maxLevels
//                                 nodes at construction, so it never asks for more afterwards.
//   M2 MapNodesOnFixedBlocks      a dedicated single-size free list (FixedBlockResource) of
//                                 maxLevels map nodes, preallocated and pre-faulted.
//
// maxLevels bounds the number of levels (0 = unbounded, M0 only). A new level beyond it is not an
// allocation but canRestAt() == false, which the book turns into CAPACITY_EXCEEDED.
// ---------------------------------------------------------------------------------------------
struct MapLadderConfig {
    std::size_t maxLevels{0};
};

namespace detail {

template <OrderSide Side>
using LevelMap = std::pmr::map<Price, PriceLevel, typename SideTraits<Side>::Compare>;

// Size and alignment of one level-map node. Implementation-defined, so measured once: record what
// a map asks its resource for when one element is inserted.
struct NodeLayout {
    std::size_t bytes{0};
    std::size_t alignment{0};
};

template <OrderSide Side>
NodeLayout levelMapNodeLayout() {
    static const NodeLayout layout = [] {
        struct Recorder final : std::pmr::memory_resource {
            bool armed{false};
            NodeLayout seen;

            void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
                if (armed) {
                    seen.bytes = std::max(seen.bytes, bytes);
                    seen.alignment = std::max(seen.alignment, alignment);
                }
                return std::pmr::new_delete_resource()->allocate(bytes, alignment);
            }

            void
            do_deallocate(void* p, const std::size_t bytes, const std::size_t alignment) override {
                std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
            }

            [[nodiscard]] bool
            do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
                return this == &o;
            }
        } recorder;
        {
            LevelMap<Side> probe(&recorder);
            recorder.armed = true;
            probe.try_emplace(Price{0}, Price{0});
            recorder.armed = false;
        }
        return recorder.seen;
    }();
    return layout;
}

} // namespace detail

// M0: today's behaviour.
class MapNodesOnHeap {
public:
    MapNodesOnHeap(const MapLadderConfig&, detail::NodeLayout)
        : pool(std::make_unique<std::pmr::unsynchronized_pool_resource>()) {}

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return pool.get(); }

private:
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> pool;
};

// M1: pool over a bounded, preallocated arena.
class MapNodesOnMonotonicBuffer {
public:
    MapNodesOnMonotonicBuffer(const MapLadderConfig& config, const detail::NodeLayout node)
        : bytes(arenaBytes(config, node))
        , buffer(std::make_unique<std::byte[]>(bytes)) // value-initialised: every page touched
        , arena(buffer.get(), bytes, std::pmr::null_memory_resource())
        , pool(std::pmr::pool_options{.max_blocks_per_chunk = config.maxLevels + 8,
                                      .largest_required_pool_block = node.bytes},
               &arena) {
        // Pre-warm: make the pool fetch every chunk it will ever need for maxLevels nodes now.
        // Afterwards its free lists hold them and the arena is never asked again.
        std::vector<void*> blocks(config.maxLevels + kHeadroom);
        for (void*& b : blocks) {
            b = pool.allocate(node.bytes, node.alignment);
        }
        for (void* b : blocks) {
            pool.deallocate(b, node.bytes, node.alignment);
        }
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return &pool; }

private:
    // The map's head node, and the container proxy of MSVC debug builds.
    static constexpr std::size_t kHeadroom = 4;

    static std::size_t arenaBytes(const MapLadderConfig& config, const detail::NodeLayout node) {
        if (config.maxLevels == 0) {
            throw std::invalid_argument("MapNodesOnMonotonicBuffer needs "
                                        "MapLadderConfig::maxLevels > 0");
        }
        // The pool rounds blocks up (to a power of two in common implementations) and grows its
        // chunks geometrically: allow 4x the node bytes plus fixed bookkeeping. Construction
        // throws std::bad_alloc if this is ever too small, never the hot path.
        return 4 * (config.maxLevels + kHeadroom + 16) * std::bit_ceil(node.bytes) + 64 * 1024;
    }

    std::size_t bytes;
    std::unique_ptr<std::byte[]> buffer;
    std::pmr::monotonic_buffer_resource arena;
    std::pmr::unsynchronized_pool_resource pool;
};

// M2: dedicated single-size free list.
class MapNodesOnFixedBlocks {
public:
    MapNodesOnFixedBlocks(const MapLadderConfig& config, const detail::NodeLayout node)
        : blocks(node.bytes, checkedCapacity(config)) {}

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return &blocks; }

    [[nodiscard]] const FixedBlockResource& blockResource() const noexcept { return blocks; }

private:
    static std::size_t checkedCapacity(const MapLadderConfig& config) {
        if (config.maxLevels == 0) {
            throw std::invalid_argument("MapNodesOnFixedBlocks needs MapLadderConfig::maxLevels > "
                                        "0");
        }
        return config.maxLevels + 4; // + head node and MSVC debug container proxy
    }

    FixedBlockResource blocks;
};

template <OrderSide Side, typename NodePolicy>
class BasicMapPriceLadder {
public:
    static constexpr OrderSide side = Side;

    using Config = MapLadderConfig;
    using Policy = NodePolicy;

    explicit BasicMapPriceLadder(const Config config = {})
        : maxLevels(config.maxLevels)
        , nodes(config, detail::levelMapNodeLayout<Side>())
        , levels(nodes.resource()) {}

    // Orders and the book hold pointers into the levels.
    BasicMapPriceLadder(const BasicMapPriceLadder&) = delete;
    BasicMapPriceLadder& operator=(const BasicMapPriceLadder&) = delete;
    BasicMapPriceLadder(BasicMapPriceLadder&&) = delete;
    BasicMapPriceLadder& operator=(BasicMapPriceLadder&&) = delete;

    ~BasicMapPriceLadder() = default;

    [[nodiscard]] bool accepts(Price) const noexcept { return true; }

    [[nodiscard]] bool canRestAt(const Price price) const noexcept {
        return maxLevels == 0 || levels.size() < maxLevels || levels.contains(price);
    }

    // Precondition: canRestAt(order.getPrice()).
    void addOrder(Order& order) {
        assert(canRestAt(order.getPrice()));
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
        forEachLevelWhile([&](const PriceLevel& level) {
            cb(level);
            return true;
        });
    }

    // Like forEachLevel, but stops as soon as cb(const PriceLevel&) returns false. Read-only.
    template <typename Callback>
    void forEachLevelWhile(Callback&& cb) const {
        for (const auto& [price, level] : levels) {
            if (!cb(level)) {
                return;
            }
        }
    }

    [[nodiscard]] std::size_t levelCount() const noexcept { return levels.size(); }

    [[nodiscard]] bool empty() const noexcept { return levels.empty(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return maxLevels; }

    [[nodiscard]] const NodePolicy& nodePolicy() const noexcept { return nodes; }

    // Audit (cold path, O(orders)): every level is non-empty, keyed by its own price and
    // consistent (PriceLevel::checkInvariants); the level count respects maxLevels.
    [[nodiscard]] bool checkInvariants() const noexcept {
        for (const auto& [price, level] : levels) {
            if (level.empty() || level.price() != price || !level.checkInvariants()) {
                return false;
            }
        }
        return maxLevels == 0 || levels.size() <= maxLevels;
    }

private:
    std::size_t maxLevels;
    NodePolicy nodes; // before `levels`: the map keeps a pointer to its resource
    detail::LevelMap<Side> levels;
};

// M0: the default sparse ladder (unchanged behaviour; unbounded unless maxLevels is set).
template <OrderSide Side>
using MapPriceLadder = BasicMapPriceLadder<Side, MapNodesOnHeap>;

// M1 / M2: bounded, no allocation after construction. Require maxLevels > 0.
template <OrderSide Side>
using MapPriceLadderM1 = BasicMapPriceLadder<Side, MapNodesOnMonotonicBuffer>;

template <OrderSide Side>
using MapPriceLadderM2 = BasicMapPriceLadder<Side, MapNodesOnFixedBlocks>;

static_assert(PriceLadder<MapPriceLadder<OrderSide::BUY>>);
static_assert(PriceLadder<MapPriceLadder<OrderSide::SELL>>);
static_assert(PriceLadder<MapPriceLadderM1<OrderSide::BUY>>);
static_assert(PriceLadder<MapPriceLadderM1<OrderSide::SELL>>);
static_assert(PriceLadder<MapPriceLadderM2<OrderSide::BUY>>);
static_assert(PriceLadder<MapPriceLadderM2<OrderSide::SELL>>);

} // namespace eigenbook
