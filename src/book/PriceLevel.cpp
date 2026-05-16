//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/PriceLevel.h"

PriceLevel::IterOrder PriceLevel::addOrder(const Order &order) {
    totalQuantity += order.remaining();
    orders.push_back(order);

    return std::prev(orders.end());
}

void PriceLevel::removeFrontOrder() {
    auto orderUnderRemoval = orders.front();
    totalQuantity -= orderUnderRemoval.remaining();

    orders.pop_front();
}

bool PriceLevel::empty() const noexcept {
    return orders.empty();
}

std::size_t PriceLevel::orderCount() const noexcept {
    return orders.size();
}

Quantity PriceLevel::quantity() const noexcept {
    return totalQuantity;
}

void PriceLevel::removeAt(const PriceLevel::IterOrder &iter) {

    if (iter == orders.end()) return;
    auto order = *iter;
    totalQuantity -= order.remaining();

    orders.erase(iter);
}

void PriceLevel::updateOrder(PriceLevel::IterOrder &iter, const Order &order) {
    if (iter != orders.end()) *iter = order;
}
