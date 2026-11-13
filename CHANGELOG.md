# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). There has been
no release yet; the CMake project version is 0.1.0.

## [Unreleased]

### Added

- Time in force and qualifiers: `TimeInForce` {DAY, GTC, IOC, FOK}; `OrderRequest::timeInForce`,
  `postOnly`, `minQuantity`; `Order::getTimeInForce()`, `isPostOnly()` (flags at offset 58).
  FOK and minimum quantity decide from a read-only look at the opposite side and never mutate
  when they fail; post-only that would trade, on add or modify, is rejected with
  `POST_ONLY_WOULD_TRADE`; bad combinations with `INVALID_QUALIFIERS`. Every ladder has
  `forEachLevelWhile`. Differential test with random qualifiers. Requirements FR-33, FR-37–FR-39.
- Participants and trade ids: `ParticipantId`, `TradeId`; `Order::getParticipant()` (offset 60,
  every `Order` field offset static-asserted); `OrderRequest::participant`,
  `ModifyRequest::participant`; participant cancel `cancelOrder(id, participant)` and owner-only
  modify, both rejecting with `NOT_OWNER`; `cancelOrder(id)` is the administrative cancel.
  `Fill::tradeId` (per book, from 1) and `Fill::makerParticipant`; `lastTradeId()`. Requirements
  FR-35, FR-36.
- `BookLimits` (third constructor argument of every book): `maxOrderQuantity`,
  `maxLevelQuantity`, price band `minPrice`/`maxPrice`, `maxNotional`, `reserveOrders`, and a hard
  `maxOrders` that preallocates and rejects instead of growing. Reject reasons
  `ORDER_QUANTITY_LIMIT`, `LEVEL_QUANTITY_LIMIT`, `PRICE_OUT_OF_BAND`, `NOTIONAL_LIMIT`. Checked
  arithmetic helpers in `core/checked_math.hpp`. Requirements FR-09, FR-29, FR-34; plan
  `docs/plans/PHASE-B1.md`.
- Configure fails if a public header is missing from the installed file sets (NFR-11).
- Order locators (`OrderLocator` concept; `BasicOrderBook<Ladder, Locator = OrderIndex>`):
  `DirectOrderTable` (engine-issued handles = generation ∥ slot, `reserveOrderId()` /
  `releaseOrderId()`, O(1) worst-case lookup, stale handles rejected) and `HardenedOrderIndex`
  (keyed hash with a per-instance seed, probe distance ≤ `maxProbe`, fixed capacity); books
  `DenseOrderBookHardened`, `DenseOrderBookDirect`, `OrderBookM2Hardened`, `OrderBookM2Direct`;
  reject reasons `INVALID_ORDER_ID`, `ORDER_ID_REJECTED`; requirements FR-27, FR-28, NFR-18,
  NFR-19; ADR 0004. Harness books `dense-hardened`, `dense-direct`; instrumented build writes
  `index_probes_*.csv`.
- `RejectReason::CAPACITY_EXCEEDED`: a bounded ladder that cannot take a new price level rejects
  the add (before any matching) or the modify (order unchanged) instead of allocating.
  `PriceLadder` concept: `canRestAt(price)`.
- Map-ladder node-allocation policies (compile-time): M0 `MapNodesOnHeap` (unchanged default),
  M1 `MapNodesOnMonotonicBuffer`, M2 `MapNodesOnFixedBlocks`; `MapLadderConfig::maxLevels`; books
  `OrderBookM1`, `OrderBookM2`. `FixedBlockResource` (single-size preallocated free list).
- Hybrid ladder `HybridPriceLadder` / `HybridOrderBook`: ring window + bitmap around the touch,
  ordered overflow maps, incremental recentering bounded per add; `PriceLevel::adoptFrom`.
- Tests: capacity rejects for every bounded book, fixed-block resource, hybrid ladder (incl.
  invariants and extreme prices), every typed book / ladder test over all five ladders,
  differential test of all ladders under a drifting mid; option `EIGENBOOK_LONG_TESTS`
  (1,000,000 steps × 5 seeds). Zero-allocation test for every book type. Requirements FR-08,
  FR-26, FR-43, FR-44; ADR 0003.
- `eigenbook_latency --book map,map-m1,map-m2,dense,hybrid|all|both`, `--hybrid-window`.
- Phase A4 measurements (`docs/BENCHMARKS.md`, `bench/results/a4-clang-21.1.8*`) and
  `docs/RESULTS_SUMMARY.md`. Harness: id translation for issuing books uses a direct-indexed table
  for dense ids (the sorted-lookup version polluted the cache before timed cancels).
- Phase A3 measurements (`docs/BENCHMARKS.md`, `bench/results/a3-clang-21.1.8*`): node
  allocation explains little of the map ladder's latency; dense / hybrid are 2–3× faster through
  p99.9; the hybrid matches dense when its window covers the active band.

- `Eigenbook::platform` (static library, Windows and Linux implementations): serialised TSC
  reads and calibration, CPUID identification (invariant TSC, hybrid core type), thread pinning
  and priority, CPU enumeration and benchmark-CPU choice, OS / power-policy facts, pre-faulting.
- Latency harness `eigenbook_latency` (`bench/`): per-operation TSC timing, log-linear histogram
  (3 significant digits), per-class percentiles, five seeded workloads W1–W5 (eight variants),
  open-loop pacing for W5, CSV output; `eigenbook_latency_instrumented` with index probe
  statistics. Option `EIGENBOOK_BUILD_BENCH`. Results in `docs/BENCHMARKS.md`.
- `OrderIndex::capacityFor`, `OrderIndex::hash`, `capacity()`, `maxClusterLength()`, and probe /
  shift statistics behind `EIGENBOOK_INSTRUMENT_INDEX` (compiled out by default; identical code
  for the default build). `BasicOrderBook::orderIndex()` read-only accessor for diagnostics.
- Zero-allocation test binary (`tests/alloc`), histogram, workload and platform tests,
  index-instrumentation test binary. Requirements NFR-13 … NFR-17.

- CMake package `Eigenbook` with targets `Eigenbook::book` (header-only) and
  `Eigenbook::eigenbook` (umbrella), consumable with `find_package(Eigenbook CONFIG)` or
  `add_subdirectory`.
- `CMakePresets.json`: `dev-debug`, `dev-release`, `relwithdebinfo`, `asan-ubsan`, `tsan`,
  `msvc-debug`, `msvc-release`, `msvc-asan`, `bench`, `bench-lto`.
- Options `EIGENBOOK_BUILD_TESTS`, `EIGENBOOK_BUILD_APPS`, `EIGENBOOK_INSTALL` (default ON only
  for a top-level build), `EIGENBOOK_WARNINGS_AS_ERRORS`, `EIGENBOOK_SANITIZE`.
- `eigenbook_warnings` warning set for our own targets; `.clang-tidy`, `.editorconfig`.
- Tests: `instantiation_test.cpp` (compiles every member of both books), CTest
  `package_consumer` (install + `find_package` smoke test).
- Requirements NFR-11 (package consumable via `find_package`) and NFR-12 (header-only book).
- `docs/ARCHITECTURE.md`, ADR 0001 (header-only book target), ADR 0002 (layout and naming),
  `docs/plans/PHASE-A.md`, `CONTRIBUTING.md`, `SECURITY.md`, this changelog.

### Changed

- **Breaking:** headers moved from `include/tradingengine/` to `include/eigenbook/{core,book,util}`
  with `snake_case.hpp` names; all code is in `namespace eigenbook`.
- **Breaking:** the CMake package and targets are `Eigenbook` / `Eigenbook::book` instead of
  `TradingEngine` / `TradingEngine::TradingEngine`. Executables are `eigenbook_tests` and
  `eigenbook_demo`.
- The map and dense ladders and `RejectReason` have their own headers.
- Minimum CMake version is 3.25.
- GoogleTest is pinned to commit `b514bdc898e2951020cbdca1304b75f5950d1f59` (v1.15.2); gmock is
  not built and GoogleTest is not installed with the package.
- Test discovery runs at test time with a 60 s timeout (fixes intermittent post-build failures
  on Windows).
- README rewritten to match the code; design content moved to `docs/ARCHITECTURE.md`.
- All C++ sources formatted with `.clang-format`; include groups are now standard library,
  third-party, `<eigenbook/...>`, local.

### Removed

- The `TradingEngine` STATIC library, the explicit instantiations in `src/book/orderbook.cpp` and
  the `extern template` declarations.
- The empty `TradingEngine` placeholder class.
- A leftover `DOCTEST_CONFIG_DISABLE` definition (the project uses GoogleTest).
- Tracked `.idea/` settings and the outdated `structure.png`.
- Unmeasured latency figures from the README.

### Fixed

- MSVC warning C4324 in `ObjectPool` is suppressed locally with a documented reason; the build is
  warning-free under `/W4 /permissive-` and under Clang's extended warning set.
- The dense ladder's range check no longer overflows on extreme prices (`INT64_MIN`, `INT64_MAX`
  against a positive `minPrice`); it computes the offset in unsigned arithmetic.
- A modify that changes price or grows quantity is all-or-nothing: every limit and capacity check
  runs before the order is detached, so a rejected modify leaves it resting unchanged.
