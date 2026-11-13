//
// Books with the A4 locators: engine-issued handles (DenseOrderBookDirect) and the hardened index
// (DenseOrderBookHardened).
//

#include <cstdint>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

DenseOrderBookDirect directBook(const std::size_t capacity = 64) {
    return DenseOrderBookDirect(kInstrument, {.minPrice = {0}, .tickCount = 1000}, capacity);
}

OrderRequest with(OrderRequest r, const OrderId id) {
    r.id = id;
    return r;
}

} // namespace

TEST(DirectBook, ReservedHandleRestsAndCancelsAndThenGoesStale) {
    auto book = directBook();
    const OrderId h = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::BUY, 100, 5), h)));
    ASSERT_NE(book.findOrder(h), nullptr);
    EXPECT_EQ(book.cancelOrder(h).value(), Quantity{5});
    EXPECT_EQ(book.cancelOrder(h).error(), RejectReason::UNKNOWN_ORDER_ID); // stale handle
    EXPECT_EQ(book.addOrder(with(limit(0, OrderSide::BUY, 100, 5), h)).error(),
              RejectReason::INVALID_ORDER_ID);
}

TEST(DirectBook, IdsTheBookDidNotIssueAreRejectedBeforeMatching) {
    auto book = directBook();
    const OrderId ask = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::SELL, 100, 5), ask)));
    FillLog fills;
    EXPECT_EQ(book.addOrder(limit(12345, OrderSide::BUY, 100, 5), fills).error(),
              RejectReason::INVALID_ORDER_ID);
    EXPECT_TRUE(fills.fills.empty());
    EXPECT_EQ(book.findOrder(ask)->getQuantity(), Quantity{5});
}

TEST(DirectBook, AnOrderThatDoesNotRestGivesItsHandleBack) {
    auto book = directBook(2);
    const OrderId maker = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::SELL, 100, 5), maker)));
    const OrderId taker = *book.reserveOrderId();
    EXPECT_FALSE(book.reserveOrderId()); // both slots in use
    FillLog fills;
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::BUY, 100, 5), taker), fills));
    ASSERT_EQ(fills.fills.size(), 1u);
    EXPECT_EQ(fills.fills[0].makerId, maker);
    EXPECT_EQ(fills.fills[0].takerId, taker);
    // Both are gone (the maker filled, the taker never rested): both slots are free again.
    EXPECT_TRUE(book.empty());
    EXPECT_TRUE(book.reserveOrderId());
    EXPECT_TRUE(book.reserveOrderId());
    EXPECT_EQ(book.reserveOrderId().error(), RejectReason::CAPACITY_EXCEEDED);
}

TEST(DirectBook, MarketOrderGivesItsHandleBack) {
    auto book = directBook(1);
    const OrderId h = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(market(0, OrderSide::BUY, 3), h)));
    EXPECT_TRUE(book.reserveOrderId());
}

TEST(DirectBook, ModifyKeepsTheHandle) {
    auto book = directBook();
    const OrderId h = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::BUY, 100, 5), h)));
    ASSERT_TRUE(book.modifyOrder({.id = h, .price = {120}, .quantity = {7}}));
    const Order* order = book.findOrder(h);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->getPrice(), Price{120});
    EXPECT_EQ(order->getQuantity(), Quantity{7});
    EXPECT_EQ(book.bids().findLevel({100}), nullptr);
}

TEST(DirectBook, ModifyThatFillsCompletelyReleasesTheHandle) {
    auto book = directBook(2);
    const OrderId ask = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::SELL, 110, 5), ask)));
    const OrderId bid = *book.reserveOrderId();
    ASSERT_TRUE(book.addOrder(with(limit(0, OrderSide::BUY, 100, 5), bid)));
    ASSERT_TRUE(book.modifyOrder({.id = bid, .price = {110}, .quantity = {5}})); // crosses, fills
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.cancelOrder(bid).error(), RejectReason::UNKNOWN_ORDER_ID);
    EXPECT_TRUE(book.reserveOrderId());
    EXPECT_TRUE(book.reserveOrderId());
}

TEST(DirectBook, UnusedReservationCanBeReleased) {
    auto book = directBook(1);
    const OrderId h = *book.reserveOrderId();
    EXPECT_TRUE(book.releaseOrderId(h));
    EXPECT_FALSE(book.releaseOrderId(h));
    EXPECT_TRUE(book.reserveOrderId());
}

TEST(HardenedBook, FullIndexIsACapacityRejectBeforeMatching) {
    // 8 expected orders: a 16-slot table (the minimum), full at load 1/2 = 8 resting orders.
    DenseOrderBookHardened book(kInstrument, {.minPrice = {0}, .tickCount = 1000}, 8);
    ASSERT_EQ(book.orderIndex().capacity(), 16u);
    for (std::uint64_t id = 1; id <= 8; ++id) {
        ASSERT_TRUE(
            book.addOrder(limit(id, OrderSide::SELL, 100 + static_cast<std::int64_t>(id), 1)));
    }
    FillLog fills;
    EXPECT_EQ(book.addOrder(limit(20, OrderSide::BUY, 200, 10), fills).error(),
              RejectReason::CAPACITY_EXCEEDED);
    EXPECT_TRUE(fills.fills.empty());
    EXPECT_EQ(book.orderCount(), 8u);
    // A cancel frees room again.
    ASSERT_TRUE(book.cancelOrder({1}));
    EXPECT_TRUE(book.addOrder(limit(20, OrderSide::BUY, 50, 1)));
}

} // namespace eigenbook
