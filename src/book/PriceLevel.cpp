//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/PriceLevel.h"

void PriceLevel::addOrder(const Order& order) {
    totalQuantity += order.getQuantity();
    orders.push_back(order);
}

void PriceLevel::addOrder(Order&& order) {
    totalQuantity += order.getQuantity();
    orders.push_back(std::move(order));
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

unsigned long PriceLevel::quantity() const noexcept {
    return totalQuantity;
}
