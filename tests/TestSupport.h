//
// Shared helpers for the TradingEngine test suite.
//

#ifndef TRADINGENGINE_TESTS_TESTSUPPORT_H
#define TRADINGENGINE_TESTS_TESTSUPPORT_H

#include <cstdint>
#include <memory>
#include <vector>

#include <tradingengine/orderbook.h>

inline constexpr InstrumentId kInstrument{7};

inline OrderRequest limit(std::uint64_t id, OrderSide side, std::int64_t price, std::uint64_t qty) {
    return {.id = {id},
            .side = side,
            .type = OrderType::LIMIT,
            .price = {price},
            .quantity = {qty},
            .timestamp = {id}};
}

inline OrderRequest market(std::uint64_t id, OrderSide side, std::uint64_t qty) {
    return {.id = {id}, .side = side, .type = OrderType::MARKET, .quantity = {qty}};
}

inline Order makeOrder(std::uint64_t id, OrderSide side, std::int64_t price, std::uint64_t qty) {
    return {OrderId{id}, side, OrderType::LIMIT, Price{price}, Quantity{qty}, Timestamp{id}};
}

struct FillLog {
    std::vector<Fill> fills;

    void operator()(const Fill& fill) { fills.push_back(fill); }
};

// Order ids at one price, in time priority.
template <typename Book>
std::vector<std::uint64_t> queueAt(const Book& book, OrderSide side, std::int64_t price) {
    std::vector<std::uint64_t> ids;
    const PriceLevel* level =
        side == OrderSide::BUY ? book.bids().findLevel({price}) : book.asks().findLevel({price});
    if (level != nullptr) {
        level->forEachOrder([&](const Order& order) { ids.push_back(order.getId().value); });
    }
    return ids;
}

// Both books over a dense range [0, 1000) so the same prices are valid for either ladder.
template <typename Book>
struct BookFactory;

template <>
struct BookFactory<OrderBook> {
    static std::unique_ptr<OrderBook> make() { return std::make_unique<OrderBook>(kInstrument); }
};

template <>
struct BookFactory<DenseOrderBook> {
    static std::unique_ptr<DenseOrderBook> make() {
        return std::make_unique<DenseOrderBook>(
            kInstrument, DenseOrderBook::LadderConfig{.minPrice = {0}, .tickCount = 1000});
    }
};

#endif // TRADINGENGINE_TESTS_TESTSUPPORT_H
