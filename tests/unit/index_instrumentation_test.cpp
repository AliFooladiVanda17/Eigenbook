//
// OrderIndex probe statistics (EIGENBOOK_INSTRUMENT_INDEX). Built as its own executable with the
// macro defined, because the macro changes OrderIndex's layout.
//

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <workloads/w4_adversarial_index.hpp>

#include <eigenbook/book/locator/hash_order_index.hpp>
#include <eigenbook/core/order.hpp>

#if !defined(EIGENBOOK_INSTRUMENT_INDEX)
#error "build this test with EIGENBOOK_INSTRUMENT_INDEX"
#endif

namespace eigenbook {

namespace {

struct Fixture {
    static constexpr std::size_t kExpected = 1024; // capacity 2048
    static constexpr std::uint64_t kHome = 5;

    OrderIndex index{kExpected};
    std::vector<std::unique_ptr<Order>> orders;

    // j-th id whose home slot is kHome.
    [[nodiscard]] static std::uint64_t collidingId(const std::uint64_t j) {
        return bench::unhash((j << 11) | kHome);
    }

    Order* order(const std::uint64_t id) {
        orders.push_back(std::make_unique<Order>(OrderId{id},
                                                 OrderSide::BUY,
                                                 OrderType::LIMIT,
                                                 Price{1},
                                                 Quantity{1},
                                                 Timestamp{id}));
        return orders.back().get();
    }
};

} // namespace

TEST(IndexInstrumentation, InsertProbesGrowAlongOneCluster) {
    Fixture f;
    ASSERT_EQ(f.index.capacity(), 2048u);
    constexpr std::uint64_t kN = 100;
    for (std::uint64_t j = 1; j <= kN; ++j) {
        ASSERT_TRUE(f.index.insert({Fixture::collidingId(j)}, f.order(j)));
    }
    const OrderIndexStats& s = f.index.stats();
    EXPECT_EQ(s.insertProbes.total, kN);
    EXPECT_EQ(s.insertProbes.max, kN); // the j-th insert inspects j slots
    for (std::uint64_t j = 1; j < 64; ++j) {
        EXPECT_EQ(s.insertProbes.counts[j], 1u) << j;
    }
    EXPECT_EQ(f.index.maxClusterLength(), kN);

    f.index.resetStats();
    EXPECT_NE(f.index.find({Fixture::collidingId(kN)}), nullptr);
    EXPECT_EQ(f.index.stats().findProbes.max, kN);
    EXPECT_EQ(f.index.find({Fixture::collidingId(kN + 1)}), nullptr); // miss: whole cluster + 1
    EXPECT_EQ(f.index.stats().findProbes.max, kN + 1);
}

TEST(IndexInstrumentation, EraseAtTheHeadShiftsTheWholeCluster) {
    Fixture f;
    constexpr std::uint64_t kN = 50;
    for (std::uint64_t j = 1; j <= kN; ++j) {
        ASSERT_TRUE(f.index.insert({Fixture::collidingId(j)}, f.order(j)));
    }
    f.index.resetStats();
    ASSERT_TRUE(f.index.erase({Fixture::collidingId(1)}));
    const OrderIndexStats& s = f.index.stats();
    EXPECT_EQ(s.eraseProbes.max, 1u);     // found in its home slot
    EXPECT_EQ(s.eraseScan.max, kN - 1);   // every later member inspected
    EXPECT_EQ(s.eraseShifts.max, kN - 1); // and moved back by one
    EXPECT_EQ(f.index.maxClusterLength(), kN - 1);

    ASSERT_TRUE(f.index.erase({Fixture::collidingId(kN)})); // tail: nothing to shift
    EXPECT_EQ(f.index.stats().eraseShifts.counts[0], 1u);
    EXPECT_EQ(f.index.stats().eraseProbes.max, kN - 1);
}

TEST(IndexInstrumentation, LongLengthsUseLogBuckets) {
    EXPECT_EQ(OrderIndexStats::bucketOf(63), 63u);
    EXPECT_EQ(OrderIndexStats::bucketOf(64), 64u);
    EXPECT_EQ(OrderIndexStats::bucketOf(127), 64u);
    EXPECT_EQ(OrderIndexStats::bucketOf(128), 65u);
    EXPECT_EQ(OrderIndexStats::bucketFloor(65), 128u);
    EXPECT_LT(OrderIndexStats::bucketOf(~std::uint64_t{0}), OrderIndexStats::kBuckets);
}

} // namespace eigenbook
