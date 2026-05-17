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
    using IterOrder = std::list<Order>::iterator;

    IterOrder addOrder(const Order &order);

    IterOrder addOrder(Order &&order);

    IterOrder removeFrontOrder();

    IterOrder removeAt(const IterOrder &);

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::size_t orderCount() const noexcept;

    [[nodiscard]] Quantity quantity() const noexcept;

    PriceLevel() = default;

    PriceLevel(const PriceLevel &newLevel) = default;
    PriceLevel &operator=(const PriceLevel &newLevel) = default;

    PriceLevel(PriceLevel &&newLevel) noexcept = default;
    PriceLevel &operator=(PriceLevel &&newLevel) noexcept = default;

    Order &frontOrder() noexcept;

    const Order &frontOrder() const noexcept;

    void consumeFront(Quantity traded);

    void consumeAt(IterOrder it, Quantity traded);

    void clear();

private:
    OrderList orders_;
    Quantity totalQuantity_{0};


};

#endif // HPC_QUEUE_SRC_BOOK_PRICELEVEL_H
