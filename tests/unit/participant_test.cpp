//
// Participants and trade ids: an order remembers its owner, only the owner may cancel or modify it
// (the administrative cancel may always), fills name the maker's owner and carry per-book trade ids
// that increase by one per execution.
//

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

constexpr ParticipantId kAlice{11};
constexpr ParticipantId kBob{22};

OrderRequest owned(OrderRequest request, const ParticipantId owner) {
    request.participant = owner;
    return request;
}

template <typename Book>
class ParticipantTest : public ::testing::Test {
protected:
    std::unique_ptr<Book> book = BookFactory<Book>::make();
};

TYPED_TEST_SUITE(ParticipantTest, AllBookTypes);

} // namespace

TYPED_TEST(ParticipantTest, RestingOrderRemembersItsOwner) {
    ASSERT_TRUE(this->book->addOrder(owned(limit(1, OrderSide::BUY, 100, 5), kAlice)));
    ASSERT_TRUE(this->book->addOrder(limit(2, OrderSide::BUY, 100, 5)));
    EXPECT_EQ(this->book->findOrder({1})->getParticipant(), kAlice);
    EXPECT_EQ(this->book->findOrder({2})->getParticipant(), ParticipantId{0});
}

TYPED_TEST(ParticipantTest, OnlyTheOwnerMayCancel) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(owned(limit(1, OrderSide::SELL, 100, 5), kAlice)));
    EXPECT_EQ(book.cancelOrder({1}, kBob).error(), RejectReason::NOT_OWNER);
    ASSERT_NE(book.findOrder({1}), nullptr);
    EXPECT_EQ(book.findOrder({1})->getQuantity(), Quantity{5});
    EXPECT_EQ(book.cancelOrder({1}, kAlice).value(), Quantity{5});
    // Unknown ids say so, whoever asks: ownership is not checked for orders that are not there.
    EXPECT_EQ(book.cancelOrder({1}, kAlice).error(), RejectReason::UNKNOWN_ORDER_ID);
    EXPECT_EQ(book.cancelOrder({1}, kBob).error(), RejectReason::UNKNOWN_ORDER_ID);
}

TYPED_TEST(ParticipantTest, AdministrativeCancelIgnoresOwnership) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(owned(limit(1, OrderSide::BUY, 100, 7), kAlice)));
    EXPECT_EQ(book.cancelOrder({1}).value(), Quantity{7});
    EXPECT_TRUE(book.empty());
}

TYPED_TEST(ParticipantTest, OnlyTheOwnerMayModifyAndReentryKeepsTheOwner) {
    auto& book = *this->book;
    ASSERT_TRUE(book.addOrder(owned(limit(1, OrderSide::BUY, 100, 5), kAlice)));
    ASSERT_TRUE(book.addOrder(owned(limit(2, OrderSide::BUY, 100, 5), kBob)));
    // Not the owner: rejected before anything else is looked at, the order keeps its place.
    EXPECT_EQ(
        book.modifyOrder({.id = {1}, .price = {100}, .quantity = {3}, .participant = kBob}).error(),
        RejectReason::NOT_OWNER);
    EXPECT_EQ(
        book.modifyOrder({.id = {1}, .price = {100}, .quantity = {0}, .participant = kBob}).error(),
        RejectReason::NOT_OWNER);
    EXPECT_EQ(queueAt(book, OrderSide::BUY, 100), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(book.findOrder({1})->getQuantity(), Quantity{5});
    // The owner moves it; it re-enters still owned by Alice.
    ASSERT_TRUE(
        book.modifyOrder({.id = {1}, .price = {101}, .quantity = {6}, .participant = kAlice}));
    EXPECT_EQ(book.findOrder({1})->getParticipant(), kAlice);
    EXPECT_EQ(book.findOrder({1})->getPrice(), Price{101});
}

TYPED_TEST(ParticipantTest, FillsCarryTradeIdsAndTheMakersOwner) {
    auto& book = *this->book;
    EXPECT_EQ(book.lastTradeId(), TradeId{0});
    ASSERT_TRUE(book.addOrder(owned(limit(1, OrderSide::SELL, 100, 2), kAlice)));
    ASSERT_TRUE(book.addOrder(owned(limit(2, OrderSide::SELL, 101, 2), kBob)));
    FillLog log;
    ASSERT_TRUE(book.addOrder(owned(limit(3, OrderSide::BUY, 101, 3), kBob), log));
    ASSERT_EQ(log.fills.size(), 2u);
    EXPECT_EQ(log.fills[0].tradeId, TradeId{1});
    EXPECT_EQ(log.fills[0].makerParticipant, kAlice);
    EXPECT_EQ(log.fills[1].tradeId, TradeId{2});
    EXPECT_EQ(log.fills[1].makerParticipant, kBob);
    // A modify that re-enters and trades continues the same sequence.
    ASSERT_TRUE(book.addOrder(owned(limit(4, OrderSide::BUY, 90, 1), kAlice)));
    ASSERT_TRUE(
        book.modifyOrder({.id = {4}, .price = {101}, .quantity = {1}, .participant = kAlice}, log));
    ASSERT_EQ(log.fills.size(), 3u);
    EXPECT_EQ(log.fills[2].tradeId, TradeId{3});
    EXPECT_EQ(log.fills[2].makerParticipant, kBob);
    EXPECT_EQ(book.lastTradeId(), TradeId{3});
    // Rejects and non-crossing orders issue no trade id.
    EXPECT_FALSE(book.addOrder(limit(5, OrderSide::BUY, 100, 0)));
    ASSERT_TRUE(book.addOrder(limit(6, OrderSide::BUY, 50, 1)));
    EXPECT_EQ(book.lastTradeId(), TradeId{3});
}

TEST(Participant, DirectHandlesCheckOwnershipToo) {
    DenseOrderBookDirect book(kInstrument, {.minPrice = {0}, .tickCount = 1000});
    const OrderId handle = book.reserveOrderId().value();
    ASSERT_TRUE(book.addOrder({.id = handle,
                               .side = OrderSide::BUY,
                               .type = OrderType::LIMIT,
                               .price = {100},
                               .quantity = {5},
                               .participant = kAlice}));
    EXPECT_EQ(book.cancelOrder(handle, kBob).error(), RejectReason::NOT_OWNER);
    EXPECT_EQ(book.cancelOrder(handle, kAlice).value(), Quantity{5});
}

} // namespace eigenbook
