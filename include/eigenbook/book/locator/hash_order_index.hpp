#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
#include <array>
#endif

#include <eigenbook/book/locator/order_locator.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
#define EIGENBOOK_INDEX_STAT(...) __VA_ARGS__
#else
#define EIGENBOOK_INDEX_STAT(...)
#endif

namespace eigenbook {

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
// Probe statistics of one OrderIndex (benchmarks only; compiled in with
// EIGENBOOK_INSTRUMENT_INDEX).
//
// A length L is counted in bucket L for L < 64, and in bucket 64 + floor(log2(L)) - 6 above, so
// short probes are exact and long ones (an attacked cluster) are still visible. Exact maxima are
// kept separately.
struct OrderIndexStats {
    static constexpr std::size_t kBuckets = 64 + 58;

    [[nodiscard]] static constexpr std::size_t bucketOf(const std::uint64_t length) noexcept {
        return length < 64 ? static_cast<std::size_t>(length)
                           : 64 + static_cast<std::size_t>(std::bit_width(length)) - 7;
    }

    // Lowest length counted in a bucket.
    [[nodiscard]] static constexpr std::uint64_t bucketFloor(const std::size_t bucket) noexcept {
        return bucket < 64 ? bucket : std::uint64_t{1} << (bucket - 64 + 6);
    }

    struct Distribution {
        std::array<std::uint64_t, kBuckets> counts{};
        std::uint64_t total{0};
        std::uint64_t max{0};

        void record(const std::uint64_t length) noexcept {
            ++counts[bucketOf(length)];
            ++total;
            max = std::max(max, length);
        }
    };

    Distribution findProbes;   // slots inspected by find() (also used for the duplicate-id check)
    Distribution insertProbes; // slots inspected by insert()
    Distribution eraseProbes;  // slots inspected by erase() to locate the key
    Distribution eraseScan;    // slots inspected after the hole by the backward shift
    Distribution eraseShifts;  // entries moved by the backward shift

    void reset() noexcept { *this = {}; }
};
#endif

// OrderId -> Order* for expected O(1) cancel / modify.
//
// Open addressing with linear probing over a flat power-of-two array and backward-shift deletion
// (no tombstones), so lookups stay short and contiguous and nothing is allocated per order. The
// load factor is kept <= 1/2; reserve() at start-up avoids rehashing on the hot path.
//
// Worst case is the length of the probe cluster, which an adversary who chooses ids can make long
// (hash() is a public bijection); see docs/plans/PHASE-A.md, A4.
class OrderIndex {
public:
    explicit OrderIndex(const std::size_t expectedOrders = 1024) { reserve(expectedOrders); }

    // Table size reserve(expectedOrders) allocates: the smallest power of two >= 2 * expected
    // (at least 16), so the load factor stays <= 1/2.
    [[nodiscard]] static constexpr std::size_t
    capacityFor(const std::size_t expectedOrders) noexcept {
        return std::bit_ceil(std::max<std::size_t>(16, expectedOrders * 2));
    }

    // splitmix64 finalizer: exchange ids are often sequential, which identity hashing would
    // cluster. The home slot of a key is hash(key) & (capacity() - 1).
    [[nodiscard]] static constexpr std::uint64_t hash(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    // EIGENBOOK_INDEX_STAT(...) expands to its argument only in instrumented builds, so the default
    // build compiles exactly the uninstrumented statements.
    [[nodiscard]] Order* find(const OrderId id) const noexcept {
        EIGENBOOK_INDEX_STAT(std::uint64_t probes = 0;)
        for (std::size_t i = slotFor(id.value);; i = (i + 1) & mask) {
            EIGENBOOK_INDEX_STAT(++probes;)
            const Slot& slot = slots[i];
            if (slot.order == nullptr) {
                EIGENBOOK_INDEX_STAT(stats_.findProbes.record(probes);)
                return nullptr;
            }
            if (slot.key == id.value) {
                EIGENBOOK_INDEX_STAT(stats_.findProbes.record(probes);)
                return slot.order;
            }
        }
    }

    [[nodiscard]] bool contains(const OrderId id) const noexcept { return find(id) != nullptr; }

    // OrderLocator: any id that is not resting may be added (the table grows as needed).
    [[nodiscard]] Admission admit(const OrderId id) const noexcept {
        return contains(id) ? Admission::DUPLICATE : Admission::OK;
    }

    // Returns false (and changes nothing) when the id is already present.
    bool insert(const OrderId id, Order* order) {
        assert(order != nullptr);
        if ((count + 1) * 2 > slots.size()) [[unlikely]] {
            rehash(slots.size() * 2);
        }
        EIGENBOOK_INDEX_STAT(std::uint64_t probes = 0;)
        for (std::size_t i = slotFor(id.value);; i = (i + 1) & mask) {
            EIGENBOOK_INDEX_STAT(++probes;)
            Slot& slot = slots[i];
            if (slot.order == nullptr) {
                slot = {id.value, order};
                ++count;
                EIGENBOOK_INDEX_STAT(stats_.insertProbes.record(probes);)
                return true;
            }
            if (slot.key == id.value) {
                EIGENBOOK_INDEX_STAT(stats_.insertProbes.record(probes);)
                return false;
            }
        }
    }

    bool erase(const OrderId id) noexcept {
        std::size_t hole = slotFor(id.value);
        EIGENBOOK_INDEX_STAT(std::uint64_t probes = 0;)
        for (;; hole = (hole + 1) & mask) {
            EIGENBOOK_INDEX_STAT(++probes;)
            if (slots[hole].order == nullptr) {
                EIGENBOOK_INDEX_STAT(stats_.eraseProbes.record(probes);)
                return false;
            }
            if (slots[hole].key == id.value) {
                break;
            }
        }
        EIGENBOOK_INDEX_STAT(stats_.eraseProbes.record(probes);)
        EIGENBOOK_INDEX_STAT(std::uint64_t scanned = 0; std::uint64_t shifted = 0;)
        // Pull later members of the probe run back into the hole so lookups never stop early.
        for (std::size_t i = (hole + 1) & mask; slots[i].order != nullptr; i = (i + 1) & mask) {
            EIGENBOOK_INDEX_STAT(++scanned;)
            const std::size_t home = slotFor(slots[i].key);
            if (((i - home) & mask) >= ((i - hole) & mask)) {
                slots[hole] = slots[i];
                hole = i;
                EIGENBOOK_INDEX_STAT(++shifted;)
            }
        }
        EIGENBOOK_INDEX_STAT(stats_.eraseScan.record(scanned); stats_.eraseShifts.record(shifted);)
        slots[hole] = {};
        --count;
        return true;
    }

    // OrderLocator: client ids need no reservation, so leaving and abandoning are plain erases.
    void unbind(const OrderId id) noexcept { erase(id); }

    void abandon(OrderId) noexcept {}

    void reserve(const std::size_t n) {
        const std::size_t wanted = capacityFor(n);
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

    // Number of slots in the table.
    [[nodiscard]] std::size_t capacity() const noexcept { return slots.size(); }

    // Longest run of consecutive occupied slots (wrapping). O(capacity): cold path only.
    [[nodiscard]] std::size_t maxClusterLength() const noexcept {
        if (count == slots.size()) {
            return count;
        }
        // Start just after an empty slot so no run is split by the wrap-around.
        std::size_t start = 0;
        while (slots[start].order != nullptr) {
            ++start;
        }
        std::size_t longest = 0;
        std::size_t run = 0;
        for (std::size_t n = 1; n <= slots.size(); ++n) {
            if (slots[(start + n) & mask].order != nullptr) {
                longest = std::max(longest, ++run);
            } else {
                run = 0;
            }
        }
        return longest;
    }

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
    [[nodiscard]] const OrderIndexStats& stats() const noexcept { return stats_; }

    // The statistics are not part of the index's logical state, hence const.
    void resetStats() const noexcept { stats_.reset(); }
#endif

private:
    struct Slot {
        std::uint64_t key{0};
        Order* order{nullptr}; // nullptr marks an empty slot
    };

#if defined(EIGENBOOK_INSTRUMENT_INDEX)
    mutable OrderIndexStats stats_;
#endif

    [[nodiscard]] std::size_t slotFor(const std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(hash(key)) & mask;
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

static_assert(OrderLocator<OrderIndex>);

} // namespace eigenbook

#undef EIGENBOOK_INDEX_STAT
