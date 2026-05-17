//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_ORDER_H
#define HPC_QUEUE_SRC_ORDER_H

#include<algorithm>
#include<expected>
#include <tradingengine/types.h>

enum class OrderError {
    InvalidPrice = -1024,
    InvalidQuantity = -1025,
    NotEnoughMemory = -1026
};


class Order {
public:

    static std::expected<Order, OrderError>
    create(OrderType type,
           OrderSide side,
           Price price,
           Quantity quantity,
           OrderId id) {
        if (price.value <= 0)
            return std::unexpected(OrderError::InvalidPrice);

        if (quantity.value == 0)
            return std::unexpected(OrderError::InvalidQuantity);

        return Order(type, side, price, quantity, id);
    }

    [[nodiscard]] constexpr OrderType type() const noexcept { return type_; }
    [[nodiscard]] constexpr OrderSide side() const noexcept { return side_; }
    [[nodiscard]] constexpr Price price() const noexcept { return price_; }
    [[nodiscard]] constexpr Quantity remaining() const noexcept { return remainingQuantity_; }
    [[nodiscard]] constexpr Quantity original() const noexcept { return originalQuantity_; }
    [[nodiscard]] constexpr OrderId id() const noexcept { return id_; }
    [[nodiscard]] constexpr Timestamp timestamp() const noexcept { return timestamp_; }

    Quantity fill(Quantity qty) noexcept {
        auto& minimum = std::min(remainingQuantity_, qty);
        remainingQuantity_ = remainingQuantity_ - minimum;

        return minimum;
    }

    [[nodiscard]] bool isFilled() const noexcept {
        return remainingQuantity_ == Quantity{0};
    }

private:
    Order(OrderType type,
          OrderSide side,
          Price price,
          Quantity quantity,
          OrderId id)
            : type_(type),
              side_(side),
              price_(price),
              originalQuantity_(quantity),
              remainingQuantity_(quantity),
              timestamp_(std::chrono::steady_clock::now()),
              id_(id) {}

    OrderType type_;
    OrderSide side_;

    Price price_;

    Quantity originalQuantity_;
    Quantity remainingQuantity_;

    OrderId id_;

    Timestamp timestamp_;
};

#endif // HPC_QUEUE_SRC_ORDER_H
