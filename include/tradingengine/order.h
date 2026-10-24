//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_ORDER_H
#define HPC_QUEUE_SRC_ORDER_H

#include <type_traits>
#include <tradingengine/types.h>

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
          const Timestamp iTimestamp) noexcept
        : id(iId)
        , price(iPrice)
        , quantity(iQuantity)
        , timestamp(iTimestamp)
        , side(iSide)
        , type(iType) {}

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

    [[nodiscard]] bool isFilled() const noexcept { return quantity.isZero(); }

    // Next order in time priority at the same price, or nullptr.
    [[nodiscard]] const Order* nextInLevel() const noexcept { return next; }

private:
    // Quantity and links change only through PriceLevel, which keeps the level aggregate in sync.
    friend class PriceLevel;

    Order* prev{nullptr};
    Order* next{nullptr};
    PriceLevel* level{nullptr};

    OrderId id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    OrderSide side;
    OrderType type;
};

// The README's name for the book's node type.
using OrderNode = Order;

static_assert(sizeof(Order) == kCacheLineSize, "Order must occupy exactly one cache line");
static_assert(std::is_trivially_destructible_v<Order>, "the pool releases nodes without destructors");

#endif // HPC_QUEUE_SRC_ORDER_H
