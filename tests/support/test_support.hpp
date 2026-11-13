//
// Shared helpers for the Eigenbook test suite.
//

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <eigenbook/book/order_book.hpp>

namespace eigenbook {

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
    const PriceLevel* level = side == OrderSide::BUY ? book.bids().findLevel({price})
                                                     : book.asks().findLevel({price});
    if (level != nullptr) {
        level->forEachOrder([&](const Order& order) { ids.push_back(order.getId().value); });
    }
    return ids;
}

// Every book type with a configuration under which the prices used by the tests ([0, 1000)) are
// valid. The hybrid book gets a deliberately small window (64 ticks starting at 0) so that the
// tests also exercise its overflow maps and recentering.
template <typename Book>
struct BookFactory;

inline constexpr std::size_t kTestMaxLevels = 4096;

inline HybridLadderConfig smallWindowHybrid(const std::size_t maxLevels = kTestMaxLevels) {
    return {.windowTicks = 64,
            .initialBase = {0},
            .maxLevels = maxLevels,
            .maxShiftsPerOp = 64,
            .maxMovesPerOp = 8};
}

template <>
struct BookFactory<OrderBook> {
    static std::unique_ptr<OrderBook> make() { return std::make_unique<OrderBook>(kInstrument); }
};

template <>
struct BookFactory<OrderBookM1> {
    static std::unique_ptr<OrderBookM1> make() {
        return std::make_unique<OrderBookM1>(kInstrument,
                                             MapLadderConfig{.maxLevels = kTestMaxLevels});
    }
};

template <>
struct BookFactory<OrderBookM2> {
    static std::unique_ptr<OrderBookM2> make() {
        return std::make_unique<OrderBookM2>(kInstrument,
                                             MapLadderConfig{.maxLevels = kTestMaxLevels});
    }
};

template <>
struct BookFactory<DenseOrderBook> {
    static std::unique_ptr<DenseOrderBook> make() {
        return std::make_unique<DenseOrderBook>(
            kInstrument,
            DenseOrderBook::LadderConfig{.minPrice = {0}, .tickCount = 1000});
    }
};

template <>
struct BookFactory<HybridOrderBook> {
    static std::unique_ptr<HybridOrderBook> make() {
        return std::make_unique<HybridOrderBook>(kInstrument, smallWindowHybrid());
    }
};

// Client ids, keyed hash (random seed), fixed capacity.
template <>
struct BookFactory<DenseOrderBookHardened> {
    static std::unique_ptr<DenseOrderBookHardened> make() {
        return std::make_unique<DenseOrderBookHardened>(
            kInstrument,
            DenseOrderBookHardened::LadderConfig{.minPrice = {0}, .tickCount = 1000});
    }
};

// Every book that takes client-chosen ids. (DenseOrderBookDirect issues its own ids and has its
// own tests.)
using AllBookTypes = ::testing::Types<OrderBook,
                                      OrderBookM1,
                                      OrderBookM2,
                                      DenseOrderBook,
                                      HybridOrderBook,
                                      DenseOrderBookHardened>;

} // namespace eigenbook
