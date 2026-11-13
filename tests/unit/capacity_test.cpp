//
// Bounded ladders: a new price level beyond maxLevels is a CAPACITY_EXCEEDED reject, checked before
// anything changes (add and modify), never an allocation or a lost order.
//

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

constexpr std::size_t kLevels = 4;

template <typename Book>
struct Bounded;

template <>
struct Bounded<OrderBook> {
    static std::unique_ptr<OrderBook> make() {
        return std::make_unique<OrderBook>(kInstrument, MapLadderConfig{.maxLevels = kLevels});
    }
};

template <>
struct Bounded<OrderBookM1> {
    static std::unique_ptr<OrderBookM1> make() {
        return std::make_unique<OrderBookM1>(kInstrument, MapLadderConfig{.maxLevels = kLevels});
    }
};

template <>
struct Bounded<OrderBookM2> {
    static std::unique_ptr<OrderBookM2> make() {
        return std::make_unique<OrderBookM2>(kInstrument, MapLadderConfig{.maxLevels = kLevels});
    }
};

template <>
struct Bounded<HybridOrderBook> {
    static std::unique_ptr<HybridOrderBook> make() {
        return std::make_unique<HybridOrderBook>(kInstrument, smallWindowHybrid(kLevels));
    }
};

template <typename Book>
class CapacityTest : public ::testing::Test {
protected:
    std::unique_ptr<Book> book = Bounded<Book>::make();

    // Bids at 100, 99, 98, 97: the bid side is full.
    void fillBids() {
        for (std::uint64_t i = 0; i < kLevels; ++i) {
            ASSERT_TRUE(book->addOrder(
                limit(1 + i, OrderSide::BUY, 100 - static_cast<std::int64_t>(i), 5)));
        }
        ASSERT_EQ(book->bids().levelCount(), kLevels);
    }
};

using BoundedBooks = ::testing::Types<OrderBook, OrderBookM1, OrderBookM2, HybridOrderBook>;
TYPED_TEST_SUITE(CapacityTest, BoundedBooks);

} // namespace

TYPED_TEST(CapacityTest, NewLevelBeyondCapacityIsRejected) {
    auto& book = *this->book;
    this->fillBids();
    EXPECT_EQ(book.addOrder(limit(50, OrderSide::BUY, 90, 1)).error(),
              RejectReason::CAPACITY_EXCEEDED);
    EXPECT_EQ(book.orderCount(), kLevels);
    EXPECT_EQ(book.findOrder({50}), nullptr);
    // An existing level still takes orders, and the other side has its own capacity.
    EXPECT_TRUE(book.addOrder(limit(51, OrderSide::BUY, 98, 1)));
    EXPECT_TRUE(book.addOrder(limit(52, OrderSide::SELL, 200, 1)));
    // Market orders never rest, so they are never capacity-limited.
    EXPECT_TRUE(book.addOrder(market(53, OrderSide::BUY, 1)));
}

TYPED_TEST(CapacityTest, RejectHappensBeforeAnyMatching) {
    auto& book = *this->book;
    this->fillBids();
    ASSERT_TRUE(book.addOrder(limit(20, OrderSide::SELL, 150, 3)));
    // A buy at a new price that would trade with the ask: rejected whole, the ask is untouched.
    FillLog fills;
    EXPECT_EQ(book.addOrder(limit(21, OrderSide::BUY, 160, 1), fills).error(),
              RejectReason::CAPACITY_EXCEEDED);
    EXPECT_TRUE(fills.fills.empty());
    EXPECT_EQ(book.findOrder({20})->getQuantity(), Quantity{3});
}

TYPED_TEST(CapacityTest, FreedLevelCanBeReused) {
    auto& book = *this->book;
    this->fillBids();
    ASSERT_TRUE(book.cancelOrder({4})); // level 97 empties
    EXPECT_TRUE(book.addOrder(limit(60, OrderSide::BUY, 80, 1)));
    EXPECT_EQ(book.bids().levelCount(), kLevels);
}

TYPED_TEST(CapacityTest, ModifyToNewLevelWhenFullIsRejectedAndLeavesTheOrderAlone) {
    auto& book = *this->book;
    this->fillBids();
    ASSERT_TRUE(book.addOrder(limit(10, OrderSide::BUY, 100, 7))); // second order at 100
    EXPECT_EQ(book.modifyOrder({.id = {10}, .price = {90}, .quantity = {7}}).error(),
              RejectReason::CAPACITY_EXCEEDED);
    const Order* order = book.findOrder({10});
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->getPrice(), Price{100});
    EXPECT_EQ(order->getQuantity(), Quantity{7});
    EXPECT_EQ(queueAt(book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{1, 10})); // priority
                                                                                        // kept
}

TYPED_TEST(CapacityTest, ModifyOfALoneOrderToANewLevelFitsBecauseItFreesItsOwn) {
    auto& book = *this->book;
    this->fillBids();
    ASSERT_TRUE(book.modifyOrder({.id = {4}, .price = {90}, .quantity = {5}})); // 97 -> 90
    EXPECT_EQ(book.findOrder({4})->getPrice(), Price{90});
    EXPECT_EQ(book.bids().findLevel({97}), nullptr);
    EXPECT_EQ(book.bids().levelCount(), kLevels);
}

TEST(Capacity, MapPoliciesNeedABound) {
    EXPECT_THROW((OrderBookM1(kInstrument, MapLadderConfig{})), std::invalid_argument);
    EXPECT_THROW((OrderBookM2(kInstrument, MapLadderConfig{})), std::invalid_argument);
    EXPECT_NO_THROW((OrderBook(kInstrument, MapLadderConfig{}))); // M0 stays unbounded by default
}

TEST(Capacity, BoundedMapsFillToExactlyMaxLevels) {
    // Construction pre-allocates (and for M1 pre-warms) for maxLevels; filling every level must
    // then work without the arena or the block list running out.
    for (const std::size_t levels : {1u, 7u, 100u, 5000u}) {
        OrderBookM1 m1(kInstrument, MapLadderConfig{.maxLevels = levels}, levels * 2);
        OrderBookM2 m2(kInstrument, MapLadderConfig{.maxLevels = levels}, levels * 2);
        for (std::uint64_t i = 0; i < levels; ++i) {
            ASSERT_TRUE(m1.addOrder(limit(1 + i, OrderSide::BUY, static_cast<std::int64_t>(i), 1)));
            ASSERT_TRUE(m2.addOrder(
                limit(1 + i, OrderSide::SELL, 1'000'000 + static_cast<std::int64_t>(i), 1)));
        }
        EXPECT_EQ(m1.bids().levelCount(), levels);
        EXPECT_EQ(m2.asks().levelCount(), levels);
        EXPECT_EQ(m1.addOrder(limit(999'999, OrderSide::BUY, -1, 1)).error(),
                  RejectReason::CAPACITY_EXCEEDED);
        EXPECT_EQ(m2.addOrder(limit(999'999, OrderSide::SELL, -1, 1)).error(),
                  RejectReason::CAPACITY_EXCEEDED);
        EXPECT_EQ(m2.asks().nodePolicy().blockResource().fallbackAllocations(),
                  m2.bids().nodePolicy().blockResource().fallbackAllocations())
            << "only construction-time bookkeeping may use the fallback";
    }
}

TEST(Capacity, DenseLadderCapacityIsItsRange) {
    DenseOrderBook book(kInstrument, {.minPrice = {0}, .tickCount = 4});
    EXPECT_TRUE(book.bids().canRestAt({3}));
    EXPECT_FALSE(book.bids().canRestAt({4}));
    EXPECT_EQ(book.addOrder(limit(1, OrderSide::BUY, 4, 1)).error(),
              RejectReason::PRICE_OUT_OF_RANGE);
}

} // namespace eigenbook
