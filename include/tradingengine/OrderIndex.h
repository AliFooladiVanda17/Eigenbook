//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_INCLUDE_TRADINGENGINE_ORDERINDEX_H
#define HPC_QUEUE_INCLUDE_TRADINGENGINE_ORDERINDEX_H

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <tradingengine/order.h>
#include <tradingengine/types.h>

// OrderId -> Order* for O(1) cancel / modify.
//
// Open addressing with linear probing over a flat power-of-two array and backward-shift deletion
// (no tombstones), so lookups stay short and contiguous and nothing is allocated per order. The
// load factor is kept <= 1/2; reserve() at start-up avoids rehashing on the hot path.
class OrderIndex {
public:
    explicit OrderIndex(const std::size_t expectedOrders = 1024) { reserve(expectedOrders); }

    [[nodiscard]] Order* find(const OrderId id) const noexcept {
        for (std::size_t i = slotFor(id.value);; i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.order == nullptr) {
                return nullptr;
            }
            if (slot.key == id.value) {
                return slot.order;
            }
        }
    }

    [[nodiscard]] bool contains(const OrderId id) const noexcept { return find(id) != nullptr; }

    // Returns false (and changes nothing) when the id is already present.
    bool insert(const OrderId id, Order* order) {
        assert(order != nullptr);
        if ((count + 1) * 2 > slots.size()) [[unlikely]] {
            rehash(slots.size() * 2);
        }
        for (std::size_t i = slotFor(id.value);; i = (i + 1) & mask) {
            Slot& slot = slots[i];
            if (slot.order == nullptr) {
                slot = {id.value, order};
                ++count;
                return true;
            }
            if (slot.key == id.value) {
                return false;
            }
        }
    }

    bool erase(const OrderId id) noexcept {
        std::size_t hole = slotFor(id.value);
        for (;; hole = (hole + 1) & mask) {
            if (slots[hole].order == nullptr) {
                return false;
            }
            if (slots[hole].key == id.value) {
                break;
            }
        }
        // Pull later members of the probe run back into the hole so lookups never stop early.
        for (std::size_t i = (hole + 1) & mask; slots[i].order != nullptr; i = (i + 1) & mask) {
            const std::size_t home = slotFor(slots[i].key);
            if (((i - home) & mask) >= ((i - hole) & mask)) {
                slots[hole] = slots[i];
                hole = i;
            }
        }
        slots[hole] = {};
        --count;
        return true;
    }

    void reserve(const std::size_t n) {
        const std::size_t wanted = std::bit_ceil(std::max<std::size_t>(16, n * 2));
        if (wanted > slots.size()) {
            rehash(wanted);
        }
    }

    void clear() noexcept {
        std::fill(slots.begin(), slots.end(), Slot{});
        count = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count; }

    [[nodiscard]] bool empty() const noexcept { return count == 0; }

private:
    struct Slot {
        std::uint64_t key{0};
        Order* order{nullptr}; // nullptr marks an empty slot
    };

    // splitmix64 finalizer: exchange ids are often sequential, which identity hashing would cluster.
    [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    [[nodiscard]] std::size_t slotFor(const std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(mix(key)) & mask;
    }

    void rehash(const std::size_t newCapacity) {
        std::vector<Slot> old(newCapacity);
        old.swap(slots);
        mask = newCapacity - 1;
        for (const Slot& slot : old) {
            if (slot.order != nullptr) {
                std::size_t i = slotFor(slot.key);
                while (slots[i].order != nullptr) {
                    i = (i + 1) & mask;
                }
                slots[i] = slot;
            }
        }
    }

    std::vector<Slot> slots;
    std::size_t mask{0};
    std::size_t count{0};
};

#endif // HPC_QUEUE_INCLUDE_TRADINGENGINE_ORDERINDEX_H
