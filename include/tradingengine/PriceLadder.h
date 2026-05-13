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

template <typename C>
concept PriceCompare =
std::same_as<C, std::less<unsigned long>> ||
    std::same_as<C, std::greater<unsigned long>>;

template <typename PriceCompare = std::less<unsigned long>>
class PriceLadder {
private:
    using Iter = std::list<Order>::iterator;

    OrderSide direction;

    std::map<unsigned long, PriceLevel, PriceCompare> levels;
    std::unordered_map<unsigned int, std::pair<unsigned long, Iter>> index;

public:

    PriceLadder() = default;
    ~PriceLadder() = default;

    PriceLadder(const PriceLadder&) = default;
    PriceLadder& operator=(const PriceLadder&) = default;

    PriceLadder& operator=(PriceLadder&&) noexcept = default;
    PriceLadder(PriceLadder&&) noexcept = default;

    PriceLevel& getOrCreateLevel(unsigned long price);

    bool hasLevel(unsigned long price) const;

    void removeLevel(unsigned long price);

    void clear();

    void addOrder(unsigned long price, const Order& order);

    void cancelOrder(unsigned long price, unsigned int id);

    void modifyOrder(unsigned long price, unsigned int id, const Order& updated);

    [[nodiscard]] const PriceLevel* getLevel(unsigned long price) const;

    template <typename Callback>
    void forEachLevel(Callback&& cb) const;

    [[nodiscard]] unsigned long bestPrice() const;

    [[nodiscard]] unsigned long worstPrice() const;

    [[nodiscard]] unsigned long bestAsk() const { return levels.begin()->first; }

    [[nodiscard]] unsigned long bestBid() const { return levels.rbegin()->first; }

    void matchAgainst(PriceLadder& opposite, Order& incoming);

    [[nodiscard]] size_t totalOrders() const;

    [[nodiscard]] size_t totalVolume() const;

    bool empty() const;

    void print() const;

    void dump() const;
};

#endif // HPC_QUEUE_SRC_BOOK_PRICELADDER_H
