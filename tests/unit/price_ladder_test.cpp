//
// Both price-ladder strategies, on both sides of the book.
//

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

template <typename Ladder>
class PriceLadderTest : public ::testing::Test {
protected:
    static constexpr OrderSide kSide = Ladder::side;
    static constexpr bool kBid = kSide == OrderSide::BUY;

    // Default configs, except that the map ladders get a level bound (M1 / M2 need one): the array
    // covers [0, 65536), the hybrid window [0, 4096) with maps beyond it.
    static typename Ladder::Config config() {
        typename Ladder::Config c{};
        if constexpr (requires { c.maxLevels; } && !requires { c.windowTicks; }) {
            c.maxLevels = 1 << 16;
        }
        return c;
    }

    Ladder ladder{config()};

    Order make(std::uint64_t id, std::int64_t price, std::uint64_t qty = 1) {
        return makeOrder(id, kSide, price, qty);
    }

    std::vector<std::int64_t> prices() const {
        std::vector<std::int64_t> out;
        ladder.forEachLevel([&](const PriceLevel& l) { out.push_back(l.price().value); });
        return out;
    }
};

using LadderTypes = ::testing::Types<MapPriceLadder<OrderSide::BUY>,
                                     MapPriceLadder<OrderSide::SELL>,
                                     MapPriceLadderM1<OrderSide::BUY>,
                                     MapPriceLadderM1<OrderSide::SELL>,
                                     MapPriceLadderM2<OrderSide::BUY>,
                                     MapPriceLadderM2<OrderSide::SELL>,
                                     ArrayPriceLadder<OrderSide::BUY>,
                                     ArrayPriceLadder<OrderSide::SELL>,
                                     HybridPriceLadder<OrderSide::BUY>,
                                     HybridPriceLadder<OrderSide::SELL>>;
TYPED_TEST_SUITE(PriceLadderTest, LadderTypes);

} // namespace

TYPED_TEST(PriceLadderTest, EmptyLadderHasNoBest) {
    EXPECT_TRUE(this->ladder.empty());
    EXPECT_EQ(this->ladder.levelCount(), 0u);
    EXPECT_EQ(this->ladder.bestLevel(), nullptr);
    EXPECT_FALSE(this->ladder.bestPrice().has_value());
    EXPECT_EQ(this->ladder.findLevel(Price{100}), nullptr);
}

TYPED_TEST(PriceLadderTest, BestIsHighestBidOrLowestAsk) {
    Order a = this->make(1, 100);
    Order b = this->make(2, 105);
    Order c = this->make(3, 95);
    this->ladder.addOrder(a);
    this->ladder.addOrder(b);
    this->ladder.addOrder(c);

    EXPECT_EQ(this->ladder.bestPrice(), Price{this->kBid ? 105 : 95});
    EXPECT_EQ(this->ladder.levelCount(), 3u);
    EXPECT_EQ(this->prices(),
              this->kBid ? (std::vector<std::int64_t>{105, 100, 95})
                         : (std::vector<std::int64_t>{95, 100, 105}));
}

TYPED_TEST(PriceLadderTest, SamePriceSharesOneLevel) {
    Order a = this->make(1, 100, 3);
    Order b = this->make(2, 100, 4);
    this->ladder.addOrder(a);
    this->ladder.addOrder(b);
    const PriceLevel* level = this->ladder.findLevel(Price{100});
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->orderCount(), 2u);
    EXPECT_EQ(level->quantity(), Quantity{7});
    EXPECT_EQ(this->ladder.levelCount(), 1u);
}

TYPED_TEST(PriceLadderTest, RemovingLastOrderDropsLevelAndMovesBest) {
    Order a = this->make(1, 100);
    Order b = this->make(2, 105);
    Order c = this->make(3, 95);
    this->ladder.addOrder(a);
    this->ladder.addOrder(b);
    this->ladder.addOrder(c);

    Order& best = this->kBid ? b : c;
    this->ladder.removeOrder(best);
    EXPECT_EQ(this->ladder.findLevel(best.getPrice()), nullptr);
    EXPECT_EQ(this->ladder.bestPrice(), Price{100});
    EXPECT_EQ(this->ladder.levelCount(), 2u);

    this->ladder.removeOrder(a);
    this->ladder.removeOrder(this->kBid ? c : b);
    EXPECT_TRUE(this->ladder.empty());
    EXPECT_FALSE(this->ladder.bestPrice().has_value());
}

TYPED_TEST(PriceLadderTest, RemovingNonBestLevelKeepsBest) {
    Order a = this->make(1, 100);
    Order b = this->make(2, 105);
    Order c = this->make(3, 95);
    this->ladder.addOrder(a);
    this->ladder.addOrder(b);
    this->ladder.addOrder(c);

    this->ladder.removeOrder(a);
    EXPECT_EQ(this->ladder.bestPrice(), Price{this->kBid ? 105 : 95});
    EXPECT_EQ(this->ladder.findLevel(Price{100}), nullptr);
    EXPECT_EQ(this->ladder.levelCount(), 2u);
}

TEST(ArrayPriceLadder, AcceptsExactlyItsTickRange) {
    const ArrayPriceLadder<OrderSide::BUY> ladder({.minPrice = {-10}, .tickCount = 21});
    EXPECT_EQ(ladder.lowestPrice(), Price{-10});
    EXPECT_EQ(ladder.highestPrice(), Price{10});
    EXPECT_TRUE(ladder.accepts(Price{-10}));
    EXPECT_TRUE(ladder.accepts(Price{10}));
    EXPECT_FALSE(ladder.accepts(Price{-11}));
    EXPECT_FALSE(ladder.accepts(Price{11}));
    EXPECT_EQ(ladder.findLevel(Price{1'000}), nullptr);
}

TEST(ArrayPriceLadder, BestTracksRangeEdges) {
    ArrayPriceLadder<OrderSide::SELL> asks({.minPrice = {0}, .tickCount = 8});
    ArrayPriceLadder<OrderSide::BUY> bids({.minPrice = {0}, .tickCount = 8});
    Order lowAsk = makeOrder(1, OrderSide::SELL, 0, 1);
    Order highAsk = makeOrder(2, OrderSide::SELL, 7, 1);
    Order lowBid = makeOrder(3, OrderSide::BUY, 0, 1);
    Order highBid = makeOrder(4, OrderSide::BUY, 7, 1);
    asks.addOrder(highAsk);
    asks.addOrder(lowAsk);
    bids.addOrder(lowBid);
    bids.addOrder(highBid);

    EXPECT_EQ(asks.bestPrice(), Price{0});
    EXPECT_EQ(bids.bestPrice(), Price{7});
    asks.removeOrder(lowAsk);
    bids.removeOrder(highBid);
    EXPECT_EQ(asks.bestPrice(), Price{7});
    EXPECT_EQ(bids.bestPrice(), Price{0});
    asks.removeOrder(highAsk);
    bids.removeOrder(lowBid);
    EXPECT_FALSE(asks.bestPrice().has_value());
    EXPECT_FALSE(bids.bestPrice().has_value());
}

TEST(MapPriceLadder, AcceptsAnyPrice) {
    const MapPriceLadder<OrderSide::SELL> ladder;
    EXPECT_TRUE(ladder.accepts(Price{INT64_MIN}));
    EXPECT_TRUE(ladder.accepts(Price{INT64_MAX}));
}

} // namespace eigenbook
