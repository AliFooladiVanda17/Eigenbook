//
// Order locators: DirectOrderTable (engine-issued handles) and HardenedOrderIndex (keyed hash,
// bounded probe distance, fixed capacity).
//

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <workloads/w4_adversarial_index.hpp>

#include <eigenbook/book/locator/direct_order_table.hpp>
#include <eigenbook/book/locator/hardened_hash_index.hpp>
#include <eigenbook/book/locator/hash_order_index.hpp>
#include <eigenbook/core/order.hpp>

namespace eigenbook {

namespace {

struct Orders {
    std::vector<std::unique_ptr<Order>> all;

    Order* make(const std::uint64_t id) {
        all.push_back(std::make_unique<Order>(OrderId{id},
                                              OrderSide::BUY,
                                              OrderType::LIMIT,
                                              Price{1},
                                              Quantity{1},
                                              Timestamp{id}));
        return all.back().get();
    }
};

} // namespace

// --- DirectOrderTable --------------------------------------------------------------------------

TEST(DirectOrderTable, IssuedHandleRestsIsFoundAndGoesStaleWhenErased) {
    DirectOrderTable table(4);
    Orders orders;
    const std::optional<OrderId> h = table.issue();
    ASSERT_TRUE(h);
    EXPECT_NE(h->value, 0u);
    EXPECT_EQ(table.admit(*h), Admission::OK);
    Order* o = orders.make(h->value);
    table.insert(*h, o);
    EXPECT_EQ(table.find(*h), o);
    EXPECT_EQ(table.admit(*h), Admission::DUPLICATE);
    EXPECT_EQ(table.size(), 1u);

    EXPECT_TRUE(table.erase(*h));
    EXPECT_EQ(table.find(*h), nullptr);
    EXPECT_FALSE(table.erase(*h)); // stale
    EXPECT_EQ(table.admit(*h), Admission::NOT_ISSUED);

    // The slot is reused with a new generation: the old handle still does not match.
    const std::optional<OrderId> again = table.issue();
    ASSERT_TRUE(again);
    EXPECT_EQ(DirectOrderTable::slotOf(*again), DirectOrderTable::slotOf(*h));
    EXPECT_NE(DirectOrderTable::generationOf(*again), DirectOrderTable::generationOf(*h));
    table.insert(*again, orders.make(again->value));
    EXPECT_EQ(table.find(*h), nullptr);
    EXPECT_NE(table.find(*again), nullptr);
}

TEST(DirectOrderTable, FullTableIssuesNothingUntilASlotIsReleased) {
    DirectOrderTable table(3);
    std::vector<OrderId> handles;
    for (int i = 0; i < 3; ++i) {
        handles.push_back(*table.issue());
    }
    EXPECT_FALSE(table.issue());
    EXPECT_TRUE(table.release(handles[1]));
    EXPECT_FALSE(table.release(handles[1])); // already released: stale
    EXPECT_TRUE(table.issue());
    EXPECT_FALSE(table.issue());
}

TEST(DirectOrderTable, ForeignAndStaleHandlesAreRejectedWithoutTouchingTheTable) {
    DirectOrderTable table(2);
    Orders orders;
    const OrderId h = *table.issue();
    table.insert(h, orders.make(1));
    const OrderId outOfRange = DirectOrderTable::handle(7, 1);
    const OrderId wrongGeneration = DirectOrderTable::handle(DirectOrderTable::slotOf(h),
                                                             DirectOrderTable::generationOf(h) + 1);
    for (const OrderId bad : {outOfRange, wrongGeneration, OrderId{0}}) {
        EXPECT_EQ(table.find(bad), nullptr);
        EXPECT_EQ(table.admit(bad), Admission::NOT_ISSUED);
        EXPECT_FALSE(table.erase(bad));
        EXPECT_FALSE(table.release(bad));
    }
    EXPECT_EQ(table.size(), 1u);
    EXPECT_NE(table.find(h), nullptr);
}

TEST(DirectOrderTable, UnbindKeepsTheHandleReservedForReentry) {
    DirectOrderTable table(2);
    Orders orders;
    const OrderId h = *table.issue();
    table.insert(h, orders.make(1));
    table.unbind(h);
    EXPECT_EQ(table.find(h), nullptr);
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.admit(h), Admission::OK); // still reserved: modify can re-insert it
    table.insert(h, orders.make(1));
    EXPECT_NE(table.find(h), nullptr);
    EXPECT_FALSE(table.release(h)); // resting handles are erased, not released
}

TEST(DirectOrderTable, AbandonedHandleGoesStale) {
    DirectOrderTable table(1);
    const OrderId h = *table.issue();
    table.abandon(h);
    EXPECT_EQ(table.admit(h), Admission::NOT_ISSUED);
    EXPECT_TRUE(table.issue()); // the slot is free again
}

TEST(DirectOrderTable, RejectsZeroCapacity) {
    EXPECT_THROW(DirectOrderTable(0), std::invalid_argument);
}

// --- HardenedOrderIndex ------------------------------------------------------------------------

TEST(HardenedOrderIndex, MatchesUnorderedMapUnderChurn) {
    HardenedOrderIndex index(1 << 12); // random seed
    std::unordered_map<std::uint64_t, Order*> reference;
    Orders orders;
    std::vector<std::uint64_t> live;
    std::mt19937_64 rng(17);
    for (int step = 0; step < 200'000; ++step) {
        if (live.size() < 3'000 && (live.empty() || rng() % 2 == 0)) {
            const std::uint64_t id = rng();
            if (index.admit({id}) != Admission::OK) {
                ASSERT_TRUE(reference.contains(id) || index.probeLimitRejects() > 0);
                continue;
            }
            Order* o = orders.make(id);
            index.insert({id}, o);
            reference.emplace(id, o);
            live.push_back(id);
        } else {
            const std::size_t pick = rng() % live.size();
            ASSERT_TRUE(index.erase({live[pick]}));
            reference.erase(live[pick]);
            live[pick] = live.back();
            live.pop_back();
        }
        if (step % 1'000 == 0) {
            for (const auto& [id, o] : reference) {
                ASSERT_EQ(index.find({id}), o);
            }
            ASSERT_EQ(index.find({rng()}), nullptr);
        }
    }
    EXPECT_EQ(index.size(), reference.size());
    EXPECT_EQ(index.probeLimitRejects(), 0u) << "random ids should never come close to the limit";
}

TEST(HardenedOrderIndex, CraftedCollisionsHitTheProbeGuardOnlyWithAKnownSeed) {
    constexpr std::size_t kExpected = 1 << 10; // capacity 2048 (11 bits)
    constexpr std::size_t kMaxProbe = 16;
    // Ids that all share one home slot under hash(id ^ 0), i.e. against an attacker who knows the
    // seed is 0.
    std::vector<std::uint64_t> ids;
    for (std::uint64_t j = 1; j <= 200; ++j) {
        ids.push_back(bench::unhash((j << 11) | 5));
    }

    HardenedOrderIndex known(kExpected, {.seed = 0, .maxProbe = kMaxProbe});
    Orders orders;
    std::size_t admitted = 0;
    for (const std::uint64_t id : ids) {
        if (known.admit({id}) == Admission::OK) {
            known.insert({id}, orders.make(id));
            ++admitted;
        } else {
            EXPECT_EQ(known.admit({id}), Admission::PROBE_LIMIT);
        }
    }
    EXPECT_EQ(admitted, kMaxProbe + 1); // distances 0 .. maxProbe
    EXPECT_GT(known.probeLimitRejects(), 0u);
    EXPECT_LE(known.maxClusterLength(), kMaxProbe + 1);
    for (std::size_t k = 0; k <= kMaxProbe; ++k) {
        EXPECT_NE(known.find({ids[k]}), nullptr);
    }
    EXPECT_EQ(known.find({ids.back()}), nullptr); // gives up after maxProbe + 1 slots

    // The same ids against an unknown seed: spread out, all admitted, no long cluster.
    HardenedOrderIndex keyed(kExpected, {.seed = 0x9e3779b97f4a7c15ULL, .maxProbe = kMaxProbe});
    for (const std::uint64_t id : ids) {
        ASSERT_EQ(keyed.admit({id}), Admission::OK);
        keyed.insert({id}, orders.make(id));
    }
    EXPECT_LT(keyed.maxClusterLength(), 10u);
    EXPECT_EQ(keyed.probeLimitRejects(), 0u);
}

TEST(HardenedOrderIndex, IsFullAtLoadOneHalfAndNeverGrows) {
    HardenedOrderIndex index(8, {.seed = 1}); // capacity 16
    ASSERT_EQ(index.capacity(), 16u);
    Orders orders;
    for (std::uint64_t id = 1; id <= 8; ++id) {
        ASSERT_EQ(index.admit({id}), Admission::OK);
        index.insert({id}, orders.make(id));
    }
    EXPECT_EQ(index.admit({99}), Admission::FULL);
    EXPECT_EQ(index.admit({3}), Admission::DUPLICATE);
    EXPECT_TRUE(index.erase({3}));
    EXPECT_EQ(index.admit({99}), Admission::OK);
    EXPECT_EQ(index.capacity(), 16u);
}

TEST(HardenedOrderIndex, ErasureKeepsEveryRemainingIdWithinReach) {
    HardenedOrderIndex index(1 << 8, {.seed = 0, .maxProbe = 8});
    Orders orders;
    std::vector<std::uint64_t> ids;
    for (std::uint64_t j = 1; j <= 9; ++j) { // one full cluster of 9 at the probe limit
        ids.push_back(bench::unhash((j << 9) | 3));
        ASSERT_EQ(index.admit({ids.back()}), Admission::OK);
        index.insert({ids.back()}, orders.make(ids.back()));
    }
    for (std::size_t k = 0; k < ids.size(); k += 2) {
        ASSERT_TRUE(index.erase({ids[k]}));
    }
    for (std::size_t k = 1; k < ids.size(); k += 2) {
        EXPECT_NE(index.find({ids[k]}), nullptr) << k;
    }
}

} // namespace eigenbook
