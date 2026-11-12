//
// PriceLevel: the intrusive FIFO queue at one price.
//

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

#include "test_support.hpp"

namespace eigenbook {

namespace {

std::vector<std::uint64_t> ids(const PriceLevel& level) {
    std::vector<std::uint64_t> out;
    level.forEachOrder([&](const Order& o) { out.push_back(o.getId().value); });
    return out;
}

} // namespace

TEST(PriceLevel, StartsEmpty) {
    const PriceLevel level{Price{100}};
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.price(), Price{100});
    EXPECT_EQ(level.quantity(), Quantity{0});
    EXPECT_EQ(level.orderCount(), 0u);
    EXPECT_EQ(level.front(), nullptr);
    EXPECT_EQ(level.back(), nullptr);
}

TEST(PriceLevel, PushBackKeepsArrivalOrderAndAggregate) {
    PriceLevel level{Price{100}};
    Order a = makeOrder(1, OrderSide::BUY, 100, 10);
    Order b = makeOrder(2, OrderSide::BUY, 100, 20);
    Order c = makeOrder(3, OrderSide::BUY, 100, 30);
    level.pushBack(a);
    level.pushBack(b);
    level.pushBack(c);

    EXPECT_EQ(ids(level), (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(level.quantity(), Quantity{60});
    EXPECT_EQ(level.orderCount(), 3u);
    EXPECT_EQ(level.front(), &a);
    EXPECT_EQ(level.back(), &c);
    EXPECT_EQ(a.nextInLevel(), &b);
    EXPECT_EQ(PriceLevel::ownerOf(b), &level);
}

TEST(PriceLevel, EraseFromHeadMiddleAndTail) {
    PriceLevel level{Price{100}};
    Order a = makeOrder(1, OrderSide::SELL, 100, 1);
    Order b = makeOrder(2, OrderSide::SELL, 100, 2);
    Order c = makeOrder(3, OrderSide::SELL, 100, 4);
    Order d = makeOrder(4, OrderSide::SELL, 100, 8);
    for (Order* o : {&a, &b, &c, &d}) {
        level.pushBack(*o);
    }

    level.erase(b); // middle
    EXPECT_EQ(ids(level), (std::vector<std::uint64_t>{1, 3, 4}));
    EXPECT_EQ(PriceLevel::ownerOf(b), nullptr);
    level.erase(d); // tail
    EXPECT_EQ(level.back(), &c);
    level.erase(a); // head
    EXPECT_EQ(level.front(), &c);
    EXPECT_EQ(level.quantity(), Quantity{4});
    EXPECT_EQ(level.orderCount(), 1u);
    level.erase(c); // last one
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.quantity(), Quantity{0});
}

TEST(PriceLevel, FillFrontAndPopFront) {
    PriceLevel level{Price{50}};
    Order a = makeOrder(1, OrderSide::BUY, 50, 10);
    Order b = makeOrder(2, OrderSide::BUY, 50, 5);
    level.pushBack(a);
    level.pushBack(b);

    level.fillFront(Quantity{4});
    EXPECT_EQ(a.getQuantity(), Quantity{6});
    EXPECT_EQ(level.quantity(), Quantity{11});
    EXPECT_EQ(level.front(), &a);

    level.popFront();
    EXPECT_EQ(level.front(), &b);
    EXPECT_EQ(level.quantity(), Quantity{5});
    EXPECT_EQ(level.orderCount(), 1u);
}

TEST(PriceLevel, ReduceKeepsPosition) {
    PriceLevel level{Price{50}};
    Order a = makeOrder(1, OrderSide::BUY, 50, 10);
    Order b = makeOrder(2, OrderSide::BUY, 50, 10);
    level.pushBack(a);
    level.pushBack(b);

    level.reduce(a, Quantity{3});
    EXPECT_EQ(ids(level), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(a.getQuantity(), Quantity{3});
    EXPECT_EQ(level.quantity(), Quantity{13});
}

} // namespace eigenbook
