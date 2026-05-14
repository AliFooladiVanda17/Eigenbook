//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/OrderBook.h"

void OrderBook::submit(Order order) {
    if (order.getSide() == OrderSide::BUY) {
        matchBuy(order);
    } else {
        matchSell(order);
    }
}

bool OrderBook::cancel(OrderId id, OrderSide side, Quantity PrevQuantity) {

    return false;
}

bool OrderBook::modify(OrderId id, Price newPrice, Quantity newQty, Quantity prevQty) {
    return false;
}

std::optional<Price> OrderBook::bestBid() const {
    return std::optional<Price>();
}

std::optional<Price> OrderBook::bestAsk() const {
    return std::optional<Price>();
}

std::optional<std::pair<Price, Price>> OrderBook::topOfBook() const {
    return std::optional<std::pair<Price, Price>>();
}

std::optional<Price> OrderBook::spread() const {
    return std::optional<Price>();
}

void OrderBook::clear() {

}

std::size_t OrderBook::size() const {
    return 0;
}

bool OrderBook::empty() const {
    return false;
}

void OrderBook::matchBuy(Order &incoming) {

}

void OrderBook::matchSell(Order &incoming) {

}
