# Phase A results summary

Short answers with the numbers behind them. Full tables, method and caveats:
[BENCHMARKS.md](BENCHMARKS.md).

**Where the numbers come from.** Intel Core i7-13650HX, one P-core (CPU 2), Windows 11, Clang 21.1.8
`-O3`, on AC power with the Windows "Balanced" power plan; thread pinned with raised priority, not
an isolated core. Every operation timed on its own (`lfence; rdtsc` … `rdtscp; lfence`, ≈ 15–20 ns
of timer overhead included); medians of 5 seeded runs; each table row comes from runs at the same
measured clock state. The far tail is not reported as a property of the book: maxima of tens of µs
or more appear for every book in every workload, and p99.99 (outside the W4-collide attack on the
current index) varied between sessions from 0.2 µs to
about 19 µs (it was about 5 µs for every book alike in W2-drift and W4-load50), which on a
non-isolated Windows core points at the operating system.

## 1. Build cleanup

| | Before | After |
|---|---|---|
| Library | `TradingEngine` STATIC library holding explicit instantiations; headers listed by `../include` paths | `Eigenbook::book`: header-only `INTERFACE` target, headers in a `FILE_SET`; `Eigenbook::platform` (static) for OS code |
| Consuming it | no install test | `find_package(Eigenbook CONFIG)`; a CTest test installs the build and compiles a separate project against it |
| Warnings | `/W4` or `-Wall -Wextra -Wpedantic` on one target; MSVC C4324 | `-Wconversion -Wsign-conversion -Wshadow -Wold-style-cast` added; 0 warnings with warnings-as-errors on MSVC 19.44 and Clang 21 |
| Checks | Debug / Release | + AddressSanitizer and UndefinedBehaviorSanitizer (Clang), AddressSanitizer (MSVC); all green |
| Tests | 94 | 316, including a zero-allocation test and differential tests of every ladder against a reference model (1,000,000 steps × 5 seeds in the long mode) |
| Dependencies | GoogleTest by tag | GoogleTest pinned by commit SHA; no runtime dependencies |

The book is now a header-only CMake package that installs, is consumed through `find_package`,
and builds warning-free under strict flags and sanitizers on both compilers.

## 2. Map-ladder tail

The question was whether the `std::map` ladder's tail comes from node allocation, pointer chasing
or rebalancing. Three map variants were built: M0 (today's pool over the heap), M1 (pool over a
preallocated arena) and M2 (preallocated single-size free list); M1 and M2 never allocate after
construction (tested).

W2-drift (≈ 0.9 price levels created or destroyed per operation, trending price), passive add /
cancel, ns:

| Book | add p50 | add p99 | add p99.9 | cancel p50 | cancel p99 | cancel p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| map M0 | 161 | 267 | 383 | 198 | 322 | 397 |
| map M1 | 159 | 260 | 354 | 197 | 324 | 396 |
| map M2 | 153 | 250 | 351 | 191 | 316 | 383 |
| dense array | 62 | 130 | 197 | 65 | 123 | 174 |
| hybrid (window covers the band) | 68 | 133 | 208 | 65 | 123 | 171 |

Removing node allocation changed the map ladder's latency by at most 7 % up to p99 and 10 % at
p99.9, while the dense and hybrid layouts were 2–3× faster, so in these workloads the tree's own
traversal and rebalancing, not its allocations, account for almost all of its extra latency.

## 3. Cancel-path tail

The question was how long the cancel path's tail is with the hash index and what bounds it. Two
alternatives were built: a hardened hash index (per-instance secret seed, bounded probe distance,
fixed capacity) and engine-issued handles (slot + generation; lookup = one array index and one
compare).

Cancel latency, ns (one clock state for the whole session):

| Workload | Current index p50 / p99 / p99.9 | Hardened index | Engine-issued handles |
|---|---|---|---|
| W4-collide: 4,096 client ids crafted to share one hash slot | 7,072 / 7,854 / 11,701 | 60 / 96 / 150 | 51 / 84 / 91 |
| W3-cancel70: small book, cancel at the front of the queue | 55 / 121 / 164 | 56 / 125 / 169 | 43 / 53 / 59 |
| W4-load50: 131,071 resting orders, index at load ½ | 119 / 236 / 386 | 118 / 239 / 383 | 93 / 247 / 403 |

Because the current index's hash is public and invertible, chosen ids can make every cancel walk a
4,096-slot cluster (about 120–140× slower at p50 than either alternative); the hardened index and engine-issued handles are
both immune, and engine-issued handles also flatten the tail when the book fits in cache, while
with 131,071 resting orders the order itself is the cache miss and all three locators are within
about 5 % from p99 up.

## Not measured

Linux, GCC, isolated cores, huge pages, a fixed power plan, and modify-heavy workloads. Google
Benchmark micro-benchmarks were not built (no network access to fetch the dependency).
