//
// Time in force and order qualifiers: IOC and FOK never rest, FOK and minimum quantity either
// meet their condition or leave the book untouched, post-only never takes liquidity, invalid
// combinations are rejected before anything else, and a re-entering modify keeps the qualifiers.
//

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

OrderRequest withTif(OrderRequest request, const TimeInForce tif) {
    request.timeInForce = tif;
    return request;
}

OrderRequest postOnly(OrderRequest request) {
    request.postOnly = true;
    return request;
}

OrderRequest withMin(OrderRequest request, const std::uint64_t minQuantity) {
    request.minQuantity = {minQuantity};
    return request;
}

template <typename Book>
class TimeInForceTest : public ::testing::Test {
protected:
    // Asks: 100 x 3 (id 1), 101 x 4 (id 2), 103 x 5 (id 3).
    void SetUp() override {
        ASSERT_TRUE(book->addOrder(limit(1, OrderSide::SELL, 100, 3)));
        ASSERT_TRUE(book->addOrder(limit(2, OrderSide::SELL, 101, 4)));
        ASSERT_TRUE(book->addOrder(limit(3, OrderSide::SELL, 103, 5)));
    }

    void expectAsksUntouched() {
        EXPECT_EQ(book->asks().levelCount(), 3u);
        EXPECT_EQ(book->findOrder({1})->getQuantity(), Quantity{3});
        EXPECT_EQ(book->findOrder({2})->getQuantity(), Quantity{4});
        EXPECT_EQ(book->findOrder({3})->getQuantity(), Quantity{5});
        EXPECT_EQ(book->lastTradeId(), TradeId{0});
    }

    std::unique_ptr<Book> book = BookFactory<Book>::make();
};

TYPED_TEST_SUITE(TimeInForceTest, AllBookTypes);

} // namespace

TYPED_TEST(TimeInForceTest, DefaultIsDayAndRests) {
    ASSERT_TRUE(this->book->addOrder(limit(10, OrderSide::BUY, 90, 1)));
    const Order* order = this->book->findOrder({10});
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->getTimeInForce(), TimeInForce::DAY);
    EXPECT_FALSE(order->isPostOnly());
    ASSERT_TRUE(this->book->addOrder(withTif(limit(11, OrderSide::BUY, 90, 1), TimeInForce::GTC)));
    EXPECT_EQ(this->book->findOrder({11})->getTimeInForce(), TimeInForce::GTC);
}

TYPED_TEST(TimeInForceTest, IocTradesWhatCrossesAndCancelsTheRest) {
    FillLog log;
    const auto r =
        this->book->addOrder(withTif(limit(10, OrderSide::BUY, 101, 10), TimeInForce::IOC), log);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->filled, Quantity{7});
    EXPECT_EQ(r->resting, Quantity{0});
    EXPECT_EQ(r->cancelled, Quantity{3});
    EXPECT_EQ(log.fills.size(), 2u);
    EXPECT_EQ(this->book->findOrder({10}), nullptr);
    EXPECT_EQ(this->book->bestBid(), std::nullopt);
    // The id did not rest, so it is free again.
    EXPECT_TRUE(this->book->addOrder(limit(10, OrderSide::BUY, 90, 1)));
}

TYPED_TEST(TimeInForceTest, IocThatCannotTradeIsCancelledWhole) {
    const auto r =
        this->book->addOrder(withTif(limit(10, OrderSide::BUY, 99, 10), TimeInForce::IOC));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->filled, Quantity{0});
    EXPECT_EQ(r->cancelled, Quantity{10});
    this->expectAsksUntouched();
}

TYPED_TEST(TimeInForceTest, FokThatCannotFillCompletelyLeavesTheBookUntouched) {
    FillLog log;
    // 7 available up to 101; 8 wanted.
    const auto r =
        this->book->addOrder(withTif(limit(10, OrderSide::BUY, 101, 8), TimeInForce::FOK), log);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->filled, Quantity{0});
    EXPECT_EQ(r->cancelled, Quantity{8});
    EXPECT_TRUE(log.fills.empty());
    this->expectAsksUntouched();
    // The worse level at 103 does not count towards a limit of 101.
    EXPECT_EQ(this->book->addOrder(withTif(limit(11, OrderSide::BUY, 102, 12), TimeInForce::FOK))
                  ->cancelled,
              Quantity{12});
    this->expectAsksUntouched();
}

TYPED_TEST(TimeInForceTest, FokThatCanFillDoesSoAcrossLevels) {
    FillLog log;
    const auto r =
        this->book->addOrder(withTif(limit(10, OrderSide::BUY, 103, 12), TimeInForce::FOK), log);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->filled, Quantity{12});
    EXPECT_TRUE(r->fullyFilled());
    EXPECT_EQ(log.fills.size(), 3u);
    EXPECT_TRUE(this->book->asks().empty());
    EXPECT_EQ(this->book->lastTradeId(), TradeId{3});
}

TYPED_TEST(TimeInForceTest, FokMarketOrder) {
    OrderRequest tooBig = withTif(market(10, OrderSide::BUY, 13), TimeInForce::FOK);
    EXPECT_EQ(this->book->addOrder(tooBig)->cancelled, Quantity{13});
    this->expectAsksUntouched();
    EXPECT_EQ(
        this->book->addOrder(withTif(market(11, OrderSide::BUY, 12), TimeInForce::FOK))->filled,
        Quantity{12});
    EXPECT_TRUE(this->book->asks().empty());
}

TYPED_TEST(TimeInForceTest, MinimumQuantityGatesTheImmediateExecution) {
    // Only 3 available at 100: a minimum of 4 fails and leaves everything as it was.
    const auto failed = this->book->addOrder(withMin(limit(10, OrderSide::BUY, 100, 6), 4));
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->cancelled, Quantity{6});
    this->expectAsksUntouched();
    EXPECT_EQ(this->book->findOrder({10}), nullptr);
    // A minimum of 3 is met: it trades 3 and the remainder rests (DAY).
    const auto met = this->book->addOrder(withMin(limit(11, OrderSide::BUY, 100, 6), 3));
    ASSERT_TRUE(met);
    EXPECT_EQ(met->filled, Quantity{3});
    EXPECT_EQ(met->resting, Quantity{3});
    EXPECT_EQ(this->book->bestBid(), Price{100});
    // With IOC the remainder is cancelled instead.
    const auto ioc = this->book->addOrder(
        withTif(withMin(limit(12, OrderSide::BUY, 101, 9), 4), TimeInForce::IOC));
    ASSERT_TRUE(ioc);
    EXPECT_EQ(ioc->filled, Quantity{4});
    EXPECT_EQ(ioc->cancelled, Quantity{5});
}

TYPED_TEST(TimeInForceTest, PostOnlyNeverTakesLiquidity) {
    FillLog log;
    EXPECT_EQ(this->book->addOrder(postOnly(limit(10, OrderSide::BUY, 100, 1)), log).error(),
              RejectReason::POST_ONLY_WOULD_TRADE);
    EXPECT_TRUE(log.fills.empty());
    this->expectAsksUntouched();
    ASSERT_TRUE(this->book->addOrder(postOnly(limit(10, OrderSide::BUY, 99, 1)), log));
    EXPECT_TRUE(this->book->findOrder({10})->isPostOnly());
    EXPECT_TRUE(log.fills.empty());
    // Selling at the resting bid would trade; one tick above joins the ask queue.
    EXPECT_EQ(this->book->addOrder(postOnly(limit(11, OrderSide::SELL, 99, 1))).error(),
              RejectReason::POST_ONLY_WOULD_TRADE);
    ASSERT_TRUE(this->book->addOrder(postOnly(limit(11, OrderSide::SELL, 100, 1))));
    EXPECT_EQ(queueAt(*this->book, OrderSide::SELL, 100), (std::vector<std::uint64_t>{1, 11}));
}

TYPED_TEST(TimeInForceTest, InvalidCombinationsAreRejectedFirst) {
    auto& book = *this->book;
    const auto invalid = [&](const OrderRequest& request) {
        const auto r = book.addOrder(request);
        return !r && r.error() == RejectReason::INVALID_QUALIFIERS;
    };
    EXPECT_TRUE(invalid(postOnly(market(10, OrderSide::BUY, 1))));
    EXPECT_TRUE(invalid(postOnly(withTif(limit(10, OrderSide::BUY, 90, 1), TimeInForce::IOC))));
    EXPECT_TRUE(invalid(postOnly(withTif(limit(10, OrderSide::BUY, 90, 1), TimeInForce::FOK))));
    EXPECT_TRUE(invalid(postOnly(withMin(limit(10, OrderSide::BUY, 90, 5), 1))));
    EXPECT_TRUE(invalid(withMin(limit(10, OrderSide::BUY, 90, 5), 6)));
    EXPECT_TRUE(invalid(withTif(limit(10, OrderSide::BUY, 90, 5), static_cast<TimeInForce>(9))));
    // Before the duplicate-id check: qualifiers are part of the request's own validity.
    EXPECT_TRUE(invalid(postOnly(market(1, OrderSide::BUY, 1))));
    this->expectAsksUntouched();
}

TYPED_TEST(TimeInForceTest, ReentryKeepsTimeInForceAndPostOnly) {
    auto& book = *this->book;
    ASSERT_TRUE(
        book.addOrder(postOnly(withTif(limit(10, OrderSide::BUY, 98, 2), TimeInForce::GTC))));
    // Moving it to a crossing price would take liquidity: rejected, unchanged.
    EXPECT_EQ(book.modifyOrder({.id = {10}, .price = {100}, .quantity = {2}}).error(),
              RejectReason::POST_ONLY_WOULD_TRADE);
    EXPECT_EQ(book.findOrder({10})->getPrice(), Price{98});
    this->expectAsksUntouched();
    // A non-crossing move re-enters and keeps both qualifiers.
    ASSERT_TRUE(book.modifyOrder({.id = {10}, .price = {99}, .quantity = {3}}));
    const Order* order = book.findOrder({10});
    EXPECT_EQ(order->getPrice(), Price{99});
    EXPECT_EQ(order->getTimeInForce(), TimeInForce::GTC);
    EXPECT_TRUE(order->isPostOnly());
}

TEST(TimeInForce, FailedFokReleasesAnIssuedHandle) {
    DenseOrderBookDirect book(kInstrument, {.minPrice = {0}, .tickCount = 1000});
    const OrderId handle = book.reserveOrderId().value();
    const auto r = book.addOrder({.id = handle,
                                  .side = OrderSide::BUY,
                                  .type = OrderType::LIMIT,
                                  .price = {100},
                                  .quantity = {5},
                                  .timeInForce = TimeInForce::FOK});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->cancelled, Quantity{5});
    EXPECT_FALSE(book.releaseOrderId(handle)); // already given back by the book
}

} // namespace eigenbook
