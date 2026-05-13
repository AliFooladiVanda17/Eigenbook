//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/PriceLadder.h"

template <typename PriceCompare>
PriceLevel& PriceLadder<PriceCompare>::getOrCreateLevel(unsigned long price) {
    return levels[price];
}

template <typename PriceCompare>
bool PriceLadder<PriceCompare>::hasLevel(unsigned long price) const {
    return levels.contains(price);
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::removeLevel(unsigned long price) {
    if (hasLevel(price)) {
        levels.erase(price);
    }
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::clear() {
    levels.clear();
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::addOrder(unsigned long price, const Order& order) {
    PriceLevel& orders = getOrCreateLevel(price);

    auto it = orders.addOrder(order);

    index[order.getId()] = {price, it};
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::cancelOrder(unsigned long price, unsigned int id) {
    PriceLevel& orders = getOrCreateLevel(price);
    const auto& [indexPrice, indexIter] = index[id];

    index.erase(id);
    orders.removeAt(indexIter);
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::modifyOrder(unsigned long price,
                                            unsigned int id,
                                            const Order& updated) {
    cancelOrder(price, id);

    addOrder(updated.getPrice(), updated);
}

template <typename PriceCompare>
const PriceLevel* PriceLadder<PriceCompare>::getLevel(unsigned long price) const {
    auto it = levels.find(price);

    return (it == levels.end()) ? nullptr : &(it->second);
}

template <typename PriceCompare>
unsigned long PriceLadder<PriceCompare>::bestPrice() const {
    if (!levels.empty()) {
        auto& [price, level] = *levels.begin();
        return price;
    }
    return -1;
}

template <typename PriceCompare>
unsigned long PriceLadder<PriceCompare>::worstPrice() const {
    if (!levels.empty()) {
        auto& [price, level]= levels.rbegin();
        return price;
    }
    return -1;
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::matchAgainst(PriceLadder& opposite, Order& incoming) {}

template <typename PriceCompare>
size_t PriceLadder<PriceCompare>::totalOrders() const {
    return 0;
}

template <typename PriceCompare>
size_t PriceLadder<PriceCompare>::totalVolume() const {
    return 0;
}

template <typename PriceCompare>
bool PriceLadder<PriceCompare>::empty() const {
    return levels.empty();
}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::print() const {}

template <typename PriceCompare>
void PriceLadder<PriceCompare>::dump() const {}

template <typename PriceCompare>
template <typename Callback>
void PriceLadder<PriceCompare>::forEachLevel(Callback&& cb) const {}
