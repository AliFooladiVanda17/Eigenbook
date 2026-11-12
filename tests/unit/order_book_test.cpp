//
// Behaviour of BasicOrderBook, run against both ladder strategies.
//

#include <cstdint>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace eigenbook {

namespace {

template <typename Book>
class OrderBookTest : public ::testing::Test {
protected:
    std::unique_ptr<Book> book = BookFactory<Book>::make();
};

using BookTypes = ::testing::Types<OrderBook, DenseOrderBook>;
TYPED_TEST_SUITE(OrderBookTest, BookTypes);

} // namespace

// ------------------------------------------------------------------------------------------------
// Resting / best prices
// ------------------------------------------------------------------------------------------------

TYPED_TEST(OrderBookTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(this->book->bestBid().has_value());
    EXPECT_FALSE(this->book->bestAsk().has_value());
    EXPECT_TRUE(this->book->empty());
    EXPECT_EQ(this->book->instrument(), kInstrument);
}

TYPED_TEST(OrderBookTest, NonCrossingLimitsRestAndSetBestPrices) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 99, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 5)));
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::SELL, 102, 7)));
    const auto res = book.addOrder(limit(4, OrderSide::SELL, 101, 3));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->filled, Quantity{0});
    EXPECT_EQ(res->resting, Quantity{3});

    EXPECT_EQ(book.bestBid(), Price{100});
    EXPECT_EQ(book.bestAsk(), Price{101});
    EXPECT_EQ(book.orderCount(), 4u);
    EXPECT_EQ(book.bids().levelCount(), 2u);
    EXPECT_EQ(book.asks().levelCount(), 2u);
}

TYPED_TEST(OrderBookTest, LevelTracksAggregateQuantityAndFifo) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 20)));
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::BUY, 100, 30)));

    const PriceLevel* level = book.bids().findLevel({100});
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->quantity(), Quantity{60});
    EXPECT_EQ(level->orderCount(), 3u);
    EXPECT_EQ(queueAt(book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{1, 2, 3}));
}

TYPED_TEST(OrderBookTest, ForEachLevelVisitsBestToWorst) {
    auto& book = *this->book;
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(book.addOrder(limit(10 + i, OrderSide::BUY, 90 + 2 * i, 1)));
        ASSERT_TRUE(book.addOrder(limit(20 + i, OrderSide::SELL, 200 - 3 * i, 1)));
    }
    std::vector<std::int64_t> bidPrices, askPrices;
    book.bids().forEachLevel([&](const PriceLevel& l) { bidPrices.push_back(l.price().value); });
    book.asks().forEachLevel([&](const PriceLevel& l) { askPrices.push_back(l.price().value); });
    EXPECT_EQ(bidPrices, (std::vector<std::int64_t>{98, 96, 94, 92, 90}));
    EXPECT_EQ(askPrices, (std::vector<std::int64_t>{188, 191, 194, 197, 200}));
}

// ------------------------------------------------------------------------------------------------
// Matching
// ------------------------------------------------------------------------------------------------

TYPED_TEST(OrderBookTest, LimitDoesNotTradeWhenNotCrossing) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 101, 10)));
    FillLog log;
    const auto res = book.addOrder(limit(2, OrderSide::BUY, 100, 10), log);
    ASSERT_TRUE(res);
    EXPECT_TRUE(log.fills.empty());
    EXPECT_EQ(res->resting, Quantity{10});
}

TYPED_TEST(OrderBookTest, ExactFillRemovesMakerAndLevel) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 10)));
    FillLog log;
    const auto res = book.addOrder(limit(2, OrderSide::BUY, 100, 10), log);
    ASSERT_TRUE(res);
    EXPECT_TRUE(res->fullyFilled());
    EXPECT_EQ(res->filled, Quantity{10});

    ASSERT_EQ(log.fills.size(), 1u);
    EXPECT_EQ(log.fills[0].makerId, OrderId{1});
    EXPECT_EQ(log.fills[0].takerId, OrderId{2});
    EXPECT_EQ(log.fills[0].price, Price{100});
    EXPECT_EQ(log.fills[0].quantity, Quantity{10});
    EXPECT_EQ(log.fills[0].takerSide, OrderSide::BUY);
    EXPECT_TRUE(log.fills[0].makerFilled);

    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_EQ(book.asks().levelCount(), 0u);
    EXPECT_EQ(book.findOrder({1}), nullptr);
}

TYPED_TEST(OrderBookTest, PartialFillOfMakerKeepsItAtFrontWithReducedQuantity) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 100, 5)));
    FillLog log;
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::BUY, 100, 4), log));

    ASSERT_EQ(log.fills.size(), 1u);
    EXPECT_FALSE(log.fills[0].makerFilled);
    const Order* maker = book.findOrder({1});
    ASSERT_NE(maker, nullptr);
    EXPECT_EQ(maker->getQuantity(), Quantity{6});
    EXPECT_EQ(book.asks().findLevel({100})->quantity(), Quantity{11});
    EXPECT_EQ(queueAt(book, OrderSide::SELL, 100), (std::vector<std::uint64_t>{1, 2}));
}

TYPED_TEST(OrderBookTest, PartialFillOfTakerRestsRemainder) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 3)));
    const auto res = book.addOrder(limit(2, OrderSide::SELL, 99, 10));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->filled, Quantity{3});
    EXPECT_EQ(res->resting, Quantity{7});
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_EQ(book.bestAsk(), Price{99});
    EXPECT_EQ(book.findOrder({2})->getQuantity(), Quantity{7});
}

TYPED_TEST(OrderBookTest, SweepFollowsPriceThenTimePriorityAtMakerPrices) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 102, 5)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 101, 5)));
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::SELL, 101, 5)));
    ASSERT_TRUE(book.addOrder(limit(4, OrderSide::SELL, 103, 5)));
    ASSERT_TRUE(book.addOrder(limit(5, OrderSide::SELL, 100, 5)));

    FillLog log;
    const auto res = book.addOrder(limit(10, OrderSide::BUY, 102, 18), log);
    ASSERT_TRUE(res);

    std::vector<std::pair<std::uint64_t, std::int64_t>> got;
    for (const Fill& f : log.fills) {
        got.emplace_back(f.makerId.value, f.price.value);
    }
    EXPECT_EQ(got,
              (std::vector<std::pair<std::uint64_t, std::int64_t>>{
                  {5, 100}, {2, 101}, {3, 101}, {1, 102}}));
    EXPECT_EQ(log.fills.back().quantity, Quantity{3});
    EXPECT_FALSE(log.fills.back().makerFilled);
    EXPECT_EQ(res->filled, Quantity{18});
    EXPECT_TRUE(res->fullyFilled());
    EXPECT_EQ(book.bestAsk(), Price{102});
    EXPECT_EQ(book.asks().levelCount(), 2u);
}

TYPED_TEST(OrderBookTest, SweepStopsAtLimitAndRestsRemainder) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 5)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 99, 5)));
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::BUY, 98, 5)));
    const auto res = book.addOrder(limit(4, OrderSide::SELL, 99, 20));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->filled, Quantity{10});
    EXPECT_EQ(res->resting, Quantity{10});
    EXPECT_EQ(book.bestBid(), Price{98});
    EXPECT_EQ(book.bestAsk(), Price{99});
}

TYPED_TEST(OrderBookTest, MarketOrderSweepsAndCancelsRemainder) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 5)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 500, 5)));
    FillLog log;
    const auto res = book.addOrder(market(3, OrderSide::BUY, 12), log);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->filled, Quantity{10});
    EXPECT_EQ(res->cancelled, Quantity{2});
    EXPECT_EQ(res->resting, Quantity{0});
    EXPECT_EQ(log.fills.size(), 2u);
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.findOrder({3}), nullptr);
}

TYPED_TEST(OrderBookTest, MarketOrderOnEmptyBookIsCancelled) {
    const auto res = this->book->addOrder(market(1, OrderSide::SELL, 7));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->cancelled, Quantity{7});
    EXPECT_TRUE(this->book->empty());
}

TYPED_TEST(OrderBookTest, RejectsInvalidRequests) {
    auto& book = *this->book;
    EXPECT_EQ(book.addOrder(limit(1, OrderSide::BUY, 100, 0)).error(),
              RejectReason::INVALID_QUANTITY);
    EXPECT_EQ(book.addOrder(market(1, OrderSide::BUY, 0)).error(), RejectReason::INVALID_QUANTITY);
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 1)));
    EXPECT_EQ(book.addOrder(limit(1, OrderSide::SELL, 200, 1)).error(),
              RejectReason::DUPLICATE_ORDER_ID);
    EXPECT_EQ(book.orderCount(), 1u);
}

TYPED_TEST(OrderBookTest, DuplicateIdIsRejectedBeforeAnyMatching) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 5)));
    FillLog log;
    // Would cross the resting bid, but reuses its id: nothing may trade.
    EXPECT_EQ(book.addOrder(limit(1, OrderSide::SELL, 100, 5), log).error(),
              RejectReason::DUPLICATE_ORDER_ID);
    EXPECT_TRUE(log.fills.empty());
    EXPECT_EQ(book.findOrder({1})->getQuantity(), Quantity{5});
    EXPECT_EQ(book.bestBid(), Price{100});
}

TYPED_TEST(OrderBookTest, FilledOrderIdCanBeReused) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 1)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 1)));
    EXPECT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 1)));
}

TYPED_TEST(OrderBookTest, CancelledOrderIdCanBeReused) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 1)));
    ASSERT_TRUE(book.cancelOrder({1}));
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 105, 2)));
    EXPECT_EQ(book.findOrder({1})->getSide(), OrderSide::SELL);
    EXPECT_FALSE(book.bestBid().has_value());
}

TYPED_TEST(OrderBookTest, ExecutionResultAccountsForWholeQuantity) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 4)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 101, 4)));

    const auto lim = book.addOrder(limit(3, OrderSide::BUY, 100, 10));
    ASSERT_TRUE(lim);
    EXPECT_EQ(lim->filled + lim->resting + lim->cancelled, Quantity{10});

    const auto mkt = book.addOrder(market(4, OrderSide::BUY, 10));
    ASSERT_TRUE(mkt);
    EXPECT_EQ(mkt->filled + mkt->resting + mkt->cancelled, Quantity{10});
    EXPECT_EQ(mkt->filled, Quantity{4});
}

// ------------------------------------------------------------------------------------------------
// Cancel / modify
// ------------------------------------------------------------------------------------------------

TYPED_TEST(OrderBookTest, CancelRemovesOrderAndEmptyLevel) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 20)));
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::BUY, 99, 30)));

    EXPECT_EQ(book.cancelOrder({1}).value(), Quantity{10});
    EXPECT_EQ(queueAt(book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{2}));
    EXPECT_EQ(book.bids().findLevel({100})->quantity(), Quantity{20});

    EXPECT_EQ(book.cancelOrder({2}).value(), Quantity{20});
    EXPECT_EQ(book.bids().findLevel({100}), nullptr);
    EXPECT_EQ(book.bestBid(), Price{99});

    EXPECT_EQ(book.cancelOrder({2}).error(), RejectReason::UNKNOWN_ORDER_ID);
    EXPECT_EQ(book.cancelOrder({42}).error(), RejectReason::UNKNOWN_ORDER_ID);
    EXPECT_EQ(book.orderCount(), 1u);
}

TYPED_TEST(OrderBookTest, CancelFromMiddleAndBackOfQueue) {
    auto& book = *this->book;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(book.addOrder(limit(i, OrderSide::SELL, 100, i)));
    }
    ASSERT_TRUE(book.cancelOrder({2}));
    ASSERT_TRUE(book.cancelOrder({4}));
    EXPECT_EQ(queueAt(book, OrderSide::SELL, 100), (std::vector<std::uint64_t>{1, 3}));
    EXPECT_EQ(book.asks().findLevel({100})->back()->getId(), OrderId{3});
    EXPECT_EQ(book.asks().findLevel({100})->quantity(), Quantity{4});
}

TYPED_TEST(OrderBookTest, CancelReturnsOnlyTheOpenQuantity) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 3)));
    EXPECT_EQ(book.cancelOrder({1}).value(), Quantity{7});
    EXPECT_TRUE(book.empty());
}

TYPED_TEST(OrderBookTest, ModifyDownKeepsTimePriority) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 10)));
    const auto res = book.modifyOrder({.id = {1}, .price = {100}, .quantity = {4}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->resting, Quantity{4});
    EXPECT_EQ(queueAt(book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(book.bids().findLevel({100})->quantity(), Quantity{14});
}

TYPED_TEST(OrderBookTest, ModifyToSameQuantityKeepsTimePriority) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 100, 10)));
    ASSERT_TRUE(book.modifyOrder({.id = {1}, .price = {100}, .quantity = {10}}));
    EXPECT_EQ(queueAt(book, OrderSide::SELL, 100), (std::vector<std::uint64_t>{1, 2}));
}

TYPED_TEST(OrderBookTest, ModifyUpLosesTimePriority) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 10)));
    ASSERT_TRUE(book.modifyOrder({.id = {1}, .price = {100}, .quantity = {15}}));
    EXPECT_EQ(queueAt(book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{2, 1}));
    EXPECT_EQ(book.bids().findLevel({100})->quantity(), Quantity{25});
}

TYPED_TEST(OrderBookTest, ModifyPriceMovesLevelAndCanTrade) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::SELL, 105, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 100, 4)));

    ASSERT_TRUE(book.modifyOrder({.id = {1}, .price = {103}, .quantity = {10}}));
    EXPECT_EQ(book.bestAsk(), Price{103});
    EXPECT_EQ(book.asks().findLevel({105}), nullptr);

    FillLog log;
    const auto res = book.modifyOrder({.id = {1}, .price = {100}, .quantity = {10}}, log);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->filled, Quantity{4});
    EXPECT_EQ(res->resting, Quantity{6});
    ASSERT_EQ(log.fills.size(), 1u);
    EXPECT_EQ(log.fills[0].makerId, OrderId{2});
    EXPECT_EQ(log.fills[0].takerId, OrderId{1});
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_EQ(book.bestAsk(), Price{100});
}

TYPED_TEST(OrderBookTest, ModifyThatFullyFillsLeavesNoRestingOrder) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 10)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 101, 6)));
    const auto res = book.modifyOrder({.id = {2}, .price = {100}, .quantity = {6}});
    ASSERT_TRUE(res);
    EXPECT_TRUE(res->fullyFilled());
    EXPECT_EQ(book.findOrder({2}), nullptr);
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_EQ(book.findOrder({1})->getQuantity(), Quantity{4});
}

TYPED_TEST(OrderBookTest, ModifyRejectsUnknownAndZero) {
    auto& book = *this->book;
    EXPECT_EQ(book.modifyOrder({.id = {9}, .price = {1}, .quantity = {1}}).error(),
              RejectReason::UNKNOWN_ORDER_ID);
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 10)));
    EXPECT_EQ(book.modifyOrder({.id = {1}, .price = {100}, .quantity = {0}}).error(),
              RejectReason::INVALID_QUANTITY);
    EXPECT_EQ(book.findOrder({1})->getQuantity(), Quantity{10});
}

// ------------------------------------------------------------------------------------------------
// Dense ladder specifics
// ------------------------------------------------------------------------------------------------

TEST(DenseOrderBook, RejectsPricesOutsideTickRange) {
    DenseOrderBook book(kInstrument, {.minPrice = {1000}, .tickCount = 100});
    EXPECT_EQ(book.addOrder(limit(1, OrderSide::BUY, 999, 1)).error(),
              RejectReason::PRICE_OUT_OF_RANGE);
    EXPECT_EQ(book.addOrder(limit(2, OrderSide::SELL, 1100, 1)).error(),
              RejectReason::PRICE_OUT_OF_RANGE);
    EXPECT_TRUE(book.addOrder(limit(3, OrderSide::BUY, 1000, 1)));
    EXPECT_TRUE(book.addOrder(limit(4, OrderSide::SELL, 1099, 1)));
    // Market orders carry no price and are always accepted.
    EXPECT_TRUE(book.addOrder(market(5, OrderSide::BUY, 1)));

    // A rejected reprice leaves the order untouched.
    EXPECT_EQ(book.modifyOrder({.id = {3}, .price = {-5}, .quantity = {1}}).error(),
              RejectReason::PRICE_OUT_OF_RANGE);
    EXPECT_EQ(book.findOrder({3})->getPrice(), Price{1000});
}

TEST(DenseOrderBook, BestPriceRecoversAcrossWideGaps) {
    DenseOrderBook book(kInstrument, {.minPrice = {0}, .tickCount = 1 << 20});
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 3, 1)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 900'000, 1)));
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::SELL, 900'001, 1)));
    ASSERT_TRUE(book.addOrder(limit(4, OrderSide::SELL, (1 << 20) - 1, 1)));
    ASSERT_TRUE(book.cancelOrder({2}));
    ASSERT_TRUE(book.cancelOrder({3}));
    EXPECT_EQ(book.bestBid(), Price{3});
    EXPECT_EQ(book.bestAsk(), Price{(1 << 20) - 1});
    ASSERT_TRUE(book.cancelOrder({1}));
    ASSERT_TRUE(book.cancelOrder({4}));
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value());
}

TEST(OrderBook, AcceptsNegativeAndVeryLargePrices) {
    OrderBook book(kInstrument);
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, -1'000'000, 1)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::SELL, 1'000'000'000'000, 1)));
    EXPECT_EQ(book.bestBid(), Price{-1'000'000});
    EXPECT_EQ(book.bestAsk(), Price{1'000'000'000'000});
}

} // namespace eigenbook
