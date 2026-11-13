# Phase A: standard layout, latency harness, map-ladder tail, cancel-path tail

Status: Part A done (A0–A4). A2 results provisional (unstable clock); A3 / A4 measured on AC, Balanced plan.

## A0 recon results

Environment used for every result in this phase unless a table says otherwise:

| Item | Value |
|---|---|
| CPU | Intel Core i7-13650HX (hybrid: 6 P-cores + 8 E-cores, 20 logical) |
| OS | Windows 11 Enterprise 10.0.26200 |
| Power plan | Balanced (381b4222-…) |
| MSVC | 19.44.35229 (VS 2022 Build Tools, toolset 14.44.35207) |
| Clang | 21.1.8, target x86_64-pc-windows-msvc (MSVC STL) |
| CMake | 4.0.1; Ninja from the VS Build Tools |
| Not available here | GCC, Linux, WSL distribution, TSan, network access to GitHub |

Build and test of the unmodified tree (commit e575e6b), Visual Studio 17 2022 generator, x64:

| Config | Build | Tests |
|---|---|---|
| Debug | OK, 1 warning (C4324 in `ObjectPool.h`) | 94/94 passed, 20 suites |
| Release | OK, same warning | 94/94 passed; `ctest -C Release` 94/94 |

GitHub is blocked by the sandbox proxy, so FetchContent cannot download GoogleTest. The local
checkout left in `build/_deps/googletest-src` by an earlier build is v1.15.2 at commit
`b514bdc898e2951020cbdca1304b75f5950d1f59`, and it is used through
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path>`. That SHA is the one pinned in A1.

Demo app (`TradingEngine_app`, Release, unpinned, Balanced plan), three consecutive runs, mean ns/op
over the whole 2,000,000-op loop:

| Ladder | Run 1 | Run 2 | Run 3 | README says |
|---|---|---|---|---|
| `std::map` | 112.1 | 119.4 | 114.8 | ~106 |
| array + bitset | 99.7 | 95.7 | 102.4 | ~90 |

The README numbers do not reproduce on this run. This is not a regression claim: the method
(a single mean, unpinned, Balanced plan) cannot tell a regression from noise. A2 replaces it.

Demo workload composition, measured by replaying `app/main.cpp`'s generator against the map
book (scratch program, not in the repo): 1,333,382 adds (155,621 of them fill at least once) and
666,618 cancels, of which **142,120 (21.3 %) miss** with `UNKNOWN_ORDER_ID`, because the live list
is never updated on fills.

### §2 claims: confirmed, with these corrections and additions

All §2 statements match the code. Corrections and additions:

1. The doctest line (`target_compile_definitions(TradingEngine PRIVATE DOCTEST_CONFIG_DISABLE)`)
   only takes effect with single-config generators and `CMAKE_BUILD_TYPE=Release`. With the Visual
   Studio generator `CMAKE_BUILD_TYPE` is empty and the line does nothing.
2. **New finding (UB):** `ArrayPriceLadder::accepts` and `indexOf` compute `price - minPrice` in
   signed `int64`. For extreme prices (e.g. `INT64_MIN` with `minPrice > 0`) that overflows,
   which is undefined behaviour, before the unsigned range check runs. B1 fixes it (checked
   arithmetic at the boundary) and adds a test.
3. **README statements that are false today:** "header-only templates" for `include/` (the two
   books are explicitly instantiated in `src/book/orderbook.cpp`), and "rejections are never
   thrown" (`rest()` can throw `std::bad_alloc` from the pool, the index rehash or a new map node).
4. `ObjectPool`'s chunk size is `clamp(expectedOrders, 64, 4096)`, and the constructor reserves
   `expectedOrders` up front. The demo passes `kOps` as `expectedOrders`, so its pool and index
   never grow during the timed loop. The hot-path growth in §2 is real but only shows when live
   orders exceed `expectedOrders`.
5. `structure.png` is not referenced anywhere; `layers.png` is referenced once in the README.
6. `.vscode/` exists locally but is git-ignored; it points at `build-vs/`. It is left alone.

## A1: layout refactor

Each step ends at a COMMIT CHECKPOINT. Build + all 94 tests must be green at every checkpoint
(MSVC Debug and Release; clang Release as a second compiler).

### Step 1: pure moves (no content edits beyond include paths and CMake file lists)

| From | To |
|---|---|
| `include/tradingengine/types.h` | `include/eigenbook/core/types.hpp` |
| `include/tradingengine/order.h` | `include/eigenbook/core/order.hpp` |
| `include/tradingengine/PriceLevel.h` | `include/eigenbook/book/price_level.hpp` |
| `include/tradingengine/PriceLadder.h` | `include/eigenbook/book/price_ladder.hpp` (split in step 3) |
| `include/tradingengine/OrderIndex.h` | `include/eigenbook/book/locator/hash_order_index.hpp` |
| `include/tradingengine/orderbook.h` | `include/eigenbook/book/order_book.hpp` |
| `include/tradingengine/util/LevelBitmap.h` | `include/eigenbook/util/level_bitmap.hpp` |
| `include/tradingengine/util/ObjectPool.h` | `include/eigenbook/util/object_pool.hpp` |
| `app/main.cpp`, `app/CMakeLists.txt` | `apps/demo/main.cpp`, `apps/demo/CMakeLists.txt` |
| `tests/TestSupport.h` | `tests/support/test_support.hpp` |
| `tests/Types_tests.cpp` | `tests/unit/types_test.cpp` |
| `tests/PriceLevel_tests.cpp` | `tests/unit/price_level_test.cpp` |
| `tests/PriceLadder_tests.cpp` | `tests/unit/price_ladder_test.cpp` |
| `tests/OrderBook_tests.cpp` | `tests/unit/order_book_test.cpp` |
| `tests/Util_tests.cpp` | `tests/unit/util_test.cpp` |
| `tests/Differential_tests.cpp` | `tests/differential/differential_test.cpp` |
| `cmake/TradingEngineConfig.cmake.in` | `cmake/EigenbookConfig.cmake.in` (still generates `TradingEngineConfig.cmake` until step 4) |
| `layers.png`, `structure.png` | `docs/images/` |
| `.idea/` | removed; `.idea/` added to `.gitignore` |

Not moved in step 1: `include/tradingengine/TradingEngine.h`, `src/engine/TradingEngine.cpp`
and `src/book/orderbook.cpp`. All three are deleted in step 4, so moving them first would only
create a rename that is immediately deleted. `orderbook.cpp` gets its one include line updated.

Content edits allowed in step 1: `#include` paths, the CMake source/header lists,
`add_subdirectory(apps/demo)`, the README image link, `.gitignore`.

### Step 2: namespace, include guards, boilerplate

- Wrap all library code in `namespace eigenbook`. No sub-namespaces yet; `eigenbook::platform`
  arrives with the platform code in A2. The `std::hash` specialisations stay at global scope and
  name `eigenbook::OrderId` etc.
- Replace every `HPC_QUEUE_*` / `TRADINGENGINE_*` guard with `#pragma once` (supported by every
  target compiler). Remove the "Created by" headers and the `hpc-queue` comment in CMake.
- Tests: put each test file inside `namespace eigenbook { namespace { … } }` (no
  `using namespace` in headers). The demo app uses qualified names.

### Step 3: split headers to the target layout

- `book/price_ladder.hpp` keeps `SideTraits` and the `PriceLadder` concept only.
- `book/ladder/map_ladder.hpp`: `MapLadderConfig`, `MapPriceLadder`.
- `book/ladder/dense_ladder.hpp`: `ArrayLadderConfig`, `ArrayPriceLadder` (class names unchanged
  in Part A to keep the diff readable; renames, if any, are a later separate change).
- `core/reject_reason.hpp`: `RejectReason` moved out of `order_book.hpp`.
- `core/config.hpp` is **not** created yet: nothing belongs there until A3 (`maxLevels`) / B1
  (`BookLimits`).
- The concept `static_assert`s move next to each ladder.

### Step 4: CMake rebuild

- Minimum CMake **3.25**: `FILE_SET HEADERS` needs 3.23, and `SYSTEM` on `FetchContent_Declare`
  (so GoogleTest headers do not trip our `-Wold-style-cast` etc.) needs 3.25. Ubuntu 24.04 ships
  3.28, VS 2022 17.8 ships 3.27.
- `eigenbook_book` INTERFACE + `Eigenbook::book`; headers via
  `target_sources(eigenbook_book INTERFACE FILE_SET HEADERS BASE_DIRS include FILES …)`;
  `target_compile_features(INTERFACE cxx_std_23)`.
- Delete the `extern template` lines, `src/book/orderbook.cpp`, `TradingEngine.h`,
  `src/engine/TradingEngine.cpp`, `src/CMakeLists.txt`. Add `tests/unit/instantiation_test.cpp`
  with `template class eigenbook::BasicOrderBook<…>;` for both ladders.
- `eigenbook_platform` / `Eigenbook::platform` is **deferred to A2**, when `src/platform/*.cpp`
  first exists (a STATIC library without sources does not configure).
  `Eigenbook::eigenbook` umbrella INTERFACE links `Eigenbook::book` now and platform from A2.
- `cmake/CompilerWarnings.cmake`: `eigenbook_warnings` INTERFACE (GCC/Clang: `-Wall -Wextra
  -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast`; MSVC: `/W4 /permissive-
  /Zc:__cplusplus`), `EIGENBOOK_WARNINGS_AS_ERRORS` adds `-Werror` / `/WX`. Linked PRIVATE by
  tests, apps, bench, so the public headers are compiled under these flags. C4324 is silenced
  locally in `object_pool.hpp` with `#pragma warning(push/disable: 4324/pop)` and a comment (the
  padding is the point: the slot must keep `Order`'s 64-byte alignment).
- `cmake/Sanitizers.cmake`: `EIGENBOOK_SANITIZE=address;undefined|thread`. Clang/GCC flags; MSVC
  `/fsanitize=address` only.
- `cmake/Dependencies.cmake`: GoogleTest by `GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59`
  (v1.15.2), `SYSTEM`, `FIND_PACKAGE_ARGS` not used (reproducibility over convenience).
  Offline builds keep working through `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`.
- Options `EIGENBOOK_BUILD_TESTS/BENCH/APPS` default to `PROJECT_IS_TOP_LEVEL`. Doctest line
  removed.
- Install: `install(TARGETS eigenbook_book eigenbook_eigenbook EXPORT EigenbookTargets FILE_SET
  HEADERS)`, `EigenbookConfig.cmake` + `EigenbookConfigVersion.cmake` (SameMajorVersion),
  namespace `Eigenbook::`, destination `${CMAKE_INSTALL_LIBDIR}/cmake/Eigenbook`. The header-only
  package is arch-independent (`ARCH_INDEPENDENT` on the version file).
- `tests/package_consumer/`: tiny project, `find_package(Eigenbook CONFIG REQUIRED)`, links
  `Eigenbook::book`, runs one order through a book. A CTest test (`package_consumer`) installs the
  build tree into `<build>/tests/package_consumer/install`, then configures, builds and runs the
  consumer against it.
- `CMakePresets.json` (version 6): `dev-debug`, `dev-release`, `relwithdebinfo` (Ninja, default
  compiler), `asan-ubsan` (clang/gcc), `tsan` (clang/gcc, Linux), `msvc-debug`, `msvc-release`
  (Ninja + `cl`), `bench` (Release, `EIGENBOOK_BUILD_BENCH=ON`, optional
  `CMAKE_INTERPROCEDURAL_OPTIMIZATION`). Matching build and test presets.
- `.clang-tidy` (`bugprone-*`, `performance-*`, `modernize-*` minus `use-trailing-return-type`
  and `avoid-c-arrays`, selected `cppcoreguidelines-*`), `.editorconfig`.

**As built (deviations from the bullets above):**

- `src/CMakeLists.txt` is kept and now defines the library targets (`eigenbook_book`, umbrella
  `eigenbook`); the top-level file only sets options and adds subdirectories. Install rules are
  in `cmake/EigenbookInstall.cmake` behind `EIGENBOOK_INSTALL` (default `PROJECT_IS_TOP_LEVEL`).
- The umbrella target is named `eigenbook` (export name `Eigenbook::eigenbook`), not
  `eigenbook_eigenbook`.
- Package config is **not** `ARCH_INDEPENDENT`: `Eigenbook::platform` (STATIC) joins it in A2,
  and changing install layout twice would break consumers.
- `EIGENBOOK_BUILD_BENCH` is added in A2 together with `bench/`; the `bench` / `bench-lto`
  presets exist now (Release, tests off) and gain the option in A2. `EIGENBOOK_BUILD_FUZZ` later.
- `msvc-*` presets use the Visual Studio 2022 generator (works from any shell, no developer
  prompt needed), one configuration per build directory. Added `msvc-asan`
  (`/fsanitize=address`, Debug).
- `include(CTest)` replaced by `enable_testing()` (no Dart/CDash targets).
- Test discovery: `DISCOVERY_MODE PRE_TEST` + `DISCOVERY_TIMEOUT 60`. The intermittent post-build
  failure seen in steps 1–3 was `--gtest_list_tests` exceeding the 5 s default on the first launch
  of a freshly linked binary (later launches ~0.1 s).
- Sanitizer runtime DLL (Windows: clang's or MSVC's `clang_rt.asan_dynamic-x86_64.dll`) is copied
  next to each executable after linking, because GoogleTest's discovery step does not take a
  modified `PATH`.
- Test executable renamed `TradingEngine_tests` → `eigenbook_tests`, demo `TradingEngine_app` →
  `eigenbook_demo`. Test case names are unchanged.

### Step 5: documentation

README rewritten so each statement is true (header-only, layout tree, presets, `find_package`),
`CONTRIBUTING.md`, `SECURITY.md` stub, `CHANGELOG.md` (Keep a Changelog), ADRs:
`docs/adr/0001-header-only-book-target.md`, `0002-file-layout-and-naming.md`. **LICENSE: ask the
owner before adding it.**

As built: the README's design section moved to `docs/ARCHITECTURE.md`, with the complexity table
corrected against the code (map ladder add at an existing price is O(log P); cancel that empties
a non-best map level is O(log P); the order-index lookup is expected O(1), not worst-case O(1)).
The README's unmeasured latency figures were removed. `docs/images/structure.png` was deleted:
it showed an old layout (`src/shard`, `benchmarks/`, `app/`) and was not referenced anywhere.
LICENSE not added: owner has not chosen one yet.

### A1 risks

| Risk | Mitigation |
|---|---|
| Git rename detection lost on moved files | Moves get their own commit; step 1 only touches include lines. |
| Removing explicit instantiation hides a template error in an unused member | `instantiation_test.cpp` instantiates every member of both books. |
| New warning flags flood the headers | Fix in the headers (they are ours), not by weakening flags; anything left is documented in NFR-09. |
| Package consumer test needs a second configure inside CTest | Driven by a CMake script with `execute_process`; uses the same generator and compiler. |
| Offline environment | `FETCHCONTENT_SOURCE_DIR_*` overrides documented in CONTRIBUTING. |

### A1 test plan

All 94 existing tests pass after every step (Debug, Release; MSVC and clang-cl/clang++), plus the
instantiation translation unit compiles, plus the `package_consumer` CTest test passes. New
requirement IDs: NFR-11 (consumable via `find_package(Eigenbook)`), NFR-12 (book target is
header-only, no compiled library needed).

## A2: latency harness

### File-by-file

**Library (small, additive changes)**

| File | Change |
|---|---|
| `include/eigenbook/book/locator/hash_order_index.hpp` | `static capacityFor(expected)` (single source of the sizing rule, used by `reserve` and by W4), public `static hash(key)` (the mixer W4 inverts), cold-path `capacity()` and `maxClusterLength()`; probe/shift statistics compiled in only under `EIGENBOOK_INSTRUMENT_INDEX` |

**Platform module: `Eigenbook::platform` (STATIC, namespace `eigenbook::platform`)**

| File | Content |
|---|---|
| `include/eigenbook/platform/tsc.hpp` | Inline `tscStart()` (`lfence; rdtsc`), `tscStop()` (`rdtscp; lfence`); `calibrateTsc(duration)` against `steady_clock` |
| `include/eigenbook/platform/cpu_info.hpp` | `CpuInfo` (vendor, brand, invariant TSC = CPUID 0x80000007 EDX[8], hybrid = CPUID 7 EDX[15]), `currentCoreType()` (CPUID 0x1A EAX[31:24]: 0x40 Core/P, 0x20 Atom/E) |
| `include/eigenbook/platform/affinity.hpp` | `logicalCpuCount`, `currentCpu`, `pinCurrentThread(cpu)`, `raiseThreadPriority(fifo)`, `enumerateCpus()` (pins to each CPU and reads its core type; on Windows also `EfficiencyClass` from `GetSystemCpuSetInformation`) |
| `include/eigenbook/platform/system_info.hpp` | OS version, power plan / governor, large-page availability, `prefault(ptr, bytes)` |
| `src/platform/cpu_x86.cpp`, `src/platform/tsc.cpp` | Portable x86-64 parts (MSVC `<intrin.h>`, GCC/Clang `<cpuid.h>`/`<x86intrin.h>`) |
| `src/platform/windows/{affinity,system_info}.cpp` | `SetThreadAffinityMask`, `SetThreadPriority(TIME_CRITICAL)` + `HIGH_PRIORITY_CLASS`, `GetCurrentProcessorNumberEx`, `RtlGetVersion`, `PowerGetActiveScheme`, `SeLockMemoryPrivilege` check |
| `src/platform/linux/{affinity,system_info}.cpp` | `pthread_setaffinity_np`, optional `SCHED_FIFO`, `sched_getcpu`, `uname`, `scaling_governor`, `/proc/meminfo` huge pages |

**Bench (`bench/`, not installed)**

| File | Content |
|---|---|
| `bench/CMakeLists.txt` | `eigenbook_bench_support` INTERFACE (harness + workloads headers); executables only if `EIGENBOOK_BUILD_BENCH` |
| `bench/harness/histogram.hpp` | Log-linear histogram: exact below 2048, then 1024 sub-buckets per power of two (relative error ≤ 2⁻¹⁰, i.e. 3 significant digits), fixed array, `record()` is `noexcept` and never allocates; exact count/sum/min/max; percentile = highest value equivalent to the bucket (conservative) |
| `bench/harness/op_class.hpp` | Op classes: add-passive, add-aggressive (1, 2–4, ≥5 fills), cancel-hit (any/front/middle/back), cancel-miss, modify-in-place, modify-reenter |
| `bench/harness/environment.hpp` | Collects and prints CPU, core id/type, OS, power plan, compiler, flags, build type, git SHA |
| `bench/workloads/workload.hpp` | `Op` (flat 40-byte record), `Workload` (ops, warm-up count, price range, peak live orders), a shadow map book used during generation to keep the live set correct on fills and to tag each op's expected class |
| `bench/workloads/{w1_baseline,w2_level_churn,w3_cancel_heavy,w4_adversarial_index,w5_microburst}.hpp` | Seeded generators (parameters below) |
| `bench/latency/main.cpp` | `eigenbook_latency` (and `eigenbook_latency_instrumented` with `EIGENBOOK_INSTRUMENT_INDEX`): pins, calibrates, measures timer overhead, runs every workload × book × seed, prints tables, writes CSV |
| `bench/results/` | `summary.csv`, `distribution_<workload>_<book>.csv`, `environment.txt` (committed only when the owner decides) |
| `bench/micro/` | **Not created**: Google Benchmark cannot be fetched here and its commit SHA cannot be verified offline |

**Tests**

| File | Covers |
|---|---|
| `tests/unit/histogram_test.cpp` | Percentiles within 0.1 % of exact (sorted reference), exact min/max/count/mean, no allocation while recording |
| `tests/unit/workload_test.cpp` | Same seed ⇒ identical op stream; every non-miss cancel hits a live order; replaying a workload on both books reproduces the generator's expected classes; `unmix(hash(x)) == x`; W4 ids share one home slot |
| `tests/unit/platform_test.cpp` | CPU info sane, TSC calibration plausible and stable, pin + `currentCpu` agree, CPU enumeration covers every logical CPU |
| `tests/alloc/zero_alloc_test.cpp` | Separate binary with counting global `operator new/delete`: zero allocations in the measured part of every workload, both books (executable proof of NFR-02) |
| `tests/unit/index_instrumentation_test.cpp` | Separate binary built with `EIGENBOOK_INSTRUMENT_INDEX`: probe and shift histograms count what they should |

### Workload parameters (defaults; all are fields of a config struct)

| Workload | Shape |
|---|---|
| W1 baseline | Exactly the demo's generator (seed 42 for run 0), 2,000,000 ops, first 200,000 untimed. The demo-style whole-loop mean is printed beside the percentiles for continuity |
| W2 level churn | Mid 100,000; passive bids in [mid−H, mid−1], asks in [mid+1, mid+H], H = 1000; 10 % aggressive; each resting order cancelled after a geometric lifetime (mean 64 ops). Variant W2-drift: mid +1 tick every 100 ops, H = 4000, 2 % long-lived orders left behind (long gaps for the dense ladder). Measured churn = (levels created + destroyed) / op |
| W3 cancel-heavy | 20 levels × 50 orders per side; timed ops 70 % (W3-70) or 90 % (W3-90) cancels at front/middle/back (equal thirds), remainder passive adds; each timed op paired with an untimed op that restores the level size; 2 % cancel-miss stream |
| W4 adversarial index | W4-collide: C = 4096 ids built by inverting the mixer so they share one home slot; timed cancels/adds of colliding ids, each paired with an untimed op keeping C constant. W4-load: index at load 1/2 (capacity/2 − 1 live, sequential ids), timed cancel + untimed add pairs |
| W5 microbursts | Hawkes arrivals (Ogata thinning; branching ratio 0.8, decay 20 µs, mean rate 500 k ops/s), 45 % passive add, 10 % aggressive add, 45 % cancel of a live order. Open-loop: the harness waits until each op's scheduled time; service time and response time (from the scheduled time, coordinated-omission correct) are reported separately |

Every workload: ≥ 200,000 warm-up ops, then 1,000,000 measured ops (`--quick`: 100,000 measured, 2 runs);
5 runs with seeds base … base+4; report the median of each percentile and the min–max spread.

### Test plan / acceptance

All existing tests plus the new ones pass in msvc-debug, msvc-release, dev-release (Clang),
asan-ubsan, msvc-asan. The zero-allocation test passes for both books on every workload, or the
failure is reported as a finding (it is the executable form of NFR-02; a map-ladder failure under
level churn is the A3 hypothesis). The harness refuses to print nanoseconds without an invariant
TSC. New requirement IDs NFR-13 … NFR-17.

### Risks / limits known now
- Hybrid CPU: CPUID leaf 0x1A reports the type of the core the instruction runs on, so detection
  pins to each logical CPU in turn. Windows also exposes `EfficiencyClass` via
  `GetSystemCpuSetInformation`; both are checked and must agree.
- Power plan is Balanced here; the harness prints it and the docs say so. Changing it is the
  owner's call.
- Windows large pages need `SeLockMemoryPrivilege`; without it `--prefault` touches pages only.
- Linux paths (`pthread_setaffinity_np`, `SCHED_FIFO`, `MAP_POPULATE`, `MADV_HUGEPAGE`) compile
  only where a Linux toolchain exists; they cannot be run or measured here.
- Google Benchmark (bench/micro) cannot be fetched in this environment (GitHub unreachable even
  outside the sandbox), and pinning needs a verified commit SHA, so bench/micro is not created in
  A2. The latency harness does not depend on it.

### As built (A2)

Deviations from the plan above, and things found while doing it:

- **Zero-allocation test needed a second counting path.** A control test (map ladder grows to
  10,000 new levels inside the counted window) counted **zero** allocations with only
  `operator new` replaced: with MSVC's STL in the DLL runtime (`/MD`), the `std::pmr` resources
  live inside the STL DLL and their `operator new` calls do not reach a replacement defined in
  the test executable. The test now also installs a counting `std::pmr` default resource (the
  map ladder's pool takes its upstream from it), and the control passes.
- **NFR-02 is violated by the map ladder** in W2-drift: new highs in the number of live price
  levels make its pool fetch chunks from upstream during the measured part (1 allocation in the
  test's 200,000 measured ops; 2–3 per 1,000,000 in the benchmark). Recorded as a known violation
  that the test asserts still happens (so the A3 fix must remove the entry); NFR-02 is Partial.
  The dense ladder: zero allocations in every workload.
- **Clock instability.** The machine ran on battery with the Balanced plan; the empty-timer
  overhead took three values (≈ 29, 55, 97 TSC ticks) between runs. Added: power source in the
  environment report, an end-of-session clock check, and a **per-run clock probe** (empty-timer
  p50 just before each measured loop) in the tables and CSV. Published numbers are marked
  provisional until re-run on AC with a performance power plan (owner's decision).
- Raw values are labelled **TSC ticks**, not cycles (the TSC runs at a fixed rate).
- The harness counts **pmr upstream allocations** per run (forwarding counter installed as the
  pmr default resource; one extra virtual call on the rare upstream path) so A3 can relate the
  map tail to allocations.
- `w4-collide` measures 100,000 ops per run (each op walks a 4096-slot cluster), not 1,000,000.
- **Modify is not exercised** by any workload (none of W1–W5 specifies it); the classes exist and
  get data when B1 reworks modify.
- `OrderIndex::find/insert/erase/reserve` compile to identical `clang -O2` assembly before and
  after the instrumentation macro was added (checked once). A first version that restructured
  the loops did not, and was replaced.
- `bench/micro/` not created (no Google Benchmark, see above).
- Linux implementations (`src/platform/linux/*`) written but never compiled.

## A3: map-ladder tail

Two checkpoints: **A3a** code + tests (no clock-sensitive work), **A3b** measurements (needs a
stable core clock: AC power and a performance power plan, see A2 "As built").

### File-by-file (A3a)

| File | Change |
|---|---|
| `core/reject_reason.hpp` | `CAPACITY_EXCEEDED` appended (existing values unchanged) |
| `book/price_ladder.hpp` | Concept gains `canRestAt(price) noexcept -> bool`: an order at this price can rest without exceeding the ladder's capacity (its level exists, or a new level fits) |
| `book/price_level.hpp` | `adoptFrom(PriceLevel&)`: moves a level's queue into another (empty) level and re-points each order's level pointer, O(orders); used by the hybrid ladder's recentering |
| `util/fixed_block_resource.hpp` | **M2**: `std::pmr::memory_resource` with one preallocated, pre-faulted array of equal blocks and an intrusive free list; requests that do not fit a block go to a fallback (only the map's own bookkeeping at construction) and are counted |
| `book/ladder/map_ladder.hpp` | `BasicMapPriceLadder<Side, NodePolicy>`; policies **M0** `MapNodesOnHeap` (today's pool, default upstream), **M1** `MapNodesOnMonotonicBuffer` (same pool over a `monotonic_buffer_resource` on a preallocated buffer, upstream `null_memory_resource()`, pool pre-warmed with `maxLevels` nodes), **M2** `MapNodesOnFixedBlocks`; node size measured at construction by recording the allocation of one probe insert; `MapLadderConfig::maxLevels` (0 = unbounded, M0 only); aliases `MapPriceLadder` (= M0, unchanged behaviour), `MapPriceLadderM1`, `MapPriceLadderM2` |
| `book/ladder/hybrid_ladder.hpp` | **H1** `HybridPriceLadder<Side>`: ring of `windowTicks` levels (power of two) + two-level bitmap covering `[base, base + W)`; ordered overflow maps on an M2 resource for prices above / below the window; after each add, up to `maxShiftsPerOp` one-tick window shifts toward the touch, moving at most `maxMovesPerOp` levels (each move is O(orders in that level)); `maxLevels` admission like M1/M2; `checkInvariants()` for tests |
| `book/ladder/dense_ladder.hpp` | `canRestAt` = `accepts` |
| `book/order_book.hpp` | Limit add: `CAPACITY_EXCEEDED` **before any matching** if the own side has no level at the price and is full (conservative: a marketable limit that would fully fill is rejected too; B1's liquidity pre-check refines it). Modify re-entry: rejected, order unchanged, if the new price needs a new level that does not fit even after removing the order. Aliases `OrderBookM1`, `OrderBookM2`, `HybridOrderBook` |
| `bench/workloads/workload.hpp` | `peakLevelsPerSide` (sizes `maxLevels`) |
| `bench/latency/main.cpp` | `--book` takes `map`, `map-m1`, `map-m2`, `dense`, `hybrid`, `all` |
| Tests | capacity rejects (add and modify) for every ladder; M1/M2 build and fill to exactly `maxLevels`; fixed-block resource; hybrid unit tests incl. recentering and invariants; differential test map M0/M1/M2 vs dense vs hybrid vs reference with a **drifting** mid and a small hybrid window; `EIGENBOOK_LONG_TESTS` (≥ 1,000,000 steps × several seeds); zero-allocation test for every book type |

### A3b

W1, W2-churn, W2-drift, W5 for M0, M1, M2, dense, H1: p50 / p99 / p99.9 / p99.99 / max with
measured churn and per-run clock probe. Hypothesis to confirm or refute: the map tail is node
allocation + pointer chasing + rebalancing; M1/M2 remove the first, only dense/hybrid remove all
three.

### Risks

- Capacity must be checked **before** any mutation. The add check is conservative (see above);
  the modify check computes whether removing the order frees its level.
- `std::pmr::map` node size is implementation-defined: measured, not assumed. MSVC debug builds
  also allocate a container proxy through the allocator; it fits a block or goes to the counted
  fallback at construction.
- Hybrid recentering moves levels: a level's address changes, so every order's level pointer is
  re-pointed. The book never holds a level reference across an own-side `addOrder`, which is the
  only place recentering runs.
- Large price jumps: recentering slides at most `maxShiftsPerOp` ticks per add, so after a jump
  the touch lives in the overflow map (correct, map-speed) until the window catches up.

### As built (A3)

- `alignof(std::max_align_t)` is 8 with the MSVC ABI (16 on Linux x86-64); `FixedBlockResource`
  serves requests up to that alignment and sends stricter ones to the counted fallback. Map nodes
  need 8.
- Hybrid window must fit below `INT64_MAX` (validated at construction; the recentering target is
  clamped), found by the extreme-price test.
- The default `OrderBook` stays M0 (baseline); no default change without the owner's decision.
- Harness: `--book` list and `--hybrid-window`. A3b measured the hybrid at 16,384 ticks (main
  session) and 4,096 ticks (second session): the window must cover the active band.
- A3b ran on AC power with the Balanced plan (the owner did not switch plans); per-run clock
  probes are in the tables. Only Clang was measured for A3.
- Result: removing node allocation (M1 / M2) changes W2 latencies by ≤ 7 % (p50–p99) and ≤ 10 %
  (p99.9); dense / hybrid are 2–3× faster than any map variant through p99.9. See
  `docs/BENCHMARKS.md`, Phase A3.

## A4: cancel-path tail

Two checkpoints again: **A4a** code + tests, **A4b** measurements + Part A deliverables.

### File-by-file (A4a)

| File | Change |
|---|---|
| `core/reject_reason.hpp` | `INVALID_ORDER_ID` (add with an id the book did not issue, or a stale one), `ORDER_ID_REJECTED` (the hardened index refuses an id whose probe distance would exceed its limit) |
| `book/locator/order_locator.hpp` | `OrderLocator` concept: `find`, `admit` (before matching: Ok / Duplicate / NotIssued / Full / ProbeLimit), `insert` (only after `admit == Ok`, cannot fail), `erase`, `unbind` (order leaves the book but keeps its id: modify re-entry), `abandon` (admitted id that did not rest), `size`, `empty` |
| `book/locator/hash_order_index.hpp` | `OrderIndex` (unchanged behaviour: client ids, grows) gains the concept's `admit` / `unbind` / `abandon` |
| `book/locator/hardened_hash_index.hpp` | `HardenedOrderIndex`: same table layout, keyed hash `hash(id ^ seed)` with a per-instance random seed (`std::random_device` unless configured), fixed capacity (load ≤ 1/2, no rehash: full → `CAPACITY_EXCEEDED`), max probe distance `maxProbe`: an insert that would land further than that from its home is refused (`ORDER_ID_REJECTED`, counted). Since every stored key is within `maxProbe` of its home, `find` stops after `maxProbe + 1` slots |
| `book/locator/direct_order_table.hpp` | `DirectOrderTable`: fixed slot array + free list; `OrderId` = generation (high 32 bits) ∥ slot (low 32 bits); `issue()` reserves a slot and returns its handle; lookup = one index + one generation compare; every release bumps the generation, so stale or duplicated handles fail; full → `CAPACITY_EXCEEDED` |
| `book/order_book.hpp` | `BasicOrderBook<Ladder, Locator = OrderIndex>`; uses only the concept; `reserveOrderId()` / `releaseOrderId()` for issuing locators; aliases `DenseOrderBookHardened`, `DenseOrderBookDirect` (and `OrderBookHardened` / `OrderBookDirect` on M2) |
| `bench/harness/books.hpp`, `bench/latency/main.cpp`, `bench/harness/executor.hpp` | Book types `dense-hardened`, `dense-direct`; for issuing books the harness translates workload ids to handles outside the timed region (sorted id table built before warm-up) and times `reserveOrderId()` + `addOrder` together for adds; instrumented build writes full probe / shift distributions to CSV |
| Tests | locator unit tests (direct: stale / duplicate / full / generation wrap behaviour; hardened: seed changes placement, probe guard with a crafted cluster, bounded find, churn vs `unordered_map`); book tests for issuing locators; typed book tests and differential test include the hardened book; differential test for the direct book with id translation; zero-allocation test for both new books |

### A4b

Instrumented run (current index) on W3 and W4: probe and shift distributions. Plain runs of W3-70,
W3-90, W4-collide, W4-load50 for `dense` (current index), `dense-hardened`, `dense-direct`.
`docs/RESULTS_SUMMARY.md`; Part A exit check.

### Risks

- Engine-issued handles change the add flow (reserve, then add); the book keeps the client-id
  flow unchanged for hash locators. Client references are mapped to handles at the gateway
  (Part B), not in the book.
- The keyed hash is a splitmix64 finalizer over `id ^ seed`, not a cryptographic PRF; it stops
  an attacker who does not know the seed from building clusters offline. A SipHash-class keyed
  hash would be stronger and slower; the probe-distance guard bounds the damage either way.
- 32-bit generations wrap after 2³² reuses of one slot; a handle that old would then match again.

### As built (A4)

- `HardenedIndexConfig::maxProbe` default 128: 32 refused legitimate ids at load 1/2 (W4-load50).
- The harness's id translation for issuing books first used a binary search over sorted ids; that
  polluted the cache right before timed cancels (W4-load50 direct cancel p99 +40 % vs the hash
  index in that session). Replaced by a direct-indexed table for dense ids (+5 %); both sessions
  are kept and described in `docs/BENCHMARKS.md`.
- Hardened index seed: random per instance in the benchmark (std::random_device), so its layout
  differs between runs; the probe limit was never reached (0 refused ids in every run).

## Part A exit criteria

All tests green in Debug, Release, and ASan+UBSan (clang on Windows here; GCC/Linux only in CI,
which is Part B12), zero-allocation test green, package-consumer test green,
`docs/BENCHMARKS.md` + `bench/results/*.csv`, `docs/RESULTS_SUMMARY.md`.

Check at the end of Phase A (see the final report): met on Windows (MSVC 19.44, Clang 21.1.8:
Debug, Release, ASan, ASan+UBSan); **not met for GCC / Linux** (no toolchain here; the Linux
platform code has never been compiled); benchmarks provisional in A2 and on the Balanced power
plan in A3 / A4.
