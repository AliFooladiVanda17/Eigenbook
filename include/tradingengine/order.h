//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_ORDER_H
#define HPC_QUEUE_SRC_ORDER_H

#include <tradingengine/types.h>

class Order {
private:
    OrderType type;
    OrderSide side;

    unsigned long price;
    unsigned long quantity;

public:
    // Copy constructor
    Order(const Order& other) = default;

    // Copy assignment
    Order& operator=(const Order& other) {
        if (this == &other)
            return *this;

        type = other.type;
        side = other.side;
        price = other.price;
        quantity = other.quantity;

        return *this;
    }

    // Move constructor
    Order(Order&& other) noexcept
        : type(other.type), side(other.side), price(other.price), quantity(other.quantity) {}

    // Move assignment
    Order& operator=(Order&& other) noexcept {
        if (this == &other)
            return *this;

        type = other.type;
        side = other.side;
        price = other.price;
        quantity = other.quantity;

        return *this;
    }

    ~Order() = default;

    [[nodiscard]] OrderType getType() const { return type; }
    void setType(OrderType Type) { type = Type; }

    [[nodiscard]] OrderSide getSide() const { return side; }
    void setSide(OrderSide Side) { side = Side; }

    [[nodiscard]] unsigned long getPrice() const { return price; }
    void setPrice(unsigned long Price) { price = Price; }

    [[nodiscard]] unsigned long getQuantity() const { return quantity; }
    void setQuantity(unsigned long Quantity) { quantity = Quantity; }
};

#endif // HPC_QUEUE_SRC_ORDER_H
