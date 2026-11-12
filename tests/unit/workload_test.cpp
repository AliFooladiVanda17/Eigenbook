//
// Benchmark workloads: determinism, and that replaying a stream on either book reproduces the
// outcome the generator predicted for every operation (so the live set is correct on fills).
//

#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <harness/executor.hpp>
#include <workloads/w1_baseline.hpp>
#include <workloads/w2_level_churn.hpp>
#include <workloads/w3_cancel_heavy.hpp>
#include <workloads/w4_adversarial_index.hpp>
#include <workloads/w5_microburst.hpp>
#include <workloads/workload.hpp>

#include <eigenbook/book/locator/hash_order_index.hpp>
#include <eigenbook/book/order_book.hpp>

namespace eigenbook::bench {

namespace {

constexpr WorkloadSize kSmall{.warmupOps = 20'000, .measuredOps = 20'000};

std::vector<Workload> smallWorkloads(const std::uint64_t seed) {
    std::vector<Workload> out;
    out.push_back(makeBaseline({.seed = seed, .totalOps = 40'000, .warmupOps = 20'000}));
    LevelChurnConfig churn;
    churn.seed = seed;
    churn.size = kSmall;
    out.push_back(makeLevelChurn(churn));
    LevelChurnConfig drift = levelChurnDrift(seed);
    drift.size = kSmall;
    out.push_back(makeLevelChurn(drift));
    out.push_back(makeCancelHeavy({.seed = seed, .size = kSmall, .cancelFraction = 0.9}));
    out.push_back(makeAdversarialIndex({.seed = seed,
                                        .size = {.warmupOps = 2'000, .measuredOps = 2'000},
                                        .mode = IndexAttack::COLLIDE,
                                        .expectedOrders = 1 << 12,
                                        .collidingIds = 256}));
    out.push_back(makeAdversarialIndex(
        {.seed = seed, .size = kSmall, .mode = IndexAttack::LOAD_HALF, .expectedOrders = 1 << 12}));
    MicroburstConfig burst;
    burst.seed = seed;
    burst.size = kSmall;
    out.push_back(makeMicroburst(burst));
    return out;
}

bool sameOp(const Op& a, const Op& b) {
    return a.kind == b.kind && a.side == b.side && a.type == b.type && a.timed == b.timed &&
           a.position == b.position && a.expected == b.expected &&
           a.expectedFills == b.expectedFills && a.id == b.id && a.price == b.price &&
           a.quantity == b.quantity && a.timestamp == b.timestamp && a.scheduledNs == b.scheduledNs;
}

template <typename Book>
std::unique_ptr<Book> bookFor(const Workload& w);

template <>
std::unique_ptr<OrderBook> bookFor<OrderBook>(const Workload& w) {
    return std::make_unique<OrderBook>(InstrumentId{1}, MapLadderConfig{}, w.expectedOrders);
}

template <>
std::unique_ptr<DenseOrderBook> bookFor<DenseOrderBook>(const Workload& w) {
    return std::make_unique<DenseOrderBook>(
        InstrumentId{1},
        ArrayLadderConfig{.minPrice = w.minPrice, .tickCount = w.tickCount()},
        w.expectedOrders);
}

template <typename Book>
void expectReplayMatches(const Workload& w) {
    auto book = bookFor<Book>(w);
    for (std::size_t i = 0; i < w.ops.size(); ++i) {
        const Op& op = w.ops[i];
        if (op.kind == OpKind::CANCEL && op.expected != OpClass::CANCEL_MISS) {
            ASSERT_NE(book->findOrder(op.id), nullptr)
                << w.name << " op " << i << ": cancel of a non-resting order";
        }
        std::uint32_t fills = 0;
        const bool ok = runOp(*book, op, fills);
        ASSERT_TRUE(outcomeMatches(op, ok, fills)) << w.name << " op " << i;
    }
}

} // namespace

TEST(Workloads, SameSeedGivesTheSameStream) {
    const std::vector<Workload> a = smallWorkloads(5);
    const std::vector<Workload> b = smallWorkloads(5);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t k = 0; k < a.size(); ++k) {
        ASSERT_EQ(a[k].ops.size(), b[k].ops.size()) << a[k].name;
        for (std::size_t i = 0; i < a[k].ops.size(); ++i) {
            ASSERT_TRUE(sameOp(a[k].ops[i], b[k].ops[i])) << a[k].name << " op " << i;
        }
    }
    const std::vector<Workload> c = smallWorkloads(6);
    EXPECT_FALSE(sameOp(a[1].ops[a[1].warmupOps + 10], c[1].ops[c[1].warmupOps + 10]));
}

TEST(Workloads, ReplayOnBothBooksMatchesThePrediction) {
    for (const Workload& w : smallWorkloads(9)) {
        ASSERT_GT(w.timedOps, 0u) << w.name;
        ASSERT_EQ(std::ranges::count_if(w.ops.begin(),
                                        w.ops.begin() + static_cast<std::ptrdiff_t>(w.warmupOps),
                                        [](const Op& o) { return o.timed; }),
                  0)
            << w.name << ": a warm-up op is marked timed";
        expectReplayMatches<OrderBook>(w);
        expectReplayMatches<DenseOrderBook>(w);
    }
}

TEST(Workloads, CancelHeavyTargetsTheQueuePositionItClaims) {
    const Workload w = makeCancelHeavy({.seed = 2, .size = kSmall, .cancelFraction = 0.7});
    OrderBook book(InstrumentId{1}, {}, w.expectedOrders);
    std::size_t checked = 0;
    for (const Op& op : w.ops) {
        if (op.kind == OpKind::CANCEL && op.timed && op.expected != OpClass::CANCEL_MISS) {
            const Order* order = book.findOrder(op.id);
            ASSERT_NE(order, nullptr);
            const PriceLevel* level = order->getSide() == OrderSide::BUY
                                          ? book.bids().findLevel(order->getPrice())
                                          : book.asks().findLevel(order->getPrice());
            ASSERT_GE(level->orderCount(), 3u);
            switch (op.position) {
                case QueuePosition::FRONT:
                    EXPECT_EQ(level->front(), order);
                    break;
                case QueuePosition::BACK:
                    EXPECT_EQ(level->back(), order);
                    break;
                case QueuePosition::MIDDLE:
                    EXPECT_NE(level->front(), order);
                    EXPECT_NE(level->back(), order);
                    break;
                case QueuePosition::ANY:
                    ADD_FAILURE() << "W3 cancel without a queue position";
                    break;
            }
            ++checked;
        }
        std::uint32_t fills = 0;
        (void)runOp(book, op, fills);
    }
    EXPECT_GT(checked, 10'000u);
}

TEST(Workloads, LevelChurnMeetsItsTarget) {
    LevelChurnConfig c;
    c.seed = 4;
    c.size = kSmall;
    EXPECT_GE(makeLevelChurn(c).churnPerOp(), 0.5);
}

TEST(Workloads, UnhashInvertsTheIndexHash) {
    std::mt19937_64 rng(1);
    for (int k = 0; k < 100'000; ++k) {
        const std::uint64_t x = rng();
        ASSERT_EQ(unhash(OrderIndex::hash(x)), x);
        ASSERT_EQ(OrderIndex::hash(unhash(x)), x);
    }
}

TEST(Workloads, CollidingIdsShareOneHomeSlotAndFormOneCluster) {
    const AdversarialIndexConfig cfg{.seed = 3,
                                     .size = {.warmupOps = 1'000, .measuredOps = 1'000},
                                     .mode = IndexAttack::COLLIDE,
                                     .expectedOrders = 1 << 12,
                                     .collidingIds = 300};
    const Workload w = makeAdversarialIndex(cfg);
    const std::uint64_t mask = OrderIndex::capacityFor(cfg.expectedOrders) - 1;
    std::uint64_t home = ~std::uint64_t{0};
    for (const Op& op : w.ops) {
        const std::uint64_t slot = OrderIndex::hash(op.id.value) & mask;
        if (home == ~std::uint64_t{0}) {
            home = slot;
        }
        ASSERT_EQ(slot, home);
    }
    OrderBook book(InstrumentId{1}, {}, w.expectedOrders);
    for (std::size_t i = 0; i < cfg.collidingIds; ++i) {
        std::uint32_t fills = 0;
        ASSERT_TRUE(runOp(book, w.ops[i], fills));
    }
    EXPECT_EQ(book.orderIndex().capacity(), mask + 1);
    EXPECT_EQ(book.orderIndex().maxClusterLength(), cfg.collidingIds);
}

TEST(Workloads, MicroburstScheduleIsOrderedAndHasTheConfiguredMeanRate) {
    MicroburstConfig c;
    c.seed = 8;
    c.size = {.warmupOps = 1'000, .measuredOps = 200'000};
    const Workload w = makeMicroburst(c);
    std::uint64_t last = 0;
    for (std::size_t i = w.warmupOps; i < w.ops.size(); ++i) {
        ASSERT_GE(w.ops[i].scheduledNs, last);
        last = w.ops[i].scheduledNs;
    }
    const double rate = static_cast<double>(c.size.measuredOps) /
                        (static_cast<double>(last) * 1e-9);
    const double expected = c.baseRatePerSecond / (1.0 - c.excitation / c.decayPerSecond);
    EXPECT_NEAR(rate, expected, expected * 0.1);
}

} // namespace eigenbook::bench
