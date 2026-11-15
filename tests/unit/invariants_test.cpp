//
// The book's invariant checker, the periodic audit and the halted state. Corruption is simulated
// with the public PriceLevel operations applied behind the book's back (const_cast), the way a
// memory error or a bug in a ladder would leave it.
//

#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

template <typename Book>
class InvariantsTest : public ::testing::Test {
protected:
    std::unique_ptr<Book> book = BookFactory<Book>::make();

    void fill() {
        ASSERT_TRUE(book->addOrder(limit(1, OrderSide::BUY, 100, 5)));
        ASSERT_TRUE(book->addOrder(limit(2, OrderSide::BUY, 100, 7)));
        ASSERT_TRUE(book->addOrder(limit(3, OrderSide::BUY, 98, 1)));
        ASSERT_TRUE(book->addOrder(limit(4, OrderSide::SELL, 103, 2)));
        ASSERT_TRUE(book->addOrder(limit(5, OrderSide::SELL, 140, 9)));
    }

    // Takes the order out of its level without telling the book or its locator.
    void unlinkBehindTheBooksBack(const std::uint64_t id) {
        Order& order = const_cast<Order&>(*book->findOrder({id}));
        PriceLevel::ownerOf(order)->erase(order);
    }
};

TYPED_TEST_SUITE(InvariantsTest, AllBookTypes);

} // namespace

TYPED_TEST(InvariantsTest, HoldAfterOrdinaryOperations) {
    auto& book = *this->book;
    EXPECT_EQ(book.checkInvariants(), std::nullopt);
    this->fill();
    EXPECT_EQ(book.checkInvariants(), std::nullopt);
    ASSERT_TRUE(book.addOrder(limit(6, OrderSide::SELL, 100, 6))); // trades 5, then 1 of 7
    ASSERT_TRUE(book.modifyOrder({.id = {2}, .price = {101}, .quantity = {6}}));
    ASSERT_TRUE(book.modifyOrder({.id = {5}, .price = {140}, .quantity = {3}}));
    ASSERT_TRUE(book.cancelOrder({3}));
    EXPECT_EQ(book.checkInvariants(), std::nullopt);
}

TYPED_TEST(InvariantsTest, AnOrderMissingFromItsLevelIsFound) {
    this->fill();
    this->unlinkBehindTheBooksBack(2); // level 100 keeps order 1
    EXPECT_EQ(this->book->checkInvariants(), InvariantViolation::ORDER_COUNT);
}

TYPED_TEST(InvariantsTest, AnEmptyLevelLeftInTheLadderIsFound) {
    this->fill();
    this->unlinkBehindTheBooksBack(3); // level 98 is now empty but still registered
    EXPECT_EQ(this->book->checkInvariants(), InvariantViolation::LADDER);
}

TYPED_TEST(InvariantsTest, HaltRejectsNewOrdersAndModifiesButNotCancels) {
    auto& book = *this->book;
    this->fill();
    EXPECT_FALSE(book.isHalted());
    book.halt();
    EXPECT_TRUE(book.isHalted());
    EXPECT_EQ(book.haltReason(), HaltReason::MANUAL);
    EXPECT_EQ(book.addOrder(limit(10, OrderSide::SELL, 100, 1)).error(),
              RejectReason::INSTRUMENT_HALTED);
    EXPECT_EQ(book.addOrder(market(11, OrderSide::SELL, 1)).error(),
              RejectReason::INSTRUMENT_HALTED);
    EXPECT_EQ(book.modifyOrder({.id = {1}, .price = {100}, .quantity = {1}}).error(),
              RejectReason::INSTRUMENT_HALTED);
    EXPECT_EQ(book.findOrder({1})->getQuantity(), Quantity{5});
    EXPECT_TRUE(book.cancelOrder({1}));
    EXPECT_TRUE(book.cancelOrder({2}, ParticipantId{0}));
    EXPECT_TRUE(book.resume());
    EXPECT_FALSE(book.isHalted());
    EXPECT_TRUE(book.addOrder(limit(10, OrderSide::SELL, 100, 1)));
}

TYPED_TEST(InvariantsTest, ManualAuditHaltsACorruptBookForGood) {
    auto& book = *this->book;
    this->fill();
    EXPECT_EQ(book.audit(), std::nullopt);
    EXPECT_FALSE(book.isHalted());
    this->unlinkBehindTheBooksBack(3);
    EXPECT_EQ(book.audit(), InvariantViolation::LADDER);
    EXPECT_EQ(book.haltReason(), HaltReason::AUDIT_FAILED);
    // A corrupt book is not resumed: it has to be rebuilt (from the journal, Phase B3).
    EXPECT_FALSE(book.resume());
    EXPECT_TRUE(book.isHalted());
    EXPECT_EQ(book.addOrder(limit(10, OrderSide::BUY, 90, 1)).error(),
              RejectReason::INSTRUMENT_HALTED);
}

TEST(Invariants, PeriodicAuditRunsEveryNMutations) {
    DenseOrderBook book(kInstrument, {.minPrice = {0}, .tickCount = 1000}, {.auditEvery = 3});
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 5)));
    ASSERT_TRUE(book.addOrder(limit(2, OrderSide::BUY, 99, 5)));
    // Corrupt between audits: the third mutation triggers the audit, which halts the book.
    Order& order = const_cast<Order&>(*book.findOrder({2}));
    PriceLevel::ownerOf(order)->erase(order);
    EXPECT_FALSE(book.isHalted());
    ASSERT_TRUE(book.addOrder(limit(3, OrderSide::SELL, 200, 1)));
    EXPECT_TRUE(book.isHalted());
    EXPECT_EQ(book.haltReason(), HaltReason::AUDIT_FAILED);
    EXPECT_EQ(book.lastViolation(), InvariantViolation::LADDER);
}

TEST(Invariants, RejectsAndHaltsDoNotCountAsMutations) {
    DenseOrderBook book(kInstrument, {.minPrice = {0}, .tickCount = 1000}, {.auditEvery = 2});
    ASSERT_TRUE(book.addOrder(limit(1, OrderSide::BUY, 100, 5)));
    Order& order = const_cast<Order&>(*book.findOrder({1}));
    PriceLevel::ownerOf(order)->erase(order);
    EXPECT_FALSE(book.addOrder(limit(2, OrderSide::BUY, 100, 0))); // rejected: no audit
    EXPECT_FALSE(book.cancelOrder({99}));                          // rejected: no audit
    EXPECT_FALSE(book.isHalted());
}

} // namespace eigenbook
