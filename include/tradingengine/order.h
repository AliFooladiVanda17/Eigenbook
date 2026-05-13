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

    unsigned long id;

public:

    Order() = default;
    // Copy
    Order(const Order& other) = default;
    Order& operator=(const Order& other) = default;

    // Move
    Order(Order&& other) noexcept = default;
    Order& operator=(Order&& other) =default;

    ~Order() = default;

    [[nodiscard]] OrderType getType() const { return type; }

    void setType(OrderType Type) { type = Type; }

    [[nodiscard]] OrderSide getSide() const { return side; }

    void setSide(OrderSide Side) { side = Side; }

    [[nodiscard]] unsigned long getPrice() const { return price; }

    void setPrice(unsigned long Price) { price = Price; }

    [[nodiscard]] unsigned long getQuantity() const { return quantity; }

    void setQuantity(unsigned long Quantity) { quantity = Quantity; }

    [[nodiscard]] unsigned long getId() const { return id; }

    void setId(unsigned long Id) { id = Id; }
};

#endif // HPC_QUEUE_SRC_ORDER_H
