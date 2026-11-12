# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html). There has been
no release yet; the CMake project version is 0.1.0.

## [Unreleased]

### Added

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
