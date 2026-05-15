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
void PriceLadder<PriceCompare>::addOrder(const Order &order) {
    PriceLevel &orders = getOrCreateLevel(order.getPrice());
    auto priceIter = levels.find(order.getPrice());
    auto orderIter = orders.addOrder(order);

    index[order.getId()] = {order.getPrice(), priceIter, orderIter};
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::cancelOrder(OrderId iId) {
    const auto &[price, priceIter, orderIter] = index[iId];

    index.erase(iId);
    static_cast<IterPrice>(priceIter)->second.removeAt(orderIter);
    if (static_cast<IterPrice>(priceIter)->second.quantity() == 0) {
        levels.erase(priceIter);
    }
}

template<typename PriceCompare>
void PriceLadder<PriceCompare>::modifyOrder(OrderId iId, const Order &updated) {

    if(index.find(iId) == index.end()) return;

    auto orderLoc = index[iId];

    auto& orderIter = static_cast<OrderLocation>(orderLoc).orderIterator;
    auto& priceIter = static_cast<OrderLocation>(orderLoc).priceIterator;

    if(orderIter->getQuantity() < updated.getQuantity() ||
    orderIter->getPrice() != updated.getPrice() ||
    orderIter->getSide() != updated.getSide()){
        cancelOrder(iId);
        addOrder(updated);
    }else{
        priceIter->second.updateOrder(iId, updated);
    }
}

template<typename PriceCompare>
const PriceLevel *PriceLadder<PriceCompare>::getLevel(Price iPrice) const {
    auto it = levels.find(iPrice);

    return (it == levels.end()) ? nullptr : &(it->second);
}

template<typename PriceCompare>
PriceLadder<PriceCompare>::IterPrice PriceLadder<PriceCompare>::bestPriceIter() const {
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
PriceLadder<PriceCompare>::IterPrice PriceLadder<PriceCompare>::worstPriceIter() const {
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
        auto &orders = bPriceIter->second;

        auto &resting = orders.frontOrder();

        if (crossedPrice(incoming, resting)) break;

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

    if (!incoming.isFilled()) otherSide.addOrder(incoming);
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


template<typename PriceCompare>
bool PriceLadder<PriceCompare>::crossedPrice(Order incoming, Order resting) {
    return direction == OrderSide::BUY ?
           resting.getPrice() < incoming.getPrice() :
           resting.getPrice() > incoming.getPrice();
}