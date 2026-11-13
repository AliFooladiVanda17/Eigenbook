//
// HybridPriceLadder: window + overflow maps, incremental recentering, structural invariants.
//

#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace eigenbook {

namespace {

template <OrderSide Side>
struct Fixture {
    HybridPriceLadder<Side> ladder;
    std::vector<std::unique_ptr<Order>> orders;

    explicit Fixture(const HybridLadderConfig c) : ladder(c) {}

    Order& add(const std::int64_t price, const std::uint64_t qty = 1) {
        orders.push_back(std::make_unique<Order>(OrderId{orders.size() + 1},
                                                 Side,
                                                 OrderType::LIMIT,
                                                 Price{price},
                                                 Quantity{qty},
                                                 Timestamp{orders.size() + 1}));
        ladder.addOrder(*orders.back());
        return *orders.back();
    }

    std::vector<std::int64_t> prices() const {
        std::vector<std::int64_t> out;
        ladder.forEachLevel([&](const PriceLevel& l) { out.push_back(l.price().value); });
        return out;
    }
};

HybridLadderConfig window64(const std::int64_t base) {
    return {.windowTicks = 64,
            .initialBase = {base},
            .maxLevels = 1 << 14,
            .maxShiftsPerOp = 16,
            .maxMovesPerOp = 4};
}

} // namespace

TEST(HybridLadder, PricesOutsideTheWindowLiveInTheMapsAndBestIsGlobal) {
    Fixture<OrderSide::BUY> bids(window64(0));
    bids.add(10);       // window
    bids.add(500'000);  // above the window: the best bid
    bids.add(-500'000); // below
    EXPECT_EQ(bids.ladder.bestPrice(), Price{500'000});
    EXPECT_EQ(bids.prices(), (std::vector<std::int64_t>{500'000, 10, -500'000}));
    EXPECT_TRUE(bids.ladder.checkInvariants());

    Fixture<OrderSide::SELL> asks(window64(1'000));
    asks.add(1'010);
    asks.add(5); // below the window: the best ask
    asks.add(900'000);
    EXPECT_EQ(asks.ladder.bestPrice(), Price{5});
    EXPECT_EQ(asks.prices(), (std::vector<std::int64_t>{5, 1'010, 900'000}));
    EXPECT_TRUE(asks.ladder.checkInvariants());
}

TEST(HybridLadder, WindowFollowsATrendingTouchWithBoundedWorkPerAdd) {
    Fixture<OrderSide::BUY> bids(window64(0));
    std::uint64_t lastMoves = 0;
    for (std::int64_t p = 0; p < 20'000; p += 3) {
        bids.add(p);
        ASSERT_TRUE(bids.ladder.checkInvariants()) << p;
        const std::uint64_t moved = bids.ladder.levelsMoved() - lastMoves;
        ASSERT_LE(moved, 4u) << "more than maxMovesPerOp levels moved in one add";
        lastMoves = bids.ladder.levelsMoved();
    }
    // The touch (19'998) is back in the window, and old levels were evicted to the map below.
    const std::int64_t base = bids.ladder.windowBase().value;
    EXPECT_GE(19'998, base);
    EXPECT_LT(19'998, base + 64);
    EXPECT_GT(bids.ladder.overflowLevels(), 6'000u);
    EXPECT_EQ(bids.ladder.levelCount(), 6'667u);
}

TEST(HybridLadder, RemovingTheBestAcrossWindowAndMapsKeepsOrder) {
    Fixture<OrderSide::SELL> asks(window64(0));
    std::mt19937_64 rng(5);
    std::vector<Order*> all;
    for (int i = 0; i < 2'000; ++i) {
        all.push_back(&asks.add(static_cast<std::int64_t>(rng() % 5'000)));
    }
    ASSERT_TRUE(asks.ladder.checkInvariants());
    // Drain from the best: prices must come out in increasing order.
    std::int64_t last = std::numeric_limits<std::int64_t>::min();
    while (PriceLevel* best = asks.ladder.bestLevel()) {
        ASSERT_GE(best->price().value, last);
        last = best->price().value;
        asks.ladder.removeOrder(*best->front());
        ASSERT_TRUE(asks.ladder.checkInvariants());
    }
    EXPECT_TRUE(asks.ladder.empty());
}

TEST(HybridLadder, LargeJumpIsServedFromTheMapUntilTheWindowCatchesUp) {
    Fixture<OrderSide::BUY> bids(window64(0));
    bids.add(10);
    bids.add(1'000'000); // far above: lands in the map, becomes the touch
    EXPECT_EQ(bids.ladder.bestPrice(), Price{1'000'000});
    // Each add slides the window by at most 16 ticks; the result stays correct throughout.
    for (int i = 0; i < 100; ++i) {
        bids.add(1'000'000 - i);
        ASSERT_TRUE(bids.ladder.checkInvariants());
        ASSERT_EQ(bids.ladder.bestPrice(), Price{1'000'000});
    }
}

TEST(HybridLadder, ExtremePricesAreHandledWithoutOverflow) {
    // UBSan builds check that no signed overflow happens on the way.
    EXPECT_THROW(
        HybridPriceLadder<OrderSide::BUY>(window64(std::numeric_limits<std::int64_t>::max() - 10)),
        std::invalid_argument); // the window would extend past INT64_MAX
    Fixture<OrderSide::BUY> bids(window64(std::numeric_limits<std::int64_t>::max() - 100));
    bids.add(std::numeric_limits<std::int64_t>::min());
    bids.add(std::numeric_limits<std::int64_t>::max());
    bids.add(0);
    EXPECT_EQ(bids.ladder.bestPrice(), Price{std::numeric_limits<std::int64_t>::max()});
    EXPECT_EQ(bids.ladder.levelCount(), 3u);
    EXPECT_TRUE(bids.ladder.checkInvariants());
}

TEST(HybridLadder, RejectsInvalidConfiguration) {
    EXPECT_THROW(HybridPriceLadder<OrderSide::BUY>({.windowTicks = 100}), std::invalid_argument);
    EXPECT_THROW(HybridPriceLadder<OrderSide::BUY>({.windowTicks = 32}), std::invalid_argument);
    EXPECT_THROW(HybridPriceLadder<OrderSide::BUY>({.windowTicks = 64, .maxMovesPerOp = 1}),
                 std::invalid_argument);
}

} // namespace eigenbook
