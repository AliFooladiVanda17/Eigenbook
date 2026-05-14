//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_BOOK_ORDERBOOK_H
#define HPC_QUEUE_SRC_BOOK_ORDERBOOK_H

#include<optional>
#include <tradingengine/PriceLadder.h>

class OrderBook {

public:
    OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook operator=(const OrderBook&) const =delete;

    OrderBook(const OrderBook&&) = delete;
    OrderBook& operator=(const OrderBook&&) = delete;

    void submit(Order order);
    bool cancel(OrderId id, OrderSide side, Quantity PrevQuantity);
    bool modify(OrderId id, Price newPrice, Quantity newQty, Quantity prevQty);

    [[nodiscard]] std::optional<Price> bestBid() const;
    [[nodiscard]] std::optional<Price> bestAsk() const;

    [[nodiscard]] std::optional<std::pair<Price, Price>> topOfBook() const;
    [[nodiscard]] std::optional<Price> spread() const;

    void clear();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

private:
    PriceLadder<std::greater<>> bids_;
    PriceLadder<std::less<>> asks_;

    void matchBuy(Order& incoming);
    void matchSell(Order& incoming);

private:
    PriceLadder<std::less<>> sellPriceLadder;
    PriceLadder<std::greater<>> buyPriceLadder;
};

#endif // HPC_QUEUE_SRC_BOOK_ORDERBOOK_H
