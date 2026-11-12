//
// Zero-allocation proof (NFR-02): replaces the global operator new / delete with counting
// versions, then runs the measured part of every benchmark workload on both books and requires
// that not a single allocation happens. Book construction, workload generation and warm-up are
// outside the counted window, exactly as in the latency benchmark.
//
// This is its own executable because replacing operator new affects the whole program.
//
// Two allocation paths are counted:
//   1. global operator new / delete, replaced below;
//   2. the std::pmr default memory resource, replaced by a counting resource. The map ladder's
//      pool resource takes its upstream from it. This second path is needed because on Windows
//      with the DLL runtime (/MD) the pmr resources are implemented inside the STL DLL, and their
//      operator new calls do not reach a replacement defined in this executable (observed with
//      MSVC 19.44's STL: without it, growing the map ladder counted zero allocations).
//

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <harness/executor.hpp>
#include <harness/histogram.hpp>
#include <workloads/w1_baseline.hpp>
#include <workloads/w2_level_churn.hpp>
#include <workloads/w3_cancel_heavy.hpp>
#include <workloads/w4_adversarial_index.hpp>
#include <workloads/w5_microburst.hpp>
#include <workloads/workload.hpp>

#include <eigenbook/book/order_book.hpp>

// --- Counting global allocation functions --------------------------------------------------------

namespace {

std::atomic<bool> gArmed{false};
std::atomic<std::uint64_t> gAllocations{0};
std::atomic<std::uint64_t> gDeallocations{0};

void* allocate(std::size_t size) {
    if (gArmed.load(std::memory_order_relaxed)) {
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) {
    if (gArmed.load(std::memory_order_relaxed)) {
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    const auto align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = (size + align - 1) / align * align;
#if defined(_MSC_VER)
    return _aligned_malloc(rounded == 0 ? align : rounded, align);
#else
    return std::aligned_alloc(align, rounded == 0 ? align : rounded);
#endif
}

void release(void* p) noexcept {
    if (p != nullptr && gArmed.load(std::memory_order_relaxed)) {
        gDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void releaseAligned(void* p) noexcept {
    if (p != nullptr && gArmed.load(std::memory_order_relaxed)) {
        gDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

void* operator new(std::size_t size) {
    if (void* p = allocate(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (void* p = allocate(size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (void* p = allocateAligned(size, alignment)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    if (void* p = allocateAligned(size, alignment)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(size, alignment);
}

void operator delete(void* p) noexcept {
    release(p);
}

void operator delete[](void* p) noexcept {
    release(p);
}

void operator delete(void* p, std::size_t) noexcept {
    release(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    release(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    release(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    release(p);
}

void operator delete(void* p, std::align_val_t) noexcept {
    releaseAligned(p);
}

void operator delete[](void* p, std::align_val_t) noexcept {
    releaseAligned(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    releaseAligned(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    releaseAligned(p);
}

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    releaseAligned(p);
}

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    releaseAligned(p);
}

// --- Tests
// -----------------------------------------------------------------------------------------

namespace eigenbook::bench {

namespace {

// Counts what reaches the pmr default resource, then forwards to new_delete_resource().
class CountingResource final : public std::pmr::memory_resource {
    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
        if (gArmed.load(std::memory_order_relaxed)) {
            gAllocations.fetch_add(1, std::memory_order_relaxed);
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, const std::size_t bytes, const std::size_t alignment) override {
        if (gArmed.load(std::memory_order_relaxed)) {
            gDeallocations.fetch_add(1, std::memory_order_relaxed);
        }
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

// Installed before any test constructs a book, so every pmr pool's upstream is counted.
class CountingDefaultResource : public ::testing::Environment {
public:
    void SetUp() override { previous = std::pmr::set_default_resource(&resource); }

    void TearDown() override { std::pmr::set_default_resource(previous); }

private:
    CountingResource resource;
    std::pmr::memory_resource* previous{nullptr};
};

[[maybe_unused]] ::testing::Environment* const kCountingEnvironment =
    ::testing::AddGlobalTestEnvironment(new CountingDefaultResource);

struct Counted {
    std::uint64_t allocations{0};
    std::uint64_t deallocations{0};
};

template <typename F>
Counted counted(F&& f) {
    gAllocations.store(0);
    gDeallocations.store(0);
    gArmed.store(true);
    std::forward<F>(f)();
    gArmed.store(false);
    return {gAllocations.load(), gDeallocations.load()};
}

// The benchmark's warm-up length; smaller measured parts keep Debug and sanitizer runs short.
constexpr WorkloadSize kSize{.warmupOps = 200'000, .measuredOps = 200'000};

struct Case {
    std::string name;
    std::function<Workload()> make;
};

std::vector<Case> cases() {
    return {
        {"w1",
         [] { return makeBaseline({.seed = 42, .totalOps = 400'000, .warmupOps = 200'000}); }},
        {"w2-churn",
         [] {
             LevelChurnConfig c;
             c.size = kSize;
             return makeLevelChurn(c);
         }},
        {"w2-drift",
         [] {
             LevelChurnConfig c = levelChurnDrift(1);
             c.size = kSize;
             return makeLevelChurn(c);
         }},
        {"w3-cancel70", [] { return makeCancelHeavy({.size = kSize, .cancelFraction = 0.70}); }},
        {"w3-cancel90", [] { return makeCancelHeavy({.size = kSize, .cancelFraction = 0.90}); }},
        {"w4-collide",
         [] {
             return makeAdversarialIndex({.size = {.warmupOps = 20'000, .measuredOps = 20'000},
                                          .mode = IndexAttack::COLLIDE});
         }},
        {"w4-load50",
         [] { return makeAdversarialIndex({.size = kSize, .mode = IndexAttack::LOAD_HALF}); }},
        {"w5-burst",
         [] {
             MicroburstConfig c;
             c.size = kSize;
             return makeMicroburst(c);
         }},
    };
}

// Known NFR-02 violations: (book, workload) pairs that currently allocate in the measured part.
// Each one is asserted to still allocate, so fixing it (Phase A3: preallocated map-ladder node
// resources) makes this test fail until the entry is removed, and a new violation is never
// silently accepted.
//
// map / w2-drift: long-lived orders left behind a trending mid keep raising the number of live
// price levels, and every new high makes the map ladder's pmr pool fetch a chunk from upstream.
struct KnownViolation {
    std::string_view book;
    std::string_view workload;
};

constexpr std::array<KnownViolation, 1> kKnownViolations{{{"map", "w2-drift"}}};

bool isKnownViolation(const std::string_view book, const std::string_view workload) {
    return std::ranges::any_of(kKnownViolations, [&](const KnownViolation& k) {
        return k.book == book && k.workload == workload;
    });
}

template <typename Book>
void expectNoAllocationInMeasuredPart(const Workload& w,
                                      std::unique_ptr<Book> book,
                                      const std::string_view bookName) {
    std::uint64_t mismatches = 0;
    std::uint32_t fills = 0;
    for (std::size_t i = 0; i < w.warmupOps; ++i) {
        fills = 0;
        const bool ok = runOp(*book, w.ops[i], fills);
        mismatches += outcomeMatches(w.ops[i], ok, fills) ? 0 : 1;
    }
    const Counted c = counted([&] {
        for (std::size_t i = w.warmupOps; i < w.ops.size(); ++i) {
            fills = 0;
            const bool ok = runOp(*book, w.ops[i], fills);
            mismatches += outcomeMatches(w.ops[i], ok, fills) ? 0 : 1;
        }
    });
    if (isKnownViolation(bookName, w.name)) {
        EXPECT_GT(c.allocations, 0u) << bookName << " / " << w.name
                                     << " no longer allocates: remove it from kKnownViolations and "
                                        "update NFR-02 in docs/REQUIREMENTS.md";
        std::cout << "[ KNOWN    ] " << bookName << " / " << w.name << ": " << c.allocations
                  << " allocations in the measured part (NFR-02 violation, fixed in A3)\n";
    } else {
        EXPECT_EQ(c.allocations, 0u)
            << bookName << " / " << w.name << ": allocations in the measured part";
        EXPECT_EQ(c.deallocations, 0u)
            << bookName << " / " << w.name << ": deallocations in the measured part";
    }
    EXPECT_EQ(mismatches, 0u) << w.name;
}

} // namespace

TEST(ZeroAllocation, CountingAllocatorSeesAllocations) {
    // Guards against the replacement not being linked in: then every other test would pass
    // vacuously.
    const Counted c = counted([] {
        auto p = std::make_unique<std::uint64_t[]>(16);
        auto q = std::make_unique<eigenbook::Order>(OrderId{1},
                                                    OrderSide::BUY,
                                                    OrderType::LIMIT,
                                                    Price{1},
                                                    Quantity{1},
                                                    Timestamp{1}); // over-aligned new
        (void)p;
        (void)q;
    });
    EXPECT_EQ(c.allocations, 2u);
    EXPECT_EQ(c.deallocations, 2u);
}

TEST(ZeroAllocation, MapLadderGrowthIsCounted) {
    // Control: the map ladder's nodes come from a pmr pool whose upstream is operator new, so a
    // book that grows to new price levels must show up here. Otherwise the zero counts below would
    // prove nothing about the map ladder.
    OrderBook book(InstrumentId{1}, MapLadderConfig{}, 20'000);
    const Counted c = counted([&] {
        for (std::uint64_t i = 1; i <= 10'000; ++i) {
            (void)book.addOrder({.id = {i},
                                 .side = OrderSide::BUY,
                                 .type = OrderType::LIMIT,
                                 .price = {static_cast<std::int64_t>(i)},
                                 .quantity = {1}});
        }
    });
    EXPECT_GT(c.allocations, 0u);
}

TEST(ZeroAllocation, HistogramRecordDoesNotAllocate) {
    LatencyHistogram h;
    const Counted c = counted([&] {
        for (std::uint64_t v = 0; v < 2'000'000; v += 7) {
            h.record(v * v);
        }
    });
    EXPECT_EQ(c.allocations, 0u);
}

TEST(ZeroAllocation, MapBookMeasuredPartOfEveryWorkload) {
    for (const Case& k : cases()) {
        const Workload w = k.make();
        expectNoAllocationInMeasuredPart(
            w,
            std::make_unique<OrderBook>(InstrumentId{1}, MapLadderConfig{}, w.expectedOrders),
            "map");
    }
}

TEST(ZeroAllocation, DenseBookMeasuredPartOfEveryWorkload) {
    for (const Case& k : cases()) {
        const Workload w = k.make();
        expectNoAllocationInMeasuredPart(
            w,
            std::make_unique<DenseOrderBook>(
                InstrumentId{1},
                ArrayLadderConfig{.minPrice = w.minPrice, .tickCount = w.tickCount()},
                w.expectedOrders),
            "dense");
    }
}

} // namespace eigenbook::bench
