#pragma once

// DirectOrderTable: engine-issued order handles with O(1) worst-case lookup.
//
// A handle is an OrderId whose low 32 bits are a slot index and whose high 32 bits are that
// slot's generation. issue() takes a free slot and returns its handle; find() is one array index
// and one generation compare: no hashing, no probing, nothing an id chooser can influence. When an
// order leaves the book (or a reserved handle is abandoned) the slot's generation is incremented,
// so a stale or duplicated handle no longer matches and is rejected.
//
// This is the exchange-side order reference: client order ids are mapped to handles at the
// gateway, off the matching thread. All memory is allocated at construction.
//
// Generations are 32 bits and skip 0 (so no handle is 0); a handle matches again only after its
// slot has been reused 2^32 - 1 times.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

#include <eigenbook/book/locator/order_locator.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

class DirectOrderTable {
public:
    explicit DirectOrderTable(const std::size_t iCapacity)
        : slotCount(checkedCapacity(iCapacity)), slots(std::make_unique<Slot[]>(slotCount)) {
        for (std::size_t i = slotCount; i-- > 0;) {
            slots[i].nextFree = freeHead;
            freeHead = static_cast<std::uint32_t>(i);
        }
    }

    DirectOrderTable(const DirectOrderTable&) = delete;
    DirectOrderTable& operator=(const DirectOrderTable&) = delete;
    DirectOrderTable(DirectOrderTable&&) = delete;
    DirectOrderTable& operator=(DirectOrderTable&&) = delete;
    ~DirectOrderTable() = default;

    [[nodiscard]] static constexpr OrderId handle(const std::uint32_t slot,
                                                  const std::uint32_t generation) noexcept {
        return OrderId{(std::uint64_t{generation} << 32) | slot};
    }

    [[nodiscard]] static constexpr std::uint32_t slotOf(const OrderId id) noexcept {
        return static_cast<std::uint32_t>(id.value);
    }

    [[nodiscard]] static constexpr std::uint32_t generationOf(const OrderId id) noexcept {
        return static_cast<std::uint32_t>(id.value >> 32);
    }

    // Reserves a slot and returns its handle, or nullopt if every slot is in use.
    [[nodiscard]] std::optional<OrderId> issue() noexcept {
        if (freeHead == kEnd) [[unlikely]] {
            return std::nullopt;
        }
        const std::uint32_t s = freeHead;
        Slot& slot = slots[s];
        freeHead = slot.nextFree;
        slot.nextFree = kReserved;
        return handle(s, slot.generation);
    }

    // Gives back a reserved (not resting) handle. False if the handle is not reserved.
    bool release(const OrderId id) noexcept {
        Slot* slot = match(id);
        if (slot == nullptr || slot->nextFree != kReserved || slot->order != nullptr) {
            return false;
        }
        free(*slot, slotOf(id));
        return true;
    }

    [[nodiscard]] Order* find(const OrderId id) const noexcept {
        const std::uint32_t s = slotOf(id);
        if (s >= slotCount) [[unlikely]] {
            return nullptr;
        }
        const Slot& slot = slots[s];
        return slot.generation == generationOf(id) ? slot.order : nullptr;
    }

    [[nodiscard]] Admission admit(const OrderId id) const noexcept {
        const Slot* slot = match(id);
        if (slot == nullptr || slot->nextFree != kReserved) {
            return Admission::NOT_ISSUED;
        }
        return slot->order != nullptr ? Admission::DUPLICATE : Admission::OK;
    }

    // Precondition: admit(id) == Admission::OK.
    void insert(const OrderId id, Order* order) noexcept {
        assert(order != nullptr && admit(id) == Admission::OK);
        slots[slotOf(id)].order = order;
        ++live;
    }

    // The order leaves the book; its handle becomes stale.
    bool erase(const OrderId id) noexcept {
        Slot* slot = match(id);
        if (slot == nullptr || slot->order == nullptr) {
            return false;
        }
        slot->order = nullptr;
        --live;
        free(*slot, slotOf(id));
        return true;
    }

    // The order leaves the book but keeps its (still reserved) handle: modify re-entry.
    void unbind(const OrderId id) noexcept {
        Slot* slot = match(id);
        if (slot != nullptr && slot->order != nullptr) {
            slot->order = nullptr;
            --live;
        }
    }

    // An admitted handle whose order did not rest.
    void abandon(const OrderId id) noexcept { (void)release(id); }

    [[nodiscard]] std::size_t size() const noexcept { return live; }

    [[nodiscard]] bool empty() const noexcept { return live == 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return slotCount; }

private:
    static constexpr std::uint32_t kEnd = 0xFFFF'FFFF;
    static constexpr std::uint32_t kReserved = 0xFFFF'FFFE;

    struct Slot {
        Order* order{nullptr};
        std::uint32_t generation{1};
        std::uint32_t nextFree{kEnd}; // free: next free slot; reserved / resting: kReserved
    };

    static std::size_t checkedCapacity(const std::size_t n) {
        if (n == 0 || n >= kReserved) {
            throw std::invalid_argument("DirectOrderTable capacity must be in [1, 2^32 - 2)");
        }
        return n;
    }

    [[nodiscard]] Slot* match(const OrderId id) const noexcept {
        const std::uint32_t s = slotOf(id);
        if (s >= slotCount || slots[s].generation != generationOf(id)) {
            return nullptr;
        }
        return &slots[s];
    }

    void free(Slot& slot, const std::uint32_t index) noexcept {
        if (++slot.generation == 0) {
            slot.generation = 1;
        }
        slot.nextFree = freeHead;
        freeHead = index;
    }

    std::size_t slotCount;
    std::unique_ptr<Slot[]> slots;
    std::uint32_t freeHead{kEnd};
    std::size_t live{0};
};

static_assert(IssuingOrderLocator<DirectOrderTable>);

} // namespace eigenbook
