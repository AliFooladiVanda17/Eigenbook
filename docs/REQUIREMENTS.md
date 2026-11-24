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
| FR-06 | With the dense ladder, a limit price outside the configured tick range MUST be rejected with `PRICE_OUT_OF_RANGE`; market orders are not range-checked. The range check MUST be defined for every int64 price (no signed overflow). | Done | `DenseOrderBook.RejectsPricesOutsideTickRange`, `ArrayPriceLadder.AcceptsExactlyItsTickRange`, `Limits.DenseLadderHandlesExtremePricesWithoutOverflow` (UBSan) |
| FR-07 | With the map ladder, any `int64` price MUST be accepted. | Done | `OrderBook.AcceptsNegativeAndVeryLargePrices`, `MapPriceLadder.AcceptsAnyPrice` |
| FR-08 | A limit order that would need a new price level when its side of the book is at capacity (`maxLevels` of a bounded ladder) MUST be rejected with `CAPACITY_EXCEEDED` before any matching takes place; the book MUST be unchanged. Market orders never rest and are not capacity-limited. (Conservative: a limit that would trade in full is rejected too.) | Done | `CapacityTest.NewLevelBeyondCapacityIsRejected`, `CapacityTest.RejectHappensBeforeAnyMatching`, `CapacityTest.FreedLevelCanBeReused` (map M0 with a bound, M1, M2, hybrid) |
| FR-09 | Per-instrument limits (`BookLimits`) MUST be checked before any mutation: order quantity above `maxOrderQuantity` → `ORDER_QUANTITY_LIMIT`; limit price outside `[minPrice, maxPrice]` → `PRICE_OUT_OF_BAND`; absolute price × quantity above `maxNotional` (computed exactly, without overflow) → `NOTIONAL_LIMIT`; a level aggregate that would exceed `maxLevelQuantity` → `LEVEL_QUANTITY_LIMIT`; a new resting order beyond `maxOrders` → `CAPACITY_EXCEEDED`. The defaults keep every quantity sum below 2^64. | Done | `LimitsTest.*` (map, dense, hybrid), `CheckedMath.NotionalIsExactAndNeverOverflows` |

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
| FR-26 | A modify that would need a new price level the ladder cannot provide, even after removing the order itself, MUST be rejected with `CAPACITY_EXCEEDED` and MUST leave the order unchanged (price, quantity, queue position). | Done | `CapacityTest.ModifyToNewLevelWhenFullIsRejectedAndLeavesTheOrderAlone`, `CapacityTest.ModifyOfALoneOrderToANewLevelFitsBecauseItFreesItsOwn` |
| FR-27 | A book whose locator issues order ids (`DirectOrderTable`) MUST accept a new order only with a handle it reserved and that is not in use (`INVALID_ORDER_ID` otherwise, before any matching); a cancel or modify with a stale or foreign handle MUST fail (`UNKNOWN_ORDER_ID`); the handle of an order that did not rest MUST be released; a modify MUST keep the handle; `reserveOrderId()` MUST return `CAPACITY_EXCEEDED` when every slot is in use. | Done | `DirectBook.*`, `DirectOrderTable.*`, `OrderBookDifferential.DirectHandlesAgreeWithTheReference`, `Workloads.ReplayOnBothBooksMatchesThePrediction` (direct book) |
| FR-28 | With the hardened index, an id that would be stored more than `maxProbe` slots from its home MUST be rejected with `ORDER_ID_REJECTED` before any matching, and counted; when the index is at load 1/2 a new order MUST be rejected with `CAPACITY_EXCEEDED` before any matching; the index MUST NOT grow. | Done | `HardenedOrderIndex.*`, `HardenedBook.FullIndexIsACapacityRejectBeforeMatching`, `OrderBookTest.*` and differential tests (hardened book) |
| FR-29 | A modify that re-enters the order MUST be all-or-nothing: every check of the new price and quantity (FR-06, FR-08, FR-09, FR-26), counting the level, level quantity and storage the order itself frees, happens before the order is touched; after that the re-entry cannot be rejected. (With map ladder M0 a tree-node allocation failure can still throw.) | Done | `LimitsTest.*` (modify cases), `CapacityTest.ModifyToNewLevelWhenFullIsRejectedAndLeavesTheOrderAlone`, differential tests |
| FR-33 | Qualifiers that do not fit together MUST be rejected with `INVALID_QUALIFIERS` before the id is admitted: an unknown time in force, a minimum above the quantity, post-only on a MARKET, IOC or FOK order or with a minimum quantity. | Done | `TimeInForceTest.InvalidCombinationsAreRejectedFirst` |
| FR-34 | A resting order MUST carry the book's priority sequence (monotonic per book), stamped when it rests or re-enters and kept by an in-place reduce; caller timestamps MUST NOT decide priority. | Done | `LimitsTest.RestingOrdersCarryTheBookPrioritySequence` |
| FR-35 | Every order MUST carry its owner (`ParticipantId`, from the request). A participant cancel (`cancelOrder(id, participant)`) or a modify by anyone but the owner MUST be rejected with `NOT_OWNER` and leave the order unchanged; an unknown id is `UNKNOWN_ORDER_ID` whoever asks. The administrative `cancelOrder(id)` (engine, risk) MUST NOT check ownership. A re-entering modify keeps the owner. | Done | `ParticipantTest.*` (all client-id books), `Participant.DirectHandlesCheckOwnershipToo` |
| FR-36 | Every fill MUST carry a trade id, per book, starting at 1 and increasing by exactly one per execution (also across modify re-entries); rejects and non-crossing orders MUST NOT consume one. Every fill MUST name the maker's owner. | Done | `ParticipantTest.FillsCarryTradeIdsAndTheMakersOwner`, differential tests (trade ids against the reference) |
| FR-37 | Time in force: DAY and GTC limit orders MAY rest (DAY expiry arrives with sessions); an IOC order MUST trade what crosses on arrival and cancel the rest; a FOK order MUST trade its whole quantity on arrival or nothing, and a FOK that cannot fill MUST leave the book untouched (no fill, no trade id). A MARKET order never rests, whatever its time in force. IOC / FOK need no room in the book (no level-quantity or capacity check). | Done | `TimeInForceTest.*` (all client-id books), `TimeInForce.FailedFokReleasesAnIssuedHandle`, `OrderBookDifferential.QualifiersAgreeWithTheReference` |
| FR-38 | A minimum quantity MUST trade on arrival or the whole order MUST be cancelled without touching the book; once it is met the remainder follows the time in force. The decision MUST come from a read-only look at the opposite side (`forEachLevelWhile`). | Done | `TimeInForceTest.MinimumQuantityGatesTheImmediateExecution`, `OrderBookDifferential.QualifiersAgreeWithTheReference` |
| FR-39 | A post-only order that would trade on arrival MUST be rejected with `POST_ONLY_WOULD_TRADE` before any matching. The flag (and the time in force) MUST stay with the resting order, and a modify that would make it trade MUST be rejected the same way, leaving the order unchanged. | Done | `TimeInForceTest.PostOnlyNeverTakesLiquidity`, `TimeInForceTest.ReentryKeepsTimeInForceAndPostOnly`, `OrderBookDifferential.QualifiersAgreeWithTheReference` |
| FR-45 | A book MUST support a halted state: while halted, `addOrder` and `modifyOrder` MUST be rejected with `INSTRUMENT_HALTED` before any other check, and participant and administrative cancels MUST still work. `halt()` / `resume()` control a manual halt; after a failed audit `resume()` MUST refuse and the book MUST stay halted. | Done | `InvariantsTest.HaltRejectsNewOrdersAndModifiesButNotCancels`, `InvariantsTest.ManualAuditHaltsACorruptBookForGood` |

### 2.4 Market data

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-30 | The book MUST expose the best bid and best ask, or none when a side is empty. | Done | `OrderBookTest.EmptyBookHasNoBestPrices`, `OrderBookTest.NonCrossingLimitsRestAndSetBestPrices` |
| FR-31 | The book MUST expose per-level aggregate quantity and order count. | Done | `OrderBookTest.LevelTracksAggregateQuantityAndFifo` |
| FR-32 | Levels MUST be iterable from best to worst on each side. | Done | `OrderBookTest.ForEachLevelVisitsBestToWorst`, `PriceLadderTest.BestIsHighestBidOrLowestAsk` |

### 2.5 Price ladders

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-40 | Interchangeable ladder strategies MUST be provided behind one concept: a `std::map` ladder for sparse prices, a fixed-array + bitset ladder for dense, bounded tick ranges, and a hybrid of the two (FR-44). | Done | `PriceLadderTest.*` (both strategies × both sides) |
| FR-41 | All strategies MUST produce identical fills and book state for the same input. | Done | `OrderBookDifferential.MapDenseAndReferenceAgree`, `OrderBookDifferential.AllLaddersAgreeUnderADriftingMid` (map M0 / M1 / M2, dense, hybrid against a reference model; 50,000 steps by default, 1,000,000 steps x 5 seeds with `EIGENBOOK_LONG_TESTS`) |
| FR-42 | The dense ladder MUST find the next best level correctly across arbitrarily wide gaps. | Done | `DenseOrderBook.BestPriceRecoversAcrossWideGaps`, `LevelBitmap.FindNextAndPrevMatchReferenceSet` |
| FR-43 | The map ladder MUST offer compile-time node-allocation policies: M0 (pool over the heap, unbounded by default), M1 (pool over a preallocated arena with no upstream, pre-warmed for `maxLevels`), M2 (preallocated single-size free list). All MUST behave identically; M1 and M2 MUST require `maxLevels` and MUST hold exactly `maxLevels` levels. | Done | `OrderBookTest.*` and `PriceLadderTest.*` (typed over M0, M1, M2), `OrderBookDifferential.*`, `Capacity.MapPoliciesNeedABound`, `Capacity.BoundedMapsFillToExactlyMaxLevels`, `FixedBlockResource.*` |
| FR-44 | A hybrid ladder (dense ring window around the touch, ordered overflow maps, incremental recentering) MUST produce the same fills and book state as the other ladders for any input, MUST accept any `int64` price, and MUST move at most `maxMovesPerOp` levels per add. | Done | `HybridLadder.*`, `OrderBookTest.*` and `PriceLadderTest.*` (typed, small window), `OrderBookDifferential.AllLaddersAgreeUnderADriftingMid` |

### 2.6 Engine (planned)

| ID | Requirement | Status |
|---|---|---|
| FR-50 | Symbols MUST be resolved to stable `InstrumentId` values on the cold path. | Planned |
| FR-51 | Each instrument MUST be routed to exactly one shard. | Planned |
| FR-52 | Each shard MUST be processed by one dedicated thread that owns all of its books. | Planned |
| FR-53 | Orders MUST reach their shard through a per-shard dispatch queue. | Planned |

### 2.7 Sequencing and the engine core (Phase B3)

| ID | Requirement | Status | Verified by |
|---|---|---|---|
| FR-60 | Every input MUST pass through the sequencer, which gives it a sequence number one greater than the previous input's (from 1, or after the last journaled input on restart) and a timestamp that never decreases, even when the clock steps back. The engine MUST read no other clock. | Done | `Sequencer.*`; no clock in `engine/engine.hpp` or the book (code review, not a test) |
| FR-61 | The engine MUST apply only the next input sequence. A gap (`SEQUENCE_GAP`) or a repeat (`DUPLICATE_SEQUENCE`) MUST be refused without emitting anything and without changing any state. | Done | `Engine.GapsAndDuplicatesAreRefusedWithoutOutput` |
| FR-62 | Every applied input MUST produce its fills, in execution order, followed by exactly one summary event (`ACCEPTED`, `MODIFIED`, `CANCELLED`, `REJECTED` with the reason, `HALTED`, `RESUMED`). Every event MUST carry a gap-free output sequence, the input sequence that caused it and that input's time. | Done | `Engine.NewOrderReportsItsFillsThenItsSummary`, `Engine.CancelAndModifyReportTheirOutcome`, `Determinism.TheSameLogGivesByteIdenticalOutput` |
| FR-63 | A command for an instrument with no registered book MUST be rejected with `UNKNOWN_INSTRUMENT`, and a command of no known type with `INVALID_COMMAND`. Halt and resume MUST be sequenced commands; a cancel command MUST be a participant cancel (FR-35). | Done | `Engine.RejectsAreReportedAndChangeNothing`, `Engine.HaltAndResumeAreSequencedInputs` |
| FR-64 | Instruments MUST be registered on the cold path; an id that is already registered, or not below `Engine::kMaxInstruments`, MUST be refused. | Done | `Engine.InstrumentRegistrationIsChecked` |

---

## 3. Non-functional requirements

| ID | Category | Requirement | Status | Verified by |
|---|---|---|---|---|
| NFR-01 | Performance | Cancel and modify MUST locate the order through the order locator in expected O(1) (hash indexes) or worst-case O(1) (engine-issued handles, NFR-18). | Done | Design; `OrderIndex.MatchesUnorderedMapUnderChurn` |
| NFR-02 | Performance | Matching MUST NOT allocate memory; resting orders come from a pre-reserved pool. After warm-up, no operation of a book sized for its peak load may allocate. | Partial | 0 allocations in the measured part of all 8 workloads for the dense, map-M1, map-M2 and hybrid books: `ZeroAllocation.DenseBookMeasuredPartOfEveryWorkload`, `ZeroAllocation.MapM1BookMeasuredPartOfEveryWorkload`, `ZeroAllocation.MapM2BookMeasuredPartOfEveryWorkload`, `ZeroAllocation.HybridBookMeasuredPartOfEveryWorkload`. **Known violation: the default `OrderBook` (map ladder M0)** in W2-drift, where new peaks in the number of live levels make its pmr node pool fetch from upstream (`ZeroAllocation.MapBookMeasuredPartOfEveryWorkload` asserts it still does); M0 is kept unchanged as the comparison baseline. Controls `ZeroAllocation.CountingAllocatorSeesAllocations`, `ZeroAllocation.MapLadderGrowthIsCounted`; `ObjectPool.RecyclesSlotsWithoutGrowing`. With `BookLimits::maxOrders` > 0 the order pool and locator are preallocated for that many orders and never grow (`LimitsTest.HardOrderLimitRejectsInsteadOfGrowing`); with the default (0) they still grow past `reserveOrders`. |
| NFR-03 | Performance | An `Order` node MUST occupy exactly one 64-byte cache line. | Done | `static_assert`s on size and every field offset in `order.hpp`; `Order.OccupiesExactlyOneCacheLine` |
| NFR-04 | Performance | Best bid / ask lookup MUST be O(1) for the dense ladder. | Done | Design |
| NFR-05 | Concurrency | An order book MUST be mutated by a single thread and MUST NOT take locks on the hot path. | Done (book); Planned (engine) | Design |
| NFR-06 | Determinism | The same input sequence MUST always produce the same fills and book state. | Done | `OrderBookDifferential.MapDenseAndReferenceAgree` (fixed seeds) |
| NFR-07 | Type safety | Prices, quantities, ids and timestamps MUST be distinct strong types. | Done | `types.h`; `Price.*`, `Quantity.*`, `StrongIds.*` |
| NFR-08 | Error handling | Business rejections MUST be returned as values (`std::expected`), not thrown. | Done | `OrderBookTest.RejectsInvalidRequests` |
| NFR-09 | Portability | The code MUST build without warnings under the `eigenbook_warnings` set (`/W4 /permissive- /Zc:__cplusplus`; `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast`), except documented, locally scoped suppressions. | Done | Builds with `EIGENBOOK_WARNINGS_AS_ERRORS=ON` on MSVC 19.44 and Clang 21; the only suppression is C4324 in `object_pool.hpp` (intentional alignment padding, commented). GCC not yet tested. |
| NFR-10 | Quality | Every functional requirement marked Done MUST be covered by at least one automated test. | Done | This document |
| NFR-11 | Packaging | The library MUST be consumable from an install tree with `find_package(Eigenbook CONFIG)`, providing `Eigenbook::book`, `Eigenbook::platform` and `Eigenbook::eigenbook`. Every public header MUST be in an installed FILE_SET. | Done | CTest `package_consumer` (installs the build, then configures, builds and runs `tests/package_consumer` against the install prefix only; the umbrella executable calls into the compiled platform library); configure-time guard in `src/CMakeLists.txt` (fails if a header under `include/eigenbook/` is in no FILE_SET; seen firing in B1) |
| NFR-12 | Packaging | The order book MUST be header-only: using it MUST NOT require linking a compiled library. Every member of the supported book and ladder instantiations MUST still be compiled by the test build. | Done | `Eigenbook::book` is an `INTERFACE` target; `tests/unit/instantiation_test.cpp` explicitly instantiates `BasicOrderBook<MapPriceLadder>`, `BasicOrderBook<ArrayPriceLadder>`, both ladders on both sides, and `ObjectPool<Order>` |
| NFR-13 | Measurement | Latency MUST be measured per operation with serialised TSC reads (`lfence; rdtsc` … `rdtscp; lfence`) and reported per operation class as percentiles (p50 … p99.99, max), never as one mixed mean. Nanoseconds MUST only be reported with an invariant TSC calibrated against `steady_clock` over ≥ 1 s; the empty-timer overhead MUST be reported. | Done | `eigenbook_latency` (design, see `docs/BENCHMARKS.md`); `Platform.TscCalibrationIsPlausibleAndStable`, `Platform.TscIsMonotonicOnOneCpu` |
| NFR-14 | Measurement | The latency histogram MUST resolve values to 3 significant digits (relative bucket width ≤ 2⁻¹⁰), keep count, min, max and mean exact, and MUST NOT allocate while recording. | Done | `LatencyHistogram.*`; `ZeroAllocation.HistogramRecordDoesNotAllocate` |
| NFR-15 | Measurement | Benchmark workloads MUST be deterministic for a given seed, and every operation's outcome (class and number of fills) MUST be predicted at generation time and reproduced by both book types, so the live-order set is correct on fills. | Done | `Workloads.SameSeedGivesTheSameStream`, `Workloads.ReplayOnBothBooksMatchesThePrediction`, `Workloads.CancelHeavyTargetsTheQueuePositionItClaims`, `Workloads.LevelChurnMeetsItsTarget`, `Workloads.CollidingIdsShareOneHomeSlotAndFormOneCluster`, `Workloads.MicroburstScheduleIsOrderedAndHasTheConfiguredMeanRate`, `Workloads.UnhashInvertsTheIndexHash`; the harness also counts outcome mismatches on every run |
| NFR-16 | Platform | Platform code MUST be isolated in `Eigenbook::platform` with Windows and Linux implementations, and MUST pin a thread to a chosen logical CPU, report each CPU's core type on hybrid Intel CPUs, and choose a P-core away from CPU 0 for benchmarks. | Partial | `Platform.*` on Windows. The Linux implementation is written but has not been compiled or run (no Linux toolchain in the environment it was written in). |
| NFR-17 | Measurement | Order-index probe statistics (find / insert / erase probe lengths, backward-shift scan and move counts) MUST be available behind `EIGENBOOK_INSTRUMENT_INDEX` and MUST NOT change the code of the default build. | Done | `IndexInstrumentation.*` (separate executable); `OrderIndex.MaxClusterLengthCountsTheLongestRunAcrossTheWrap`, `OrderIndex.CapacityForIsWhatReserveAllocates`; identical `clang -O2` assembly for `find`/`insert`/`erase`/`reserve` before and after the change (checked once, Phase A2) |
| NFR-18 | Performance | With engine-issued handles, finding an order for cancel or modify MUST take one array index and one generation compare, whatever the id values (O(1) worst case, no hashing or probing). | Done | `DirectOrderTable::find` (design); W4-collide measurement in Phase A4 (`docs/BENCHMARKS.md`) |
| NFR-19 | Security | With client-chosen ids, colliding ids MUST NOT be computable without secret state: the hardened index hashes with a per-instance random seed, and every lookup is bounded by `maxProbe` + 1 slots. | Done | `HardenedOrderIndex.CraftedCollisionsHitTheProbeGuardOnlyWithAKnownSeed`, `HardenedOrderIndex.MatchesUnorderedMapUnderChurn`; the default `OrderIndex` does not meet it (public, invertible hash; see W4-collide) |
| NFR-20 | Robustness | `checkInvariants()` MUST verify, read-only: each ladder's structure (levels keyed by their own price, no empty registered level, dense bitmap / count / best index, hybrid window and maps) and every queue (links in both directions, head and tail, owner, price, non-zero quantities, count, aggregate); orders on the right side, found by the locator, with strictly increasing priority stamps, inside `BookLimits`; the order count; an uncrossed book. It MUST report the first violation found. | Done | `InvariantsTest.*` (all client-id books; corruption simulated through `PriceLevel`), every differential test (full audit every 4,096 steps) |
| NFR-21 | Robustness | With `BookLimits::auditEvery = N`, the book MUST run the full check after every N-th accepted mutation and halt itself (`AUDIT_FAILED`) on a violation; rejected requests MUST NOT count. With `auditEvery = 0` (default) the cost MUST be one compare per mutation. | Done | `Invariants.PeriodicAuditRunsEveryNMutations`, `Invariants.RejectsAndHaltsDoNotCountAsMutations`; the cost with auditing off by reading `afterMutation()` (not measured on its own) |
| NFR-22 | Verification | The matching rules MUST have a formal specification (TLA+) that is model-checked on bounded models, with every invariant shown to be falsifiable by a seeded fault; the bounds, tool version and results of every recorded run MUST be documented. Conformance of the C++ book to the specification is NFR-23. | Done (B2.1) | `formal/tla/EigenbookMatching.tla` with TLC runs recorded in `formal/README.md`; `formal/tla/mutation_check.sh` (14 of 14 mutants caught) |
| NFR-23 | Verification | Every book type MUST be checkable against the TLA+ specification: a recorder outside the book writes each request and the state after it as an NDJSON trace under the abstraction function in `formal/README.md`, and TLC MUST confirm that each trace is a behaviour of the specification, step by step. Every run MUST also show that the check is not vacuous: corrupted traces MUST be rejected at the corrupted step. The tracing MUST NOT add code to the book's hot path. | Done (B2.2) | `formal_trace_validation` (CTest label `formal`, needs `EIGENBOOK_TLA_TOOLS_JAR`); recorded run in `formal/README.md` (21 of 21 traces accepted, 5 of 5 corruptions rejected at the corrupted step); `TraceFormat.*` |
| NFR-24 | Determinism | The engine's output MUST be a function of its input log: the same log MUST give a byte-identical output stream on a fresh engine and on every book type. `Command`, `SequencedCommand` and `OutputEvent` MUST be fixed-layout values without padding bytes, so that byte comparison is meaningful and the journal can store them as they are. | Done | `Determinism.TheSameLogGivesByteIdenticalOutput`, `Determinism.EveryBookTypeGivesTheSameOutput` (20,000 inputs, three instruments, map M0 / M2, dense, hybrid); `static_assert`s on size, offsets and `has_unique_object_representations` in `engine/command.hpp` and `engine/output_event.hpp` |

---

## 4. Change process

When behaviour changes:

1. Update or add the requirement here, keeping existing IDs stable (never reuse a retired ID).
2. Add or update the tests listed in **Verified by**.
3. Run the full test suite before committing, in Debug, Release and a sanitizer preset where available
   (e.g. `ctest --preset dev-debug`, `ctest --preset dev-release`, `ctest --preset asan-ubsan`; on
   Windows `msvc-debug`, `msvc-release`, `msvc-asan`). See [CONTRIBUTING.md](../CONTRIBUTING.md).
