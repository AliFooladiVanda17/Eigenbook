#pragma once

// HardenedOrderIndex: client-chosen ids, but resistant to engineered collisions (HashDoS).
//
// Same flat table as OrderIndex (open addressing, linear probing, backward-shift deletion), with
// three changes:
//   1. Keyed hash: home slot = OrderIndex::hash(id ^ seed) with a per-instance 64-bit seed (from
//      std::random_device unless configured). Without the seed, colliding ids cannot be computed
//      offline by inverting the mixer. It is not a cryptographic PRF; see docs/plans/PHASE-A.md.
//   2. Bounded probe distance: an id that would be stored more than maxProbe slots from its home
//      is refused at admit() (Admission::PROBE_LIMIT, counted). Every stored id is therefore
//      within maxProbe of its home, so find() gives up after maxProbe + 1 slots: lookups are
//      O(maxProbe) in the worst case.
//   3. Fixed capacity: no rehash. When the load factor would exceed 1/2, admit() says FULL.
//
// erase() still scans the rest of the cluster for the backward shift; the cluster length is not
// bounded by maxProbe, only made hard to inflate deliberately by the keyed hash.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>

#include <eigenbook/book/locator/hash_order_index.hpp>
#include <eigenbook/book/locator/order_locator.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

struct HardenedIndexConfig {
    std::optional<std::uint64_t> seed; // nullopt: random per instance
    // Slots beyond the home slot an id may be stored at. Must sit far above the natural maximum at
    // load 1/2: with 32, the zero-allocation test's W4-load50 run (131,071 live sequential ids)
    // had 19 legitimate ids refused (Phase A4); with 128, none.
    std::size_t maxProbe{128};
};

class HardenedOrderIndex {
public:
    explicit HardenedOrderIndex(const std::size_t expectedOrders,
                                const HardenedIndexConfig config = {})
        : slotCount(OrderIndex::capacityFor(expectedOrders))
        , mask(slotCount - 1)
        , slots(std::make_unique<Slot[]>(slotCount))
        , seed(config.seed ? *config.seed : randomSeed())
        , maxProbe(config.maxProbe) {}

    HardenedOrderIndex(const HardenedOrderIndex&) = delete;
    HardenedOrderIndex& operator=(const HardenedOrderIndex&) = delete;
    HardenedOrderIndex(HardenedOrderIndex&&) = delete;
    HardenedOrderIndex& operator=(HardenedOrderIndex&&) = delete;
    ~HardenedOrderIndex() = default;

    [[nodiscard]] Order* find(const OrderId id) const noexcept {
        std::size_t i = homeOf(id.value);
        for (std::size_t distance = 0; distance <= maxProbe; ++distance, i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.order == nullptr) {
                return nullptr;
            }
            if (slot.key == id.value) {
                return slot.order;
            }
        }
        return nullptr; // no stored id lives further than maxProbe from its home
    }

    [[nodiscard]] Admission admit(const OrderId id) const noexcept {
        std::size_t i = homeOf(id.value);
        for (std::size_t distance = 0; distance <= maxProbe; ++distance, i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.order == nullptr) {
                return (live + 1) * 2 > slotCount ? Admission::FULL : Admission::OK;
            }
            if (slot.key == id.value) {
                return Admission::DUPLICATE;
            }
        }
        ++probeRejects;
        return Admission::PROBE_LIMIT;
    }

    // Precondition: admit(id) == Admission::OK (the slot found then is still within reach: erases
    // in between only move entries closer to their homes).
    void insert(const OrderId id, Order* order) noexcept {
        assert(order != nullptr && admit(id) == Admission::OK);
        std::size_t i = homeOf(id.value);
        while (slots[i].order != nullptr) {
            i = (i + 1) & mask;
        }
        slots[i] = {id.value, order};
        ++live;
    }

    bool erase(const OrderId id) noexcept {
        std::size_t hole = homeOf(id.value);
        for (std::size_t distance = 0;; ++distance, hole = (hole + 1) & mask) {
            if (distance > maxProbe || slots[hole].order == nullptr) {
                return false;
            }
            if (slots[hole].key == id.value) {
                break;
            }
        }
        // Backward shift, as in OrderIndex. Moves only bring entries closer to their homes, so the
        // probe-distance bound keeps holding.
        for (std::size_t i = (hole + 1) & mask; slots[i].order != nullptr; i = (i + 1) & mask) {
            const std::size_t home = homeOf(slots[i].key);
            if (((i - home) & mask) >= ((i - hole) & mask)) {
                slots[hole] = slots[i];
                hole = i;
            }
        }
        slots[hole] = {};
        --live;
        return true;
    }

    void unbind(const OrderId id) noexcept { erase(id); }

    void abandon(OrderId) noexcept {}

    [[nodiscard]] std::size_t size() const noexcept { return live; }

    [[nodiscard]] bool empty() const noexcept { return live == 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return slotCount; }

    // Ids refused because they would have been stored too far from home.
    [[nodiscard]] std::uint64_t probeLimitRejects() const noexcept { return probeRejects; }

    // Longest run of consecutive occupied slots (wrapping). O(capacity): cold path only.
    [[nodiscard]] std::size_t maxClusterLength() const noexcept {
        std::size_t start = 0;
        while (start < slotCount && slots[start].order != nullptr) {
            ++start;
        }
        if (start == slotCount) {
            return slotCount;
        }
        std::size_t longest = 0;
        std::size_t run = 0;
        for (std::size_t n = 1; n <= slotCount; ++n) {
            if (slots[(start + n) & mask].order != nullptr) {
                longest = std::max(longest, ++run);
            } else {
                run = 0;
            }
        }
        return longest;
    }

private:
    struct Slot {
        std::uint64_t key{0};
        Order* order{nullptr};
    };

    [[nodiscard]] static std::uint64_t randomSeed() {
        std::random_device device;
        return (std::uint64_t{device()} << 32) ^ device();
    }

    [[nodiscard]] std::size_t homeOf(const std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(OrderIndex::hash(key ^ seed)) & mask;
    }

    std::size_t slotCount;
    std::size_t mask;
    std::unique_ptr<Slot[]> slots;
    std::uint64_t seed;
    std::size_t maxProbe;
    std::size_t live{0};
    mutable std::uint64_t probeRejects{0};
};

static_assert(OrderLocator<HardenedOrderIndex>);

} // namespace eigenbook
