//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_ORDER_H
#define HPC_QUEUE_SRC_ORDER_H

#include<expected>
#include <tradingengine/types.h>

enum class returnCodes{
    NOT_ENOUGH_QUANTITY = -2,
};


class Order {
private:
    OrderType type;
    OrderSide side;

    Price price;
    Quantity quantity;

    OrderId id;

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

    [[nodiscard]] Price getPrice() const { return price; }

    void setPrice(Price iPrice) { price = iPrice; }

    [[nodiscard]] Quantity getQuantity() const { return quantity; }

    void setQuantity(Quantity iQuantity) { quantity = iQuantity; }

    [[nodiscard]] OrderId getId() const { return id; }

    void setId(OrderId iId) { id = iId; }

    Quantity subtract(Quantity decQuantity);

    [[nodiscard]] bool isFilled()const {return (quantity.value == 0);}
};

#endif // HPC_QUEUE_SRC_ORDER_H
