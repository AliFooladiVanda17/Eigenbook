//
// Strong types and the Order node.
//

#include <cstdint>
#include <unordered_set>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

TEST(Price, ArithmeticIsInTicks) {
    constexpr Price p{100};
    static_assert(p + 5 == Price{105});
    static_assert(p - 5 == Price{95});
    static_assert(Price{105} - p == 5);
    static_assert(Price{-3} < Price{2});
    EXPECT_EQ((Price{-10} - Price{10}), -20);
}

TEST(Quantity, ArithmeticAndZero) {
    Quantity q{10};
    q += Quantity{5};
    EXPECT_EQ(q, Quantity{15});
    q -= Quantity{15};
    EXPECT_TRUE(q.isZero());
    EXPECT_EQ(Quantity{7} + Quantity{3}, Quantity{10});
    EXPECT_EQ(Quantity{7} - Quantity{3}, Quantity{4});
    EXPECT_LT(Quantity{1}, Quantity{2});
}

TEST(OrderSide, OppositeFlipsSide) {
    static_assert(opposite(OrderSide::BUY) == OrderSide::SELL);
    static_assert(opposite(OrderSide::SELL) == OrderSide::BUY);
}

TEST(StrongIds, HashAndCompareByValue) {
    std::unordered_set<OrderId> ids{{1}, {2}, {2}, {3}};
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_TRUE(ids.contains(OrderId{2}));

    std::unordered_set<InstrumentId> instruments{{7}, {7}};
    EXPECT_EQ(instruments.size(), 1u);

    std::unordered_set<ShardId> shards{{0}, {1}};
    EXPECT_EQ(shards.size(), 2u);
    EXPECT_LT(ShardId{0}, ShardId{1});
    EXPECT_LT(Timestamp{1}, Timestamp{2});
}

TEST(Order, ConstructionExposesFieldsAndStartsUnlinked) {
    const Order order{OrderId{42},
                      OrderSide::SELL,
                      OrderType::LIMIT,
                      Price{101},
                      Quantity{9},
                      Timestamp{5}};
    EXPECT_EQ(order.getId(), OrderId{42});
    EXPECT_EQ(order.getSide(), OrderSide::SELL);
    EXPECT_EQ(order.getType(), OrderType::LIMIT);
    EXPECT_EQ(order.getPrice(), Price{101});
    EXPECT_EQ(order.getQuantity(), Quantity{9});
    EXPECT_EQ(order.getTimestamp(), Timestamp{5});
    EXPECT_FALSE(order.isFilled());
    EXPECT_EQ(order.nextInLevel(), nullptr);
}

TEST(Order, OccupiesExactlyOneCacheLine) {
    EXPECT_EQ(sizeof(Order), kCacheLineSize);
    EXPECT_EQ(alignof(Order), kCacheLineSize);
}

} // namespace eigenbook
