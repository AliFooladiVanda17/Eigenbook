//
// Building blocks: occupancy bitmap, order index, object pool.
//

#include <cstdint>
#include <gtest/gtest.h>
#include <iterator>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include <eigenbook/book/locator/hash_order_index.hpp>
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
        }
        else {
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
        }
        else {
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
        live.push_back(pool.create(
            OrderId{i}, OrderSide::BUY, OrderType::LIMIT, Price{1}, Quantity{1}, Timestamp{0}));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(live.back()) % kCacheLineSize, 0u);
    }
    Order* recycled = live[3];
    pool.destroy(recycled);
    EXPECT_EQ(pool.create(OrderId{99}, OrderSide::SELL, OrderType::LIMIT, Price{2}, Quantity{2},
                          Timestamp{0}),
              recycled);
    EXPECT_EQ(pool.capacity(), 8u);
    EXPECT_EQ(pool.size(), 8u);
}

} // namespace eigenbook
