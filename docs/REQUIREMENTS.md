# Requirements

This document lists what Eigenbook must do (functional requirements), how well it must do it
(non-functional requirements), and what is needed to build it (system requirements). Every
requirement has a stable ID and is traced to the tests that verify it.

The keywords **MUST**, **MUST NOT**, **SHOULD** and **MAY** are used as defined in
[RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

Status values: **Done** (implemented and tested), **Partial**, **Planned**.

---

## 1. System requirements

### 1.1 Toolchain

| Component | Minimum | Tested with | Notes |
|---|---|---|---|
| C++ standard | C++23 | C++23 | Uses `std::expected`, concepts, designated initialisers |
| CMake | 3.25 | 4.0.1 | `FILE_SET HEADERS` (3.23), `SYSTEM` on `FetchContent_Declare` and presets schema 6 (3.25) |
| MSVC | 19.38 (VS 2022 17.8) | 19.44 (VS 2022 Build Tools 17.14) | Windows |
| GCC | 13 | – | Linux; first release with `std::expected` |
| Clang | 17 with libc++ or libstdc++ 13 | 21.1.8 (Windows, MSVC STL) | Linux / macOS / Windows |
| Build tool | Ninja (recommended), Make, or Visual Studio generator | Ninja 1.12, VS 2022 | |
| Git | any | – | Needed by CMake to fetch GoogleTest (tests only) |

### 1.2 Dependencies

| Dependency | Version | Scope | How it is obtained |
|---|---|---|---|
| C++ standard library | C++23 | Library | Shipped with the compiler |
| Operating-system libraries | – | `Eigenbook::platform` | Windows: `powrprof` (power plan name); Linux: `Threads::Threads` |
| [GoogleTest](https://github.com/google/googletest) | 1.15.2 (commit `b514bdc898e2951020cbdca1304b75f5950d1f59`) | Tests only | CMake `FetchContent`, pinned by SHA in `cmake/Dependencies.cmake`; offline via `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` |

The libraries have **no third-party runtime dependencies**. Google Benchmark (planned for
`bench/micro`) is not used yet: it could not be fetched, nor its commit SHA verified, in the
environment Phase A2 was built in.

### 1.3 Platforms

| Platform | Status |
|---|---|
| Windows 10/11 x64 (MSVC, Clang) | Built and tested (MSVC 19.44, Clang 21.1.8; Debug, Release, ASan, ASan+UBSan) |
| Linux x86-64 (GCC / Clang) | Supported by the build, not yet tested; intended production platform (CPU affinity, NUMA) |
| macOS (Apple Clang) | Expected to work; not tested |

---

## 2. Functional requirements

### 2.1 Orders and validation

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-01 | The book MUST accept LIMIT and MARKET orders on the BUY and SELL sides. | Done | `OrderBookTest.*` |
| FR-02 | Prices MUST be integer ticks (`int64`); floating point MUST NOT be used for prices. | Done | `Price.ArithmeticIsInTicks` |
| FR-03 | An order with zero quantity MUST be rejected with `INVALID_QUANTITY`. | Done | `OrderBookTest.RejectsInvalidRequests` |
| FR-04 | An order whose id is already resting MUST be rejected with `DUPLICATE_ORDER_ID`, before any matching takes place. | Done | `OrderBookTest.RejectsInvalidRequests`, `OrderBookTest.DuplicateIdIsRejectedBeforeAnyMatching` |
| FR-05 | The id of an order that has been filled or cancelled MAY be reused. | Done | `OrderBookTest.FilledOrderIdCanBeReused`, `OrderBookTest.CancelledOrderIdCanBeReused` |
| FR-06 | With the dense ladder, a limit price outside the configured tick range MUST be rejected with `PRICE_OUT_OF_RANGE`; market orders are not range-checked. | Done | `DenseOrderBook.RejectsPricesOutsideTickRange`, `ArrayPriceLadder.AcceptsExactlyItsTickRange` |
| FR-07 | With the map ladder, any `int64` price MUST be accepted. | Done | `OrderBook.AcceptsNegativeAndVeryLargePrices`, `MapPriceLadder.AcceptsAnyPrice` |

### 2.2 Matching

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-10 | Matching MUST follow price-time priority: best price first, then arrival order (FIFO) within a price. | Done | `OrderBookTest.SweepFollowsPriceThenTimePriorityAtMakerPrices`, `OrderBookDifferential.MapDenseAndReferenceAgree` |
| FR-11 | Every execution MUST take place at the resting (maker) order's price. | Done | `OrderBookTest.SweepFollowsPriceThenTimePriorityAtMakerPrices` |
| FR-12 | A limit order MUST NOT trade at a price worse than its limit. | Done | `OrderBookTest.LimitDoesNotTradeWhenNotCrossing`, `OrderBookTest.SweepStopsAtLimitAndRestsRemainder` |
| FR-13 | The unfilled remainder of a limit order MUST rest in the book. | Done | `OrderBookTest.PartialFillOfTakerRestsRemainder` |
| FR-14 | The unfilled remainder of a market order MUST be cancelled and MUST NOT rest. | Done | `OrderBookTest.MarketOrderSweepsAndCancelsRemainder`, `OrderBookTest.MarketOrderOnEmptyBookIsCancelled` |
| FR-15 | A partially filled maker MUST keep its place at the front of its level. | Done | `OrderBookTest.PartialFillOfMakerKeepsItAtFrontWithReducedQuantity` |
| FR-16 | A price level MUST be removed as soon as it becomes empty. | Done | `OrderBookTest.ExactFillRemovesMakerAndLevel`, `PriceLadderTest.RemovingLastOrderDropsLevelAndMovesBest` |
| FR-17 | One fill event MUST be reported per maker/taker execution, carrying maker id, taker id, price, quantity, taker side, and whether the maker left the book. | Done | `OrderBookTest.ExactFillRemovesMakerAndLevel` |
| FR-18 | For every accepted request, `filled + resting + cancelled` MUST equal the requested quantity. | Done | `OrderBookTest.ExecutionResultAccountsForWholeQuantity`, `OrderBookDifferential.MapDenseAndReferenceAgree` |
| FR-19 | After any operation the book MUST NOT be crossed (best bid < best ask). | Done | `OrderBookDifferential.MapDenseAndReferenceAgree` |

### 2.3 Order management

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-20 | Cancelling a resting order MUST remove it and return its open quantity. | Done | `OrderBookTest.CancelRemovesOrderAndEmptyLevel`, `OrderBookTest.CancelReturnsOnlyTheOpenQuantity` |
| FR-21 | Cancelling or modifying an unknown id MUST be rejected with `UNKNOWN_ORDER_ID`. | Done | `OrderBookTest.CancelRemovesOrderAndEmptyLevel`, `OrderBookTest.ModifyRejectsUnknownAndZero` |
| FR-22 | An order MUST be cancellable from any position in its queue. | Done | `OrderBookTest.CancelFromMiddleAndBackOfQueue`, `PriceLevel.EraseFromHeadMiddleAndTail` |
| FR-23 | Reducing an order's quantity at the same price MUST keep its time priority. | Done | `OrderBookTest.ModifyDownKeepsTimePriority`, `OrderBookTest.ModifyToSameQuantityKeepsTimePriority` |
| FR-24 | Increasing quantity or changing price MUST lose time priority; the order re-enters and MAY trade. | Done | `OrderBookTest.ModifyUpLosesTimePriority`, `OrderBookTest.ModifyPriceMovesLevelAndCanTrade`, `OrderBookTest.ModifyThatFullyFillsLeavesNoRestingOrder` |
| FR-25 | A rejected modify MUST leave the order unchanged. | Done | `OrderBookTest.ModifyRejectsUnknownAndZero`, `DenseOrderBook.RejectsPricesOutsideTickRange` |

### 2.4 Market data

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-30 | The book MUST expose the best bid and best ask, or none when a side is empty. | Done | `OrderBookTest.EmptyBookHasNoBestPrices`, `OrderBookTest.NonCrossingLimitsRestAndSetBestPrices` |
| FR-31 | The book MUST expose per-level aggregate quantity and order count. | Done | `OrderBookTest.LevelTracksAggregateQuantityAndFifo` |
| FR-32 | Levels MUST be iterable from best to worst on each side. | Done | `OrderBookTest.ForEachLevelVisitsBestToWorst`, `PriceLadderTest.BestIsHighestBidOrLowestAsk` |

### 2.5 Price ladders

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-40 | Two interchangeable ladder strategies MUST be provided: a `std::map` ladder for sparse prices and a fixed-array + bitset ladder for dense, bounded tick ranges. | Done | `PriceLadderTest.*` (both strategies × both sides) |
| FR-41 | Both strategies MUST produce identical fills and book state for the same input. | Done | `OrderBookDifferential.MapDenseAndReferenceAgree` |
| FR-42 | The dense ladder MUST find the next best level correctly across arbitrarily wide gaps. | Done | `DenseOrderBook.BestPriceRecoversAcrossWideGaps`, `LevelBitmap.FindNextAndPrevMatchReferenceSet` |

### 2.6 Engine (planned)

| ID | Requirement | Status |
|---|---|---|
| FR-50 | Symbols MUST be resolved to stable `InstrumentId` values on the cold path. | Planned |
| FR-51 | Each instrument MUST be routed to exactly one shard. | Planned |
| FR-52 | Each shard MUST be processed by one dedicated thread that owns all of its books. | Planned |
| FR-53 | Orders MUST reach their shard through a per-shard dispatch queue. | Planned |

---

## 3. Non-functional requirements

| ID | Category | Requirement | Status | Verified by |
|---|---|---|---|---|
| NFR-01 | Performance | Cancel and modify MUST locate the order in O(1) through the order index. | Done | Design; `OrderIndex.MatchesUnorderedMapUnderChurn` |
| NFR-02 | Performance | Matching MUST NOT allocate memory; resting orders come from a pre-reserved pool. After warm-up, no operation of a book sized for its peak load may allocate. | Partial | `ZeroAllocation.DenseBookMeasuredPartOfEveryWorkload` (0 allocations in all 8 workloads); `ZeroAllocation.MapBookMeasuredPartOfEveryWorkload` (0 in 7 workloads; **known violation: map ladder in W2-drift**, where new peaks in the number of live levels make the pmr node pool fetch from upstream; fix planned in A3); controls `ZeroAllocation.CountingAllocatorSeesAllocations`, `ZeroAllocation.MapLadderGrowthIsCounted`; `ObjectPool.RecyclesSlotsWithoutGrowing`. Beyond the reserved order count, the pool and index still grow (B1: hard limits). |
| NFR-03 | Performance | An `Order` node MUST occupy exactly one 64-byte cache line. | Done | `static_assert` in `order.h`; `Order.OccupiesExactlyOneCacheLine` |
| NFR-04 | Performance | Best bid / ask lookup MUST be O(1) for the dense ladder. | Done | Design |
| NFR-05 | Concurrency | An order book MUST be mutated by a single thread and MUST NOT take locks on the hot path. | Done (book); Planned (engine) | Design |
| NFR-06 | Determinism | The same input sequence MUST always produce the same fills and book state. | Done | `OrderBookDifferential.MapDenseAndReferenceAgree` (fixed seeds) |
| NFR-07 | Type safety | Prices, quantities, ids and timestamps MUST be distinct strong types. | Done | `types.h`; `Price.*`, `Quantity.*`, `StrongIds.*` |
| NFR-08 | Error handling | Business rejections MUST be returned as values (`std::expected`), not thrown. | Done | `OrderBookTest.RejectsInvalidRequests` |
| NFR-09 | Portability | The code MUST build without warnings under the `eigenbook_warnings` set (`/W4 /permissive- /Zc:__cplusplus`; `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast`), except documented, locally scoped suppressions. | Done | Builds with `EIGENBOOK_WARNINGS_AS_ERRORS=ON` on MSVC 19.44 and Clang 21; the only suppression is C4324 in `object_pool.hpp` (intentional alignment padding, commented). GCC not yet tested. |
| NFR-10 | Quality | Every functional requirement marked Done MUST be covered by at least one automated test. | Done | This document |
| NFR-11 | Packaging | The library MUST be consumable from an install tree with `find_package(Eigenbook CONFIG)`, providing `Eigenbook::book`, `Eigenbook::platform` and `Eigenbook::eigenbook`. | Done | CTest `package_consumer` (installs the build, then configures, builds and runs `tests/package_consumer` against the install prefix only; the umbrella executable calls into the compiled platform library) |
| NFR-12 | Packaging | The order book MUST be header-only: using it MUST NOT require linking a compiled library. Every member of the supported book and ladder instantiations MUST still be compiled by the test build. | Done | `Eigenbook::book` is an `INTERFACE` target; `tests/unit/instantiation_test.cpp` explicitly instantiates `BasicOrderBook<MapPriceLadder>`, `BasicOrderBook<ArrayPriceLadder>`, both ladders on both sides, and `ObjectPool<Order>` |
| NFR-13 | Measurement | Latency MUST be measured per operation with serialised TSC reads (`lfence; rdtsc` … `rdtscp; lfence`) and reported per operation class as percentiles (p50 … p99.99, max), never as one mixed mean. Nanoseconds MUST only be reported with an invariant TSC calibrated against `steady_clock` over ≥ 1 s; the empty-timer overhead MUST be reported. | Done | `eigenbook_latency` (design, see `docs/BENCHMARKS.md`); `Platform.TscCalibrationIsPlausibleAndStable`, `Platform.TscIsMonotonicOnOneCpu` |
| NFR-14 | Measurement | The latency histogram MUST resolve values to 3 significant digits (relative bucket width ≤ 2⁻¹⁰), keep count, min, max and mean exact, and MUST NOT allocate while recording. | Done | `LatencyHistogram.*`; `ZeroAllocation.HistogramRecordDoesNotAllocate` |
| NFR-15 | Measurement | Benchmark workloads MUST be deterministic for a given seed, and every operation's outcome (class and number of fills) MUST be predicted at generation time and reproduced by both book types, so the live-order set is correct on fills. | Done | `Workloads.SameSeedGivesTheSameStream`, `Workloads.ReplayOnBothBooksMatchesThePrediction`, `Workloads.CancelHeavyTargetsTheQueuePositionItClaims`, `Workloads.LevelChurnMeetsItsTarget`, `Workloads.CollidingIdsShareOneHomeSlotAndFormOneCluster`, `Workloads.MicroburstScheduleIsOrderedAndHasTheConfiguredMeanRate`, `Workloads.UnhashInvertsTheIndexHash`; the harness also counts outcome mismatches on every run |
| NFR-16 | Platform | Platform code MUST be isolated in `Eigenbook::platform` with Windows and Linux implementations, and MUST pin a thread to a chosen logical CPU, report each CPU's core type on hybrid Intel CPUs, and choose a P-core away from CPU 0 for benchmarks. | Partial | `Platform.*` on Windows. The Linux implementation is written but has not been compiled or run (no Linux toolchain in the environment it was written in). |
| NFR-17 | Measurement | Order-index probe statistics (find / insert / erase probe lengths, backward-shift scan and move counts) MUST be available behind `EIGENBOOK_INSTRUMENT_INDEX` and MUST NOT change the code of the default build. | Done | `IndexInstrumentation.*` (separate executable); `OrderIndex.MaxClusterLengthCountsTheLongestRunAcrossTheWrap`, `OrderIndex.CapacityForIsWhatReserveAllocates`; identical `clang -O2` assembly for `find`/`insert`/`erase`/`reserve` before and after the change (checked once, Phase A2) |

---

## 4. Change process

When behaviour changes:

1. Update or add the requirement here, keeping existing IDs stable (never reuse a retired ID).
2. Add or update the tests listed in **Verified by**.
3. Run the full test suite before committing, in Debug, Release and a sanitizer preset where available
   (e.g. `ctest --preset dev-debug`, `ctest --preset dev-release`, `ctest --preset asan-ubsan`; on
   Windows `msvc-debug`, `msvc-release`, `msvc-asan`). See [CONTRIBUTING.md](../CONTRIBUTING.md).
