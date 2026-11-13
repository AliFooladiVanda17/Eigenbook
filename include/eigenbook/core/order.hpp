#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <eigenbook/core/types.hpp>

namespace eigenbook {

class PriceLevel;

// A resting order, which is also the intrusive node of its price level's FIFO queue.
//
// Exactly one cache line: the links used while walking a queue and the fields read while matching
// (quantity, id) are fetched together. Nodes are owned by the book's pool and are never copied or
// moved, because the level and the order index point at them.
class alignas(kCacheLineSize) Order {
public:
    Order(const OrderId iId,
          const OrderSide iSide,
          const OrderType iType,
          const Price iPrice,
          const Quantity iQuantity,
          const Timestamp iTimestamp,
          const ParticipantId iParticipant = ParticipantId{0},
          const TimeInForce iTimeInForce = TimeInForce::DAY,
          const bool iPostOnly = false) noexcept
        : id(iId)
        , price(iPrice)
        , quantity(iQuantity)
        , timestamp(iTimestamp)
        , side(iSide)
        , type(iType)
        , flags(packFlags(iTimeInForce, iPostOnly))
        , participant(iParticipant) {}

    Order(const Order&) = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&&) = delete;
    Order& operator=(Order&&) = delete;

    ~Order() = default;

    [[nodiscard]] OrderId getId() const noexcept { return id; }

    [[nodiscard]] OrderSide getSide() const noexcept { return side; }

    [[nodiscard]] OrderType getType() const noexcept { return type; }

    [[nodiscard]] Price getPrice() const noexcept { return price; }

    // Remaining (open) quantity.
    [[nodiscard]] Quantity getQuantity() const noexcept { return quantity; }

    [[nodiscard]] Timestamp getTimestamp() const noexcept { return timestamp; }

    [[nodiscard]] ParticipantId getParticipant() const noexcept { return participant; }

    // DAY or GTC for a resting order (IOC / FOK never rest).
    [[nodiscard]] TimeInForce getTimeInForce() const noexcept {
        return static_cast<TimeInForce>(flags & kTimeInForceMask);
    }

    // A re-entry (modify) of a post-only order must not take liquidity either.
    [[nodiscard]] bool isPostOnly() const noexcept { return (flags & kPostOnlyBit) != 0; }

    [[nodiscard]] bool isFilled() const noexcept { return quantity.isZero(); }

    // Next order in time priority at the same price, or nullptr.
    [[nodiscard]] const Order* nextInLevel() const noexcept { return next; }

private:
    // Quantity and links change only through PriceLevel, which keeps the level aggregate in sync.
    friend class PriceLevel;
    friend struct OrderLayout;

    static constexpr std::uint16_t kTimeInForceMask = 0x3;
    static constexpr std::uint16_t kPostOnlyBit = 0x4;

    [[nodiscard]] static constexpr std::uint16_t packFlags(const TimeInForce tif,
                                                           const bool postOnly) noexcept {
        return static_cast<std::uint16_t>((static_cast<unsigned>(tif) & kTimeInForceMask) |
                                          (postOnly ? kPostOnlyBit : 0U));
    }

    Order* prev{nullptr};
    Order* next{nullptr};
    PriceLevel* level{nullptr};

    OrderId id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    OrderSide side;
    OrderType type;
    std::uint16_t flags; // time in force (bits 0-1), post-only (bit 2)
    ParticipantId participant;
};

// The README's name for the book's node type.
using OrderNode = Order;

// The field layout is part of the design (the matching loop reads links, id and quantity from one
// line); later phases put new fields into the gaps named here, so every offset is checked.
struct OrderLayout {
    static_assert(std::is_standard_layout_v<Order>);
    static_assert(offsetof(Order, prev) == 0);
    static_assert(offsetof(Order, next) == 8);
    static_assert(offsetof(Order, level) == 16);
    static_assert(offsetof(Order, id) == 24);
    static_assert(offsetof(Order, price) == 32);
    static_assert(offsetof(Order, quantity) == 40);
    static_assert(offsetof(Order, timestamp) == 48);
    static_assert(offsetof(Order, side) == 56);
    static_assert(offsetof(Order, type) == 57);
    static_assert(offsetof(Order, flags) == 58);
    static_assert(offsetof(Order, participant) == 60);
};

static_assert(sizeof(Order) == kCacheLineSize, "Order must occupy exactly one cache line");
static_assert(std::is_trivially_destructible_v<Order>,
              "the pool releases nodes without destructors");

} // namespace eigenbook
