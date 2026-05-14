//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_BOOK_PRICELADDER_H
#define HPC_QUEUE_SRC_BOOK_PRICELADDER_H

#include <list>
#include <map>
#include <unordered_map>
#include <tradingengine/PriceLevel.h>
#include <tradingengine/order.h>

template<typename C>
concept PriceCompare =
std::same_as<C, std::less<Price>> ||
std::same_as<C, std::greater<Price>>;

template<typename PriceCompare = std::less<Price>>
class PriceLadder {
public:

    using IterOrder = std::list<Order>::iterator;
    using IterPrice = std::map<Price, PriceLevel, PriceCompare>::iterator;

    PriceLadder() = default;

    ~PriceLadder() = default;

    PriceLadder(const PriceLadder &) = default;

    PriceLadder &operator=(const PriceLadder &) = default;

    PriceLadder &operator=(PriceLadder &&) noexcept = default;

    PriceLadder(PriceLadder &&) noexcept = default;

    PriceLevel &getOrCreateLevel(Price iPrice);

    bool hasLevel(Price iPrice) const;

    void removeLevel(Price iPrice);

    void clear();

    void addOrder(const Order &order);

    void cancelOrder(OrderId iId);

    void modifyOrder(OrderId iId, const Order &updated);

    [[nodiscard]] const PriceLevel *getLevel(Price iPrice) const;

    template<typename Callback>
    void forEachLevel(Callback &&cb) const;

    [[nodiscard]] IterPrice bestPriceIter() const;

    [[nodiscard]] Price bestPrice() const;

    [[nodiscard]] IterPrice worstPriceIter() const;

    [[nodiscard]] Price worstPrice() const;

    void matchAgainst(PriceLadder &opposite, Order &incoming);

    [[nodiscard]] size_t totalOrders() const;

    [[nodiscard]] size_t totalVolume() const;

    [[nodiscard]] bool crossedPrice(Order ord1, Order ord2);

    bool empty() const;

    void print() const;

    void dump() const;

private:

    struct OrderLocation{
        Price price{0};

        IterPrice priceIterator;
        IterOrder orderIterator;
    };

    const OrderSide direction =
            std::is_same_v<PriceCompare, std::less<Price>>
            ? OrderSide::SELL
            : OrderSide::BUY;

    std::map<Price, PriceLevel, PriceCompare> levels;
    std::unordered_map<OrderId, OrderLocation> index;

};



#endif // HPC_QUEUE_SRC_BOOK_PRICELADDER_H
