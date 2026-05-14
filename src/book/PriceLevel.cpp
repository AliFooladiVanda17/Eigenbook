//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/PriceLevel.h"

PriceLevel::Iter PriceLevel::addOrder(const Order& order) {
    totalQuantity += order.getQuantity();
    orders.push_back(order);

    return std::prev(orders.end());
}

void PriceLevel::removeFrontOrder() {
    auto orderUnderRemoval = orders.front();
    totalQuantity -= orderUnderRemoval.getQuantity();

    orders.pop_front();
}

Order& PriceLevel::frontOrder() {
    return orders.front();
}

const Order& PriceLevel::frontOrder() const {
    return orders.front();
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

void PriceLevel::removeAt(const PriceLevel::Iter& iter) {

    if(iter == orders.end()) return;
    auto order = *iter;
    totalQuantity -= order.getQuantity();

    orders.erase(iter);
}
