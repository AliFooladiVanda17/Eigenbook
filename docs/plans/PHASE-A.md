# Phase A: standard layout, latency harness, map-ladder tail, cancel-path tail

Status: A0 done (recon); A1 steps 1–4 done, step 5 next.

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

## A2: latency harness (outline, refined before A2 starts)

Files: `include/eigenbook/platform/{tsc,cpu_info,affinity}.hpp`, `src/platform/{tsc,cpu_info,
affinity}_{win,linux}.cpp` → `Eigenbook::platform`; `bench/harness/{timer,histogram,
environment}.hpp`; `bench/workloads/{w1_baseline,w2_level_churn,w3_cancel_heavy,
w4_adversarial_index,w5_microburst}.hpp`; `bench/latency/main.cpp` → `eigenbook_latency`;
`tests/alloc/zero_alloc_test.cpp` (replaced global `operator new/delete`); `docs/BENCHMARKS.md`.

Risks / limits known now:
- Hybrid CPU: CPUID leaf 0x1A reports the type of the core the instruction runs on, so detection
  pins to each logical CPU in turn. Windows also exposes `EfficiencyClass` via
  `GetSystemCpuSetInformation`; both are checked and must agree.
- Power plan is Balanced here; the harness prints it and the docs say so. Changing it is the
  owner's call.
- Windows large pages need `SeLockMemoryPrivilege`; without it `--prefault` touches pages only.
- Linux paths (`pthread_setaffinity_np`, `SCHED_FIFO`, `MAP_POPULATE`, `MADV_HUGEPAGE`) compile
  only where a Linux toolchain exists; they cannot be run or measured here.
- Google Benchmark (bench/micro) cannot be fetched in this environment. bench/micro is wired but
  only built when the dependency is available; the latency harness does not depend on it.

## A3: map-ladder tail (outline)

Allocation policy as a template parameter of `MapPriceLadder` (M0 default pool, M1 monotonic over
a preallocated buffer with `null_memory_resource` upstream, M2 dedicated single-size free list),
`MapLadderConfig::maxLevels`, `RejectReason::CAPACITY_EXCEEDED` at the book boundary,
`book/ladder/hybrid_ladder.hpp` (H1), differential test map/dense/hybrid/reference,
`EIGENBOOK_LONG_TESTS`. Results per variant per workload with measured churn.

Risk: making exhaustion a reject requires the capacity check **before** any mutation; the same
pre-check is what fixes the modify loss bug in B1, so A3 implements the check at the ladder
boundary and B1 reuses it.

## A4: cancel-path tail (outline)

`EIGENBOOK_INSTRUMENT_INDEX` probe/shift histograms; `OrderLocator` concept;
`book/locator/direct_order_table.hpp` (slot index + generation in the 64-bit `OrderId`);
hardened hash index (per-instance random seed, max-probe guard with a counted reject). W3/W4
tables for the three locators.

## Part A exit criteria

All tests green in Debug, Release, and ASan+UBSan (clang on Windows here; GCC/Linux only in CI,
which is Part B12), zero-allocation test green, package-consumer test green,
`docs/BENCHMARKS.md` + `bench/results/*.csv`, `docs/RESULTS_SUMMARY.md`.
