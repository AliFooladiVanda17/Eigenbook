//
// FixedBlockResource (map-ladder policy M2): single-size preallocated free list.
//

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <eigenbook/util/fixed_block_resource.hpp>

namespace eigenbook {

namespace {

// Counts what reaches it; forwards to new/delete.
class CountingFallback final : public std::pmr::memory_resource {
public:
    int allocations{0};
    int deallocations{0};

private:
    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, const std::size_t bytes, const std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        return this == &o;
    }
};

} // namespace

TEST(FixedBlockResource, HandsOutDistinctAlignedBlocksUpToCapacity) {
    CountingFallback fallback;
    FixedBlockResource r(40, 8, &fallback);
    EXPECT_EQ(r.blockSize() % alignof(std::max_align_t), 0u);
    EXPECT_GE(r.blockSize(), 40u);
    std::set<void*> seen;
    for (int i = 0; i < 8; ++i) {
        void* p = r.allocate(40, 8);
        EXPECT_TRUE(r.owns(p));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t), 0u);
        EXPECT_TRUE(seen.insert(p).second);
    }
    EXPECT_EQ(r.available(), 0u);
    EXPECT_THROW((void)r.allocate(40, 8), std::bad_alloc);
    EXPECT_EQ(fallback.allocations, 0);
    for (void* p : seen) {
        r.deallocate(p, 40, 8);
    }
    EXPECT_EQ(r.available(), 8u);
}

TEST(FixedBlockResource, RecyclesBlocksWithoutTouchingTheFallback) {
    CountingFallback fallback;
    FixedBlockResource r(64, 4, &fallback);
    // alignof(std::max_align_t): 8 with the MSVC ABI, 16 on Linux x86-64.
    constexpr std::size_t kAlign = alignof(std::max_align_t);
    std::vector<void*> live;
    for (int round = 0; round < 10'000; ++round) {
        if (live.size() < 4 && (round % 3 != 0 || live.empty())) {
            live.push_back(r.allocate(64, kAlign));
        } else {
            r.deallocate(live.back(), 64, kAlign);
            live.pop_back();
        }
    }
    EXPECT_EQ(fallback.allocations, 0);
    EXPECT_EQ(r.available() + live.size(), 4u);
}

TEST(FixedBlockResource, OversizeOrOveralignedRequestsGoToTheCountedFallback) {
    CountingFallback fallback;
    FixedBlockResource r(32, 2, &fallback);
    void* big = r.allocate(r.blockSize() + 1, 8);
    EXPECT_FALSE(r.owns(big));
    EXPECT_EQ(r.fallbackAllocations(), 1u);
    r.deallocate(big, r.blockSize() + 1, 8);
    EXPECT_EQ(fallback.deallocations, 1);
    void* aligned = r.allocate(16, 2 * alignof(std::max_align_t));
    EXPECT_FALSE(r.owns(aligned));
    r.deallocate(aligned, 16, 2 * alignof(std::max_align_t));
    EXPECT_EQ(r.fallbackAllocations(), 2u);
    EXPECT_EQ(r.available(), 2u);
}

} // namespace eigenbook
