//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/PriceLadder.h"

template<typename PriceCompare>
PriceLevel &PriceLadder<PriceCompare>::getOrCreateLevel(Price iPrice) {
    return levels[iPrice];
}

template<typename PriceCompare>
bool PriceLadder<PriceCompare>::hasLevel(Price iPrice) const {
    return levels.contains(iPrice);
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::removeLevel(Price iPrice) {
    if (hasLevel(iPrice)) {
        levels.erase(iPrice);
    }
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::clear() {
    levels.clear();
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::addOrder(Price iPrice, const Order &order) {
    PriceLevel &orders = getOrCreateLevel(iPrice);

    auto it = orders.addOrder(order);

    index[order.getId()] = {iPrice, it};
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::cancelOrder(Price iPrice, OrderId iId) {
    PriceLevel &orders = getOrCreateLevel(iPrice);
    const auto &[indexPrice, indexIter] = index[iId];

    index.erase(iId);
    orders.removeAt(indexIter);
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::modifyOrder(Price iPrice,
                                            OrderId iId,
                                            const Order &updated) {
    cancelOrder(iPrice, iId);

    addOrder(updated.getPrice(), updated);
}

template<typename PriceCompare>
const PriceLevel *PriceLadder<PriceCompare>::getLevel(Price iPrice) const {
    auto it = levels.find(iPrice);

    return (it == levels.end()) ? nullptr : &(it->second);
}

template<typename PriceCompare>
std::map<Price, PriceLevel>::iterator PriceLadder<PriceCompare>::bestPriceIter() const {
    if (!levels.empty()) {
        return levels.begin();
    }
    return levels.end();
}

template<typename PriceCompare>
Price PriceLadder<PriceCompare>::bestPrice() const {
    if (!levels.empty()) {
        auto &[price, level] = *levels.begin();
        return price;
    }
    return {-1};
}

template<typename PriceCompare>
std::map<Price, PriceLevel>::iterator PriceLadder<PriceCompare>::worstPriceIter() const {
    if (!levels.empty()) {
        return levels.rbegin();
    }
    return levels.end();
}

template<typename PriceCompare>
Price PriceLadder<PriceCompare>::worstPrice() const {
    if (!levels.empty()) {
        auto &[price, level] = levels.rbegin();
        return price;
    }
    return {-1};
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::matchAgainst(PriceLadder &otherSide, Order &incoming) {
    auto bPriceIter = bestPriceIter();

    while (bPriceIter != levels.end() && !incoming.isFilled()) {
        auto& orders = bPriceIter->second;

        auto& resting = orders.frontOrder();

        if(crossedPrice(incoming, resting)) break;

        Quantity traded =
                std::min(incoming.getQuantity(),
                         resting.getQuantity());

        incoming.subtract(traded);
        resting.subtract(traded);

        if (resting.isFilled()) {
            orders.removeFrontOrder();
        }

        if (orders.empty()) {
            bPriceIter = levels.erase(bPriceIter);
        }
    }

    if(!incoming.isFilled()) otherSide.addOrder(incoming);
}

template<typename PriceCompare>
size_t PriceLadder<PriceCompare>::totalOrders() const {
    return 0;
}

template<typename PriceCompare>
size_t PriceLadder<PriceCompare>::totalVolume() const {
    return 0;
}

template<typename PriceCompare>
bool PriceLadder<PriceCompare>::empty() const {
    return levels.empty();
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::print() const {}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::dump() const {}

template<typename PriceCompare>
template<typename Callback>
void PriceLadder<PriceCompare>::forEachLevel([[maybe_unused]] Callback &&cb) const {}
