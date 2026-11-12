//
// Building blocks: occupancy bitmap, order index, object pool.
//

#include <bit>
#include <cstdint>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <workloads/w4_adversarial_index.hpp>

#include <eigenbook/book/locator/hash_order_index.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/util/level_bitmap.hpp>
#include <eigenbook/util/object_pool.hpp>

namespace eigenbook {

TEST(LevelBitmap, FindNextAndPrevMatchReferenceSet) {
    constexpr std::size_t kBits = 70'000; // spans several summary words, ragged tail
    LevelBitmap bitmap(kBits);
    std::set<std::size_t> reference;
    std::mt19937_64 rng(1);
    std::uniform_int_distribution<std::size_t> pick(0, kBits - 1);

    for (int round = 0; round < 4000; ++round) {
        const std::size_t i = pick(rng);
        if (rng() % 3 == 0) {
            bitmap.reset(i);
            reference.erase(i);
        } else {
            bitmap.set(i);
            reference.insert(i);
        }
        const std::size_t q = pick(rng);
        const auto next = reference.lower_bound(q);
        EXPECT_EQ(bitmap.findNext(q), next == reference.end() ? LevelBitmap::npos : *next);
        auto prev = reference.upper_bound(q);
        EXPECT_EQ(bitmap.findPrev(q),
                  prev == reference.begin() ? LevelBitmap::npos : *std::prev(prev));
    }
    EXPECT_EQ(bitmap.findFirst(), reference.empty() ? LevelBitmap::npos : *reference.begin());
    EXPECT_EQ(bitmap.findLast(), reference.empty() ? LevelBitmap::npos : *reference.rbegin());
}

TEST(LevelBitmap, EmptyBitmapFindsNothing) {
    const LevelBitmap bitmap(1000);
    EXPECT_EQ(bitmap.size(), 1000u);
    EXPECT_EQ(bitmap.findFirst(), LevelBitmap::npos);
    EXPECT_EQ(bitmap.findLast(), LevelBitmap::npos);
    EXPECT_EQ(bitmap.findNext(0), LevelBitmap::npos);
    EXPECT_EQ(bitmap.findPrev(999), LevelBitmap::npos);
}

TEST(OrderIndex, MatchesUnorderedMapUnderChurn) {
    OrderIndex index(16); // force several rehashes
    std::unordered_map<std::uint64_t, Order*> reference;
    std::vector<Order*> handles;
    for (std::uintptr_t i = 1; i <= 64; ++i) {
        handles.push_back(reinterpret_cast<Order*>(i * alignof(Order)));
    }
    std::mt19937_64 rng(2);
    for (int round = 0; round < 50'000; ++round) {
        const std::uint64_t key = rng() % 2048;
        Order* value = handles[rng() % handles.size()];
        if (rng() % 2 == 0) {
            EXPECT_EQ(index.insert({key}, value), reference.emplace(key, value).second);
        } else {
            EXPECT_EQ(index.erase({key}), reference.erase(key) == 1);
        }
        const std::uint64_t probe = rng() % 2048;
        const auto it = reference.find(probe);
        EXPECT_EQ(index.find({probe}), it == reference.end() ? nullptr : it->second);
    }
    EXPECT_EQ(index.size(), reference.size());
}

TEST(ObjectPool, RecyclesSlotsWithoutGrowing) {
    ObjectPool<Order> pool(8);
    pool.reserve(8);
    EXPECT_EQ(pool.capacity(), 8u);
    std::vector<Order*> live;
    for (std::uint64_t i = 0; i < 8; ++i) {
        live.push_back(pool.create(OrderId{i},
                                   OrderSide::BUY,
                                   OrderType::LIMIT,
                                   Price{1},
                                   Quantity{1},
                                   Timestamp{0}));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(live.back()) % kCacheLineSize, 0u);
    }
    Order* recycled = live[3];
    pool.destroy(recycled);
    EXPECT_EQ(pool.create(OrderId{99},
                          OrderSide::SELL,
                          OrderType::LIMIT,
                          Price{2},
                          Quantity{2},
                          Timestamp{0}),
              recycled);
    EXPECT_EQ(pool.capacity(), 8u);
    EXPECT_EQ(pool.size(), 8u);
}

TEST(OrderIndex, CapacityForIsWhatReserveAllocates) {
    for (const std::size_t n : {0u, 1u, 8u, 9u, 1000u, 1024u, 1025u, 131072u}) {
        const OrderIndex index(n);
        EXPECT_EQ(index.capacity(), OrderIndex::capacityFor(n)) << n;
        EXPECT_GE(index.capacity(), 2 * n);
        EXPECT_TRUE(std::has_single_bit(index.capacity()));
    }
}

TEST(OrderIndex, MaxClusterLengthCountsTheLongestRunAcrossTheWrap) {
    OrderIndex index(8); // capacity 16
    ASSERT_EQ(index.capacity(), 16u);
    EXPECT_EQ(index.maxClusterLength(), 0u);
    std::vector<std::unique_ptr<Order>> orders;
    auto insertHomedAt = [&](const std::uint64_t home, const std::uint64_t j) {
        const std::uint64_t id = bench::unhash((j << 4) | home);
        orders.push_back(std::make_unique<Order>(OrderId{id},
                                                 OrderSide::BUY,
                                                 OrderType::LIMIT,
                                                 Price{1},
                                                 Quantity{1},
                                                 Timestamp{1}));
        ASSERT_TRUE(index.insert({id}, orders.back().get()));
    };
    // Three ids homed at the last slot occupy slots 15, 0 and 1.
    for (std::uint64_t j = 1; j <= 3; ++j) {
        insertHomedAt(15, j);
    }
    EXPECT_EQ(index.maxClusterLength(), 3u);
    // Two homed at slot 7: a separate, shorter run.
    insertHomedAt(7, 1);
    insertHomedAt(7, 2);
    EXPECT_EQ(index.maxClusterLength(), 3u);
}

} // namespace eigenbook
