# Eigenbook

**A header-only limit order book in C++23.**

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C.svg)
![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)
![Tests](https://img.shields.io/badge/tests-GoogleTest-green.svg)

Eigenbook is a price-time priority limit order book for one instrument per book, designed to be
owned by a single thread so that matching needs no locks.

> **Status:** active development. The order book (matching, cancel, modify, two price ladders) is
> implemented and tested. Everything around it (sequencer, journal, sessions, the sharded
> engine, market data) is planned; see [Roadmap](#roadmap). There are no published latency
> numbers yet: the measurement harness is being built in Phase A2.

---

## Table of contents

- [Features](#features)
- [Known limitations](#known-limitations)
- [Requirements](#requirements)
- [Building](#building)
- [Using Eigenbook in your project](#using-eigenbook-in-your-project)
- [Usage](#usage)
- [Testing](#testing)
- [Benchmarks](#benchmarks)
- [Design](#design)
- [Project layout](#project-layout)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Features

- **Price-time priority matching** for limit and market orders, with partial fills and
  multi-level sweeps. Every execution is at the resting order's price.
- **Cancel and modify through an order locator:** by default a flat hash index (open addressing,
  linear probing) with client-chosen ids; or a hardened variant (keyed hash, bounded probes); or
  engine-issued handles with an O(1) worst-case lookup (`DenseOrderBookDirect`). Hash index: expected
  O(1) lookup. Reducing quantity at the same price keeps time priority; any other change
  re-enters the order.
- **Price-ladder strategies** behind one C++20 concept (`PriceLadder`):
  - `OrderBook`: `std::pmr::map` ladder over a pool resource (M0), for sparse or unbounded prices.
  - `OrderBookM1`, `OrderBookM2`: the same map ladder with a level bound (`maxLevels`) and its
    nodes preallocated (M1: pool over a preallocated arena; M2: single-size free list); no
    allocation after construction.
  - `DenseOrderBook`: fixed array + two-level occupancy bitmap, for dense, bounded tick ranges.
  - `HybridOrderBook`: a dense window around the touch plus ordered maps for the rest, with
    bounded, incremental recentering; any price, no allocation after construction.
- **Capacity is a reject:** a bounded ladder that is full returns `CAPACITY_EXCEEDED` before any
  matching, for adds and modifies, instead of allocating.
- **No allocation in the sweep:** fills are reported through a callback; resting orders come from
  a pre-reserved object pool.
- **One cache line per order:** each `Order` node is exactly 64 bytes (`static_assert`).
- **Strong types** for `Price`, `Quantity`, `OrderId`, `Timestamp`, `InstrumentId` and `ShardId`.
- **Rejections as values:** business rejections are returned as
  `std::expected<…, RejectReason>`.
- **Header-only:** link `Eigenbook::book`; nothing is compiled on the consumer's behalf.
- **Tested against a reference model:** randomized differential tests run every ladder against a
  deliberately naive reference book, including a drifting-mid stream that keeps the hybrid
  window recentering.

## Known limitations

These are tracked in [docs/plans/PHASE-A.md](docs/plans/PHASE-A.md) and fixed in the phases
named there. Until then, do not expose the book to untrusted input.

- Resting an order can allocate (and throw `std::bad_alloc`) when the order pool or the order
  index has to grow beyond what was reserved at construction, and, for the default `OrderBook`
  (map ladder M0), when its node pool grows. The zero-allocation test shows M0 doing this after
  warm-up whenever the number of live price levels reaches a new high (workload W2-drift); the
  dense, M1, M2 and hybrid books did not allocate in any workload.
- A modify that re-enters the order removes the original first; if re-insertion throws, the
  order is lost.
- No ownership check: any caller can cancel or modify any order id. No self-trade prevention,
  no price protection on market orders, no maximum order size.
- `Quantity` / `Price` arithmetic is unchecked; the dense ladder's range check computes
  `price - minPrice` in signed 64-bit, which overflows for extreme prices.
- The default order index hash is a public, invertible mixer, so colliding client-chosen ids can be
  constructed; erase and lookup have no O(1) worst case. Use `HardenedOrderIndex` (client ids) or
  `DirectOrderTable` (engine-issued handles, O(1) worst case) where that matters (ADR 0004).
- Timestamps are supplied by the caller.

---

## Requirements

| Component | Minimum |
|---|---|
| Compiler | C++23: MSVC 19.38+ (VS 2022 17.8+), GCC 13+, or Clang 17+ |
| CMake | 3.25+ |
| Build tool | Ninja (recommended) or Visual Studio 2022 |
| Git | Only to download GoogleTest when tests are enabled |

The library has no third-party dependencies. The tests use GoogleTest 1.15.2, pinned by commit
SHA in [cmake/Dependencies.cmake](cmake/Dependencies.cmake).

The functional and non-functional requirements, each traced to the tests that verify it, are in
**[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md)**.

Tested so far on Windows 11 with MSVC 19.44 and Clang 21.1.8. GCC and Linux are supported by the
build but not yet tested; CI for them is planned.

---

## Building

The build is driven by [CMakePresets.json](CMakePresets.json):

| Configure preset | What it is |
|---|---|
| `dev-debug`, `dev-release`, `relwithdebinfo` | Ninja, default compiler |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer (Clang or GCC) |
| `tsan` | ThreadSanitizer (Clang or GCC, not on Windows) |
| `msvc-debug`, `msvc-release` | Visual Studio 2022 generator, x64 |
| `msvc-asan` | MSVC Debug with `/fsanitize=address` |
| `bench`, `bench-lto` | Release (optionally with LTO), tests off |

Every preset builds into `build/<preset>/`. Each has a build preset and (except `bench*`) a test
preset of the same name.

**Linux:**

```bash
sudo apt install g++-13 cmake ninja-build git
CXX=g++-13 cmake --preset dev-release
cmake --build --preset dev-release
ctest --preset dev-release
```

**Windows (Visual Studio 2022 or its Build Tools, any shell):**

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

The Ninja presets on Windows need `ninja` and the compiler on `PATH`, for example from a
*Developer PowerShell for VS 2022*. To use Clang, pass `-DCMAKE_CXX_COMPILER=clang++` to the
configure step.

Run the demo:

```bash
./build/dev-release/apps/demo/eigenbook_demo                 # Ninja presets
.\build\msvc-release\apps\demo\Release\eigenbook_demo.exe    # Visual Studio presets
```

### CMake options

| Option | Default | Description |
|---|---|---|
| `EIGENBOOK_BUILD_TESTS` | ON if top-level | Build the test suite (downloads GoogleTest) |
| `EIGENBOOK_BUILD_APPS` | ON if top-level | Build `apps/` |
| `EIGENBOOK_BUILD_BENCH` | ON if top-level | Build `bench/` (`eigenbook_latency`) |
| `EIGENBOOK_INSTALL` | ON if top-level | Generate install and package-export rules |
| `EIGENBOOK_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` for our own targets |
| `EIGENBOOK_SANITIZE` | empty | `address;undefined`, `thread`, or `address` (MSVC) |

"Top-level" means Eigenbook is the project being configured, not a subdirectory of another one.

**Offline builds:** point FetchContent at a local checkout of the pinned GoogleTest commit with
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest`. Machine-specific settings like this
can go in a `CMakeUserPresets.json` (git-ignored).

---

## Using Eigenbook in your project

**From an install tree:**

```bash
cmake --preset dev-release
cmake --install build/dev-release --prefix /opt/eigenbook
```

```cmake
find_package(Eigenbook 0.1 CONFIG REQUIRED)   # with -DCMAKE_PREFIX_PATH=/opt/eigenbook
target_link_libraries(my_app PRIVATE Eigenbook::book)
```

**As a subdirectory or with FetchContent:** `add_subdirectory(eigenbook)` and link
`Eigenbook::book`. Tests, apps and install rules stay off when Eigenbook is not the top-level
project.

| Target | Contents |
|---|---|
| `Eigenbook::book` | The header-only order book (`include/eigenbook/{core,book,util}`) |
| `Eigenbook::platform` | Static library: TSC, CPU / core-type detection, thread pinning, OS facts (`include/eigenbook/platform`; Windows and Linux) |
| `Eigenbook::eigenbook` | Umbrella: every public Eigenbook library |

---

## Usage

```cpp
#include <iostream>
#include <eigenbook/book/order_book.hpp>

int main() {
    using namespace eigenbook;

    OrderBook book(InstrumentId{1});

    // A resting sell order: 100 lots at 10 050 ticks.
    if (!book.addOrder({.id = {1}, .side = OrderSide::SELL, .type = OrderType::LIMIT,
                        .price = {10'050}, .quantity = {100}})) {
        return 1;
    }

    // An aggressive buy: trades 40 lots against order 1, reported through the callback.
    const auto result = book.addOrder(
        {.id = {2}, .side = OrderSide::BUY, .type = OrderType::LIMIT,
         .price = {10'050}, .quantity = {40}},
        [](const Fill& f) {
            std::cout << "fill " << f.quantity.value << " @ " << f.price.value << '\n';
        });
    if (!result) {
        return 1; // result.error() is a RejectReason, e.g. DUPLICATE_ORDER_ID
    }

    // Order 1 has 60 lots open; reducing it at the same price keeps its time priority.
    const auto modified = book.modifyOrder({.id = {1}, .price = {10'050}, .quantity = {30}});

    const auto cancelled = book.cancelOrder({1});
    if (modified && cancelled) {
        std::cout << "cancelled " << cancelled->value << " open lots\n"; // 30
    }
}
```

For a dense, bounded tick range, use `DenseOrderBook` and give it the range:

```cpp
eigenbook::DenseOrderBook book(eigenbook::InstrumentId{1},
                               {.minPrice = {9'000}, .tickCount = 2'048});
```

| Operation | Returns |
|---|---|
| `addOrder(request[, onFill])` | `std::expected<ExecutionResult, RejectReason>`: filled / resting / cancelled quantities |
| `modifyOrder(request[, onFill])` | Same as `addOrder` |
| `cancelOrder(id)` | `std::expected<Quantity, RejectReason>`: the open quantity that was removed |
| `bestBid()`, `bestAsk()` | `std::optional<Price>` |
| `bids()`, `asks()` | The ladder for each side: `findLevel`, `forEachLevel`, `levelCount` |

The constructor's third argument, `expectedOrders` (default 1024), sizes the order pool and the
index. Size it for the maximum number of resting orders; beyond that, resting an order allocates
(see [Known limitations](#known-limitations)).

---

## Testing

```bash
ctest --preset dev-debug          # or msvc-debug, asan-ubsan, msvc-asan, ...
```

| Location | Covers |
|---|---|
| [tests/unit/types_test.cpp](tests/unit/types_test.cpp) | Strong types and the `Order` node |
| [tests/unit/price_level_test.cpp](tests/unit/price_level_test.cpp) | The intrusive FIFO queue at one price |
| [tests/unit/price_ladder_test.cpp](tests/unit/price_ladder_test.cpp) | Both ladder strategies, on both sides |
| [tests/unit/order_book_test.cpp](tests/unit/order_book_test.cpp) | Matching, cancel and modify, on both book types |
| [tests/unit/util_test.cpp](tests/unit/util_test.cpp) | `LevelBitmap`, `OrderIndex`, `ObjectPool` |
| [tests/unit/instantiation_test.cpp](tests/unit/instantiation_test.cpp) | Compiles every member of both books (no test cases) |
| [tests/unit/histogram_test.cpp](tests/unit/histogram_test.cpp) | Benchmark histogram accuracy against an exact reference |
| [tests/unit/workload_test.cpp](tests/unit/workload_test.cpp) | Benchmark workloads: determinism, predicted outcomes on both books |
| [tests/unit/platform_test.cpp](tests/unit/platform_test.cpp) | CPU detection, TSC calibration, pinning, CPU choice |
| [tests/unit/capacity_test.cpp](tests/unit/capacity_test.cpp) | `CAPACITY_EXCEEDED` for adds and modifies on every bounded book |
| [tests/unit/hybrid_ladder_test.cpp](tests/unit/hybrid_ladder_test.cpp) | Hybrid window, overflow maps, recentering, invariants |
| [tests/unit/fixed_block_resource_test.cpp](tests/unit/fixed_block_resource_test.cpp) | The M2 single-size block resource |
| [tests/unit/locator_test.cpp](tests/unit/locator_test.cpp) | Direct order table and hardened hash index |
| [tests/unit/locator_book_test.cpp](tests/unit/locator_book_test.cpp) | Books with engine-issued handles and with the hardened index |
| [tests/differential/differential_test.cpp](tests/differential/differential_test.cpp) | 20 000 random operations: map ladder vs dense ladder vs a reference model |
| [tests/alloc/zero_alloc_test.cpp](tests/alloc/zero_alloc_test.cpp) | Separate binary with counting `operator new` and pmr default resource: zero allocations in the measured part of every workload |
| [tests/unit/index_instrumentation_test.cpp](tests/unit/index_instrumentation_test.cpp) | Separate binary built with `EIGENBOOK_INSTRUMENT_INDEX`: index probe statistics |
| [tests/package_consumer/](tests/package_consumer/) | CTest `package_consumer`: installs the build and builds a separate project against it with `find_package(Eigenbook)` |

`ctest` runs 315 GoogleTest cases in three executables plus the `package_consumer` test. Each
test is mapped to a requirement in [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md).

---

## Benchmarks

Results, method and environment: **[docs/BENCHMARKS.md](docs/BENCHMARKS.md)**.

`eigenbook_latency` times every operation separately (serialised TSC reads, pinned to a P-core),
records it in the histogram of its class (passive add, aggressive add by number of fills,
cancel hit by queue position, cancel miss, modify) and reports p50 … p99.99 and max per class,
over five seeded runs, for eight workloads and both ladders:

```bash
cmake --preset bench && cmake --build --preset bench
./build/bench/bench/latency/eigenbook_latency   # --quick for a short run; options: top of bench/latency/main.cpp
```

`eigenbook_demo` still prints its old whole-loop mean; it is a usage example, not a benchmark.

---

## Design

See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for the data structures, the matching model,
complexity, and the target architecture around the book. Architecture decisions are recorded in
[docs/adr/](docs/adr/).

---

## Project layout

```
Eigenbook/
├── CMakeLists.txt            Options and add_subdirectory only
├── CMakePresets.json         Configure / build / test presets
├── cmake/                    Warnings, sanitizers, dependencies, install/export, package config
├── include/eigenbook/        Public headers (namespace eigenbook)
│   ├── core/                 types.hpp, order.hpp, reject_reason.hpp
│   ├── book/                 order_book.hpp, price_level.hpp, price_ladder.hpp (concept)
│   │   ├── ladder/           map_ladder.hpp, dense_ladder.hpp, hybrid_ladder.hpp
│   │   └── locator/          order_locator.hpp (concept), hash_order_index.hpp,
│   │                         hardened_hash_index.hpp, direct_order_table.hpp
│   ├── util/                 level_bitmap.hpp, object_pool.hpp, fixed_block_resource.hpp
│   └── platform/             tsc.hpp, cpu_info.hpp, affinity.hpp, system_info.hpp
├── src/                      Library targets; platform/ (portable, windows/, linux/)
├── apps/demo/                Small usage example
├── bench/
│   ├── harness/              Histogram, op classes, executor, environment report
│   ├── workloads/            Seeded generators W1–W5
│   ├── latency/              eigenbook_latency
│   └── results/              CSV + environment of published runs
├── tests/
│   ├── support/              Shared test helpers
│   ├── unit/                 One file per component
│   ├── differential/         Ladders vs reference model
│   ├── alloc/                Zero-allocation test binary
│   └── package_consumer/     find_package(Eigenbook) smoke project
└── docs/
    ├── REQUIREMENTS.md       Requirements and test traceability
    ├── ARCHITECTURE.md       Design
    ├── BENCHMARKS.md         Method, environment, results
    ├── adr/                  Architecture decision records
    ├── plans/                Per-phase plans
    └── images/
```

---

## Roadmap

Detailed plans live in [docs/plans/](docs/plans/).

**Phase A: layout and measurement**
- [x] Standard layout, namespace, header-only target, presets, install/export
- [x] Latency harness: TSC timer, log-linear histograms, CPU pinning, seeded workloads,
      zero-allocation test
- [x] Map-ladder tail: preallocated node resources (M1, M2), hybrid ladder, measured (A3)
- [ ] Cancel-path tail: direct order table with generation-checked handles, hardened hash index

**Phase B: around the book** (one phase at a time)
- [ ] Hard capacity limits, all-or-nothing modify, checked arithmetic, ownership, IOC/FOK/post-only
- [ ] Formal model of matching (TLA+) and trace validation
- [ ] Sequencer, journal, snapshots, deterministic replay
- [ ] Trading phases, auctions, price protection, self-trade prevention, risk gate
- [ ] Market-data and order-entry sessions, thread-per-shard engine, primary/backup
- [ ] Deterministic simulation testing, CI, fuzzing

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security issues: [SECURITY.md](SECURITY.md). Changes are
listed in [CHANGELOG.md](CHANGELOG.md).

---

## License

No license has been chosen yet, so all rights are reserved by the author.
