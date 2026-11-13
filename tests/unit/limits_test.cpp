//
// BookLimits and checked arithmetic: every limit rejects before any mutation, on add and on
// modify; a modify is all-or-nothing; a hard order limit never grows storage; resting orders carry
// the book's priority sequence.
//

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <eigenbook/core/checked_math.hpp>

#include "test_support.hpp"

namespace eigenbook {

namespace {

template <typename Book>
std::unique_ptr<Book> withLimits(const BookLimits limits);

template <>
std::unique_ptr<OrderBook> withLimits<OrderBook>(const BookLimits limits) {
    return std::make_unique<OrderBook>(kInstrument, MapLadderConfig{}, limits);
}

template <>
std::unique_ptr<DenseOrderBook> withLimits<DenseOrderBook>(const BookLimits limits) {
    return std::make_unique<DenseOrderBook>(kInstrument,
                                            ArrayLadderConfig{.minPrice = {0}, .tickCount = 1000},
                                            limits);
}

template <>
std::unique_ptr<HybridOrderBook> withLimits<HybridOrderBook>(const BookLimits limits) {
    return std::make_unique<HybridOrderBook>(kInstrument, smallWindowHybrid(), limits);
}

template <typename Book>
class LimitsTest : public ::testing::Test {};

using LimitBooks = ::testing::Types<OrderBook, DenseOrderBook, HybridOrderBook>;
TYPED_TEST_SUITE(LimitsTest, LimitBooks);

// The order with `id` still rests unchanged at `price` with `qty`, at `position` in its queue.
template <typename Book>
void expectUnchanged(const Book& book, std::uint64_t id, std::int64_t price, std::uint64_t qty) {
    const Order* order = book.findOrder({id});
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->getPrice(), Price{price});
    EXPECT_EQ(order->getQuantity(), Quantity{qty});
}

} // namespace

TYPED_TEST(LimitsTest, OrderQuantityAboveTheLimitIsRejected) {
    auto book = withLimits<TypeParam>({.maxOrderQuantity = {100}});
    EXPECT_EQ(book->addOrder(limit(1, OrderSide::BUY, 100, 101)).error(),
              RejectReason::ORDER_QUANTITY_LIMIT);
    EXPECT_EQ(book->addOrder(market(2, OrderSide::BUY, 101)).error(),
              RejectReason::ORDER_QUANTITY_LIMIT);
    EXPECT_TRUE(book->addOrder(limit(3, OrderSide::BUY, 100, 100)));
    EXPECT_EQ(book->modifyOrder({.id = {3}, .price = {100}, .quantity = {101}}).error(),
              RejectReason::ORDER_QUANTITY_LIMIT);
    expectUnchanged(*book, 3, 100, 100);
}

TYPED_TEST(LimitsTest, PricesOutsideTheBandAreRejectedButMarketOrdersAreNot) {
    auto book = withLimits<TypeParam>({.minPrice = {90}, .maxPrice = {110}});
    EXPECT_EQ(book->addOrder(limit(1, OrderSide::BUY, 89, 1)).error(),
              RejectReason::PRICE_OUT_OF_BAND);
    EXPECT_EQ(book->addOrder(limit(2, OrderSide::SELL, 111, 1)).error(),
              RejectReason::PRICE_OUT_OF_BAND);
    ASSERT_TRUE(book->addOrder(limit(3, OrderSide::BUY, 90, 1)));
    ASSERT_TRUE(book->addOrder(limit(4, OrderSide::SELL, 110, 1)));
    EXPECT_TRUE(book->addOrder(market(5, OrderSide::BUY, 1)));
    EXPECT_EQ(book->modifyOrder({.id = {3}, .price = {120}, .quantity = {1}}).error(),
              RejectReason::PRICE_OUT_OF_BAND);
    expectUnchanged(*book, 3, 90, 1);
}

TYPED_TEST(LimitsTest, NotionalAboveTheLimitIsRejected) {
    auto book = withLimits<TypeParam>({.maxNotional = 1'000});
    EXPECT_EQ(book->addOrder(limit(1, OrderSide::BUY, 100, 11)).error(),
              RejectReason::NOTIONAL_LIMIT);
    EXPECT_TRUE(book->addOrder(limit(2, OrderSide::BUY, 100, 10))); // exactly 1,000
    EXPECT_EQ(book->modifyOrder({.id = {2}, .price = {101}, .quantity = {10}}).error(),
              RejectReason::NOTIONAL_LIMIT);
    expectUnchanged(*book, 2, 100, 10);
}

TYPED_TEST(LimitsTest, LevelQuantityLimitCountsTheModifiedOrderOnce) {
    auto book = withLimits<TypeParam>({.maxLevelQuantity = {50}});
    ASSERT_TRUE(book->addOrder(limit(1, OrderSide::BUY, 100, 30)));
    ASSERT_TRUE(book->addOrder(limit(2, OrderSide::BUY, 100, 20)));
    EXPECT_EQ(book->addOrder(limit(3, OrderSide::BUY, 100, 1)).error(),
              RejectReason::LEVEL_QUANTITY_LIMIT);
    EXPECT_TRUE(book->addOrder(limit(4, OrderSide::BUY, 99, 50))); // other level
    // Order 1 grows to 31 at the same level: 31 + 20 > 50.
    EXPECT_EQ(book->modifyOrder({.id = {1}, .price = {100}, .quantity = {31}}).error(),
              RejectReason::LEVEL_QUANTITY_LIMIT);
    expectUnchanged(*book, 1, 100, 30);
    EXPECT_EQ(queueAt(*book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{1, 2}));
    // Moving order 2 to level 99 (already 50): rejected, unchanged.
    EXPECT_EQ(book->modifyOrder({.id = {2}, .price = {99}, .quantity = {20}}).error(),
              RejectReason::LEVEL_QUANTITY_LIMIT);
    expectUnchanged(*book, 2, 100, 20);
    // Within the limit: fine.
    EXPECT_TRUE(book->modifyOrder({.id = {1}, .price = {100}, .quantity = {30}}));
}

TYPED_TEST(LimitsTest, LimitRejectsHappenBeforeAnyMatching) {
    auto book = withLimits<TypeParam>({.maxOrderQuantity = {10}, .maxNotional = 5'000});
    ASSERT_TRUE(book->addOrder(limit(1, OrderSide::SELL, 100, 5)));
    FillLog fills;
    EXPECT_EQ(book->addOrder(limit(2, OrderSide::BUY, 100, 11), fills).error(),
              RejectReason::ORDER_QUANTITY_LIMIT);
    EXPECT_EQ(book->addOrder(limit(3, OrderSide::BUY, 600, 10), fills).error(),
              RejectReason::NOTIONAL_LIMIT);
    EXPECT_TRUE(fills.fills.empty());
    expectUnchanged(*book, 1, 100, 5);
}

TYPED_TEST(LimitsTest, HardOrderLimitRejectsInsteadOfGrowing) {
    auto book = withLimits<TypeParam>({.maxOrders = 4});
    for (std::uint64_t id = 1; id <= 4; ++id) {
        ASSERT_TRUE(
            book->addOrder(limit(id, OrderSide::BUY, 100 - static_cast<std::int64_t>(id), 1)));
    }
    EXPECT_EQ(book->addOrder(limit(5, OrderSide::BUY, 50, 1)).error(),
              RejectReason::CAPACITY_EXCEEDED);
    EXPECT_TRUE(book->addOrder(market(6, OrderSide::SELL, 1))); // never rests: not capacity-limited
    EXPECT_EQ(book->orderCount(), 3u);
    EXPECT_TRUE(book->addOrder(limit(7, OrderSide::BUY, 50, 1)));
    // A modify re-entry reuses its own slot: always fits.
    EXPECT_TRUE(book->modifyOrder({.id = {7}, .price = {60}, .quantity = {2}}));
    const std::size_t indexCapacity = book->orderIndex().capacity();
    for (std::uint64_t round = 0; round < 1'000; ++round) {
        ASSERT_TRUE(book->cancelOrder({7 + round}));
        ASSERT_TRUE(book->addOrder(limit(8 + round, OrderSide::BUY, 60, 1)));
    }
    EXPECT_EQ(book->orderIndex().capacity(), indexCapacity);
}

TYPED_TEST(LimitsTest, RestingOrdersCarryTheBookPrioritySequence) {
    auto book = withLimits<TypeParam>({});
    ASSERT_TRUE(book->addOrder(limit(1, OrderSide::BUY, 100, 5)));
    ASSERT_TRUE(book->addOrder(limit(2, OrderSide::BUY, 100, 5)));
    const Timestamp first = book->findOrder({1})->getTimestamp();
    const Timestamp second = book->findOrder({2})->getTimestamp();
    EXPECT_LT(first, second);
    ASSERT_TRUE(book->modifyOrder({.id = {1}, .price = {100}, .quantity = {4}})); // in place
    EXPECT_EQ(book->findOrder({1})->getTimestamp(), first);
    ASSERT_TRUE(book->modifyOrder({.id = {1}, .price = {100}, .quantity = {6}})); // re-enters
    EXPECT_GT(book->findOrder({1})->getTimestamp(), second);
}

TEST(Limits, DenseLadderHandlesExtremePricesWithoutOverflow) {
    // UBSan builds check that no signed overflow happens on the way.
    DenseOrderBook book(kInstrument, {.minPrice = {1'000}, .tickCount = 100});
    for (const std::int64_t p : {std::numeric_limits<std::int64_t>::min(),
                                 std::numeric_limits<std::int64_t>::max(),
                                 std::int64_t{-1},
                                 std::int64_t{999},
                                 std::int64_t{1'100}}) {
        EXPECT_EQ(book.addOrder(limit(1, OrderSide::BUY, p, 1)).error(),
                  RejectReason::PRICE_OUT_OF_RANGE)
            << p;
    }
    EXPECT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 1'099, 1)));
}

TEST(CheckedMath, NotionalIsExactAndNeverOverflows) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(notionalWithin(Price{0}, Quantity{kMax}, 0));
    EXPECT_TRUE(notionalWithin(Price{-7}, Quantity{3}, 21));
    EXPECT_FALSE(notionalWithin(Price{-7}, Quantity{3}, 20));
    EXPECT_TRUE(notionalWithin(Price{1 << 20}, Quantity{1 << 20}, std::uint64_t{1} << 40));
    EXPECT_FALSE(
        notionalWithin(Price{std::numeric_limits<std::int64_t>::min()}, Quantity{2}, kMax));
    EXPECT_TRUE(notionalWithin(Price{std::numeric_limits<std::int64_t>::min()}, Quantity{1}, kMax));
    EXPECT_EQ(magnitude(Price{std::numeric_limits<std::int64_t>::min()}), std::uint64_t{1} << 63);
    EXPECT_TRUE(sumWithin(Quantity{40}, Quantity{10}, Quantity{50}));
    EXPECT_FALSE(sumWithin(Quantity{41}, Quantity{10}, Quantity{50}));
    EXPECT_FALSE(sumWithin(Quantity{0}, Quantity{kMax}, Quantity{kMax - 1}));
    EXPECT_FALSE(sumWithin(Quantity{kMax}, Quantity{1}, Quantity{kMax})); // would wrap
}

} // namespace eigenbook
