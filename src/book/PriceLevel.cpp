//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/PriceLevel.h"

PriceLevel::IterOrder PriceLevel::addOrder(const Order &order) {
    orders_.emplace_back(order);
    totalQuantity_ += order.remaining();

    return std::prev(orders_.end());
}

PriceLevel::IterOrder PriceLevel::addOrder(Order &&order) {
    orders_.emplace_back(std::move(order));
    totalQuantity_ += order.remaining();

    return std::prev(orders_.end());
}

Order& PriceLevel::frontOrder() noexcept
{
    return orders_.front();
}

const Order& PriceLevel::frontOrder() const noexcept
{
    return orders_.front();
}

void PriceLevel::consumeFront(Quantity traded)
{
    auto& order = orders_.front();

    totalQuantity_ -= order.fill(traded);

    if (order.isFilled()) {
        orders_.pop_front();
    }
}

void PriceLevel::consumeAt(PriceLevel::IterOrder it, Quantity traded)
{
    if (it == orders_.end()) return;

    // Precondition: traded <= it->remaining()
    totalQuantity_ -= it->fill(traded);

    if (it->isFilled()) {
        orders_.erase(it);
    }
}

PriceLevel::IterOrder PriceLevel::removeFrontOrder()
{
    return removeAt(orders_.begin());
}

PriceLevel::IterOrder PriceLevel::removeAt(const IterOrder& it)
{
    if (it == orders_.end()) {
        return it;
    }

    totalQuantity_ -= it->remaining();
    return orders_.erase(it);
}

bool PriceLevel::empty() const noexcept {
    return orders_.empty();
}

std::size_t PriceLevel::orderCount() const noexcept {
    return orders_.size();
}

Quantity PriceLevel::quantity() const noexcept {
    return totalQuantity_;
}

void PriceLevel::clear() noexcept
{
    orders_.clear();
    totalQuantity_ = Quantity{0};
}

