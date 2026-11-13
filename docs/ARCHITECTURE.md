# Architecture

This document describes what exists today (the order book) and, separately, the target
architecture around it. Anything not yet implemented is marked **planned**.

## Target architecture

![Target system flow](images/layers.png)

Only the right-hand part exists today: `OrderBook` → bid / ask side → price ladder → price level
→ FIFO order list. Symbol normalisation, the routing table, shard threads and the order-book
registry are **planned** (Phase B10). The planned engine follows the sequenced
state-machine pattern: every input is sequenced once, and each book is a deterministic state
machine over that log (Phase B3).

### Principles

- **Single owner per book.** A book is mutated by exactly one thread and has no internal
  locking. Concurrency is handled only at the boundaries (ingestion, sequencing, publication).
- **Pre-resolved identifiers.** Symbols are resolved to stable `InstrumentId` values off the hot
  path (planned); the book only sees ids.
- **Book-local matching.** Each book does its own matching, cancel and modify.
- **No allocation, locks, exceptions or I/O on the hot path** after warm-up. This is not fully
  true yet: see [Known limitations](../README.md#known-limitations).

## The order book

`BasicOrderBook<Ladder>` (`include/eigenbook/book/order_book.hpp`) owns:

| Component | Structure | File |
|---|---|---|
| Order storage | Fixed-size object pool with an intrusive free list; chunks of up to 4096 slots | `util/object_pool.hpp` |
| Order node | `Order`, exactly 64 bytes, `alignas(64)`; also the intrusive list node | `core/order.hpp` |
| Order locator (default) | `OrderIndex`: `OrderId → Order*`, open addressing, linear probing, power-of-two table, load ≤ 1/2, backward-shift deletion, splitmix64 finalizer as hash; grows | `book/locator/hash_order_index.hpp` |
| Order locator (hardened) | `HardenedOrderIndex`: same table, hash of `id ^ seed` with a per-instance random seed, probe distance ≤ `maxProbe`, fixed capacity | `book/locator/hardened_hash_index.hpp` |
| Order locator (direct) | `DirectOrderTable`: slot array + free list; handle = generation ∥ slot, issued by the book; lookup = one index + one compare | `book/locator/direct_order_table.hpp` |
| Price level | Intrusive doubly linked FIFO list plus aggregate quantity and count | `book/price_level.hpp` |
| Price ladder (sparse) | `std::pmr::map<Price, PriceLevel>`; node memory by compile-time policy: M0 `unsynchronized_pool_resource` over the heap, M1 the same pool over a preallocated arena with no upstream, M2 a preallocated single-size free list | `book/ladder/map_ladder.hpp`, `util/fixed_block_resource.hpp` |
| Price ladder (dense) | One preallocated `PriceLevel` per tick + two-level occupancy bitmap + cached best index | `book/ladder/dense_ladder.hpp`, `util/level_bitmap.hpp` |
| Price ladder (hybrid) | Ring window of preallocated levels + bitmap around the touch; ordered overflow maps (fixed-block nodes) above and below; incremental recentering | `book/ladder/hybrid_ladder.hpp` |

Every ladder satisfies the `PriceLadder` concept (`book/price_ladder.hpp`), so the book is one
template over the ladder:

| Book | Ladder | Capacity |
|---|---|---|
| `OrderBook` | `MapPriceLadder` (M0) | unbounded by default (`maxLevels` = 0) |
| `OrderBookM1` | `MapPriceLadderM1` | `maxLevels` per side, preallocated |
| `OrderBookM2` | `MapPriceLadderM2` | `maxLevels` per side, preallocated |
| `DenseOrderBook` | `ArrayPriceLadder` | the configured tick range |
| `HybridOrderBook` | `HybridPriceLadder` | `maxLevels` per side (window + maps), preallocated |

A bounded ladder that cannot take a new level reports `canRestAt(price) == false`; the book turns
that into `CAPACITY_EXCEEDED` **before** any matching (add) or any change (modify). See
[ADR 0003](adr/0003-bounded-ladders-and-capacity-rejects.md).

### Hybrid ladder

- **Window**: `windowTicks` (power of two) levels covering `[base, base + windowTicks)`, slot
  `price & (windowTicks − 1)`, occupancy in a two-level bitmap. Inside it, every operation costs
  what it costs in the dense ladder.
- **Overflow**: prices above the window in one ordered map, below it in another, both ordered by
  the side's priority, so `begin()` is each map's best. Best level = better-side map's best, else
  the window's best, else the worse-side map's best.
- **Recentering**: after each add, if the touch has left the middle half of the window, the
  window slides towards it one tick at a time, at most `maxShiftsPerOp` ticks and at most
  `maxMovesPerOp` level moves per add. A one-tick shift evicts the level at the leaving edge (if
  any) into a map and imports the level at the entering edge (if a map holds it). Moving a level
  re-points each of its orders' level pointers (`PriceLevel::adoptFrom`, O(orders)).
- **Large jumps**: the touch is served from a map (map speed, same results) until the window has
  caught up.

### Order node layout

| Offset | Field | Size |
|---|---|---|
| 0 | `prev` | 8 |
| 8 | `next` | 8 |
| 16 | `level` | 8 |
| 24 | `id` | 8 |
| 32 | `price` | 8 |
| 40 | `quantity` | 8 |
| 48 | `timestamp` (book priority sequence) | 8 |
| 56 | `side` | 1 |
| 57 | `type` | 1 |
| 58 | `flags`: time in force (bits 0–1), post-only (bit 2) | 2 |
| 60 | `participant` | 4 |

The links used while walking a queue and the fields read while matching share one cache line.
Every offset is `static_assert`ed (`OrderLayout` in `core/order.hpp`).

### Matching model

1. Find the best level on the opposite side.
2. Fill against its orders in FIFO order, at the maker's price; one `Fill` callback per
   execution.
3. Continue with the next level until the incoming order is filled, its limit is reached, or the
   opposite side is empty.
4. Remove each level as soon as it empties.
5. Rest the remainder of a limit order; cancel the remainder of a market order.

Modify: same price and quantity not larger → reduce in place, time priority kept. Anything else
→ remove and re-enter as a new limit order (it may trade, and it goes to the back of its level).

### Complexity

P = number of non-empty levels on one side, K = fills, L = levels emptied by a sweep,
T = ticks between the old best and the next non-empty level (dense ladder).

| Operation | Map ladder | Dense ladder |
|---|---|---|
| Best bid / ask | O(1) | O(1) (cached index) |
| Rest a limit order | O(log P) (`try_emplace` searches even when the level exists) | O(1) |
| Order lookup by id (cancel, modify) | `OrderIndex`: expected O(1), worst case the probe cluster; `HardenedOrderIndex`: ≤ `maxProbe` + 1 slots; `DirectOrderTable`: O(1) | same |
| Cancel, level stays non-empty | Lookup + O(1) unlink | same |
| Cancel that empties a level | + O(log P) erase (O(1) amortised if it is the best level) | + O(1), or, if it was the best level, a bitmap scan of O(1 + T/4096) words (one summary word covers 4096 ticks) |
| Modify: reduce at same price | Lookup + O(1) | same |
| Modify: price change or increase | Cancel + add | same |
| Sweep | O(K + L), each emptied best level erased in O(1) amortised | O(K + L) plus one bitmap scan per emptied level |

M1 and M2 have the map ladder's complexity. The hybrid ladder has the dense ladder's complexity
for prices inside its window and the map ladder's outside it, plus, per add, at most
`maxShiftsPerOp` window shifts and `maxMovesPerOp` level moves (each O(orders at that level)).

### Price ladder choice

| | Dense (array + bitmap) | Map (M0 / M1 / M2) | Hybrid |
|---|---|---|---|
| Best for | Dense, bounded tick ranges | Sparse or unbounded prices | Unbounded prices, activity concentrated near the touch |
| Memory | Proportional to the tick range | M0: live levels; M1 / M2: `maxLevels`, preallocated | `windowTicks` + `maxLevels`, preallocated |
| Out-of-range prices | Rejected (`PRICE_OUT_OF_RANGE`) | Accepted | Accepted |
| Allocation after construction | none | M0: when live levels reach a new high; M1 / M2: none | none |

Which one has the best tail latency for which workload is measured in Phase A3b
([BENCHMARKS.md](BENCHMARKS.md)).

## Build structure

- `Eigenbook::book`: header-only `INTERFACE` target with a `FILE_SET HEADERS`
  ([ADR 0001](adr/0001-header-only-book-target.md)).
- `Eigenbook::eigenbook`: umbrella over all public libraries.
- `Eigenbook::platform`: the only compiled library (namespace `eigenbook::platform`): TSC
  reads and calibration, CPUID, thread pinning and priority, OS facts. Portable parts in
  `src/platform/`, OS parts in `src/platform/windows/` and `src/platform/linux/`. Used by the
  benchmarks now and by the engine later; the book itself does not depend on it.
- `bench/` (not installed): latency harness and seeded workloads, see
  [BENCHMARKS.md](BENCHMARKS.md).
- File layout and naming: [ADR 0002](adr/0002-file-layout-and-naming.md).
