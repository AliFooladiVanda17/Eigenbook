//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_BOOK_PRICELEVEL_H
#define HPC_QUEUE_SRC_BOOK_PRICELEVEL_H

#include <list>
#include <tradingengine/order.h>

class PriceLevel {
public:
    using OrderList = std::list<Order>;
    using Iter = std::list<Order>::iterator;

    Iter addOrder(const Order& order);

    void removeFrontOrder();

    decltype(auto) frontOrder(this auto&& self) {
        return self.orders.front();
    }

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::size_t orderCount() const noexcept;

    [[nodiscard]] Quantity quantity() const noexcept;

    void removeAt(const Iter&);

    PriceLevel() = default;

    PriceLevel(const PriceLevel& newLevel) = default;
    PriceLevel& operator=(const PriceLevel& newLevel) = default;

    PriceLevel(PriceLevel&& newLevel) noexcept = default;
    PriceLevel& operator=(PriceLevel&& newLevel) noexcept = default;

private:
    OrderList orders;
    Quantity totalQuantity{0};
};

#endif // HPC_QUEUE_SRC_BOOK_PRICELEVEL_H
