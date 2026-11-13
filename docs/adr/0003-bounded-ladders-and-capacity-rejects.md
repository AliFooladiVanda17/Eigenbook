# ADR 0003: Bounded ladders; capacity is a reject, not an allocation

Status: Accepted (Phase A3a). The choice of default book is deferred to the A3b measurements.

## Context

Phase A2 showed, with a zero-allocation test that can see the map ladder's allocations, that the
default `OrderBook` (map ladder over `pmr::unsynchronized_pool_resource` with the heap as
upstream) allocates after warm-up whenever the number of live price levels reaches a new high
(workload W2-drift). The project rule is "no heap allocation after warm-up; capacity problems are
a `RejectReason`, never an allocation".

Three ways to get there were on the table: preallocate the map's nodes (two variants), or avoid
the tree near the touch altogether (hybrid ladder). Which one has the best tail is an empirical
question (A3b), so all of them must exist side by side and be selectable without runtime cost.

## Decision

- **Capacity is part of the ladder contract.** The `PriceLadder` concept gains
  `canRestAt(price)`: true if a level at that price exists or a new one still fits. The book
  checks it **before any mutation**:
  - limit add: `CAPACITY_EXCEEDED` if the own side has no level at the price and is full, before
    matching (so a marketable limit that would have traded in full is rejected too; the
    liquidity pre-check of Phase B1 can refine that);
  - modify re-entry: `CAPACITY_EXCEEDED`, order untouched, unless removing the order frees its own
    level.
- **Map-ladder node allocation is a compile-time policy**:
  `BasicMapPriceLadder<Side, NodePolicy>` with `MapNodesOnHeap` (M0, today's behaviour),
  `MapNodesOnMonotonicBuffer` (M1) and `MapNodesOnFixedBlocks` (M2). No virtual dispatch: each
  policy is a different type, bound through alias templates (`MapPriceLadder`,
  `MapPriceLadderM1`, `MapPriceLadderM2`) and books (`OrderBook`, `OrderBookM1`, `OrderBookM2`).
- **The map node size is measured, not assumed**: once per instantiation, by recording what a
  `std::pmr::map` requests for one insert. Standard libraries differ, and MSVC debug builds also
  allocate a container proxy through the allocator.
- **Hybrid ladder H1** (`HybridPriceLadder`): a ring window of preallocated levels around the
  touch with the dense ladder's bitmap, ordered overflow maps on a fixed-block resource, and
  incremental recentering bounded per add. Moving a level re-points its orders' level pointers
  (`PriceLevel::adoptFrom`), O(orders at that level).
- **The default `OrderBook` stays M0 for now.** It is the baseline the A3b measurements compare
  against; switching the default is a separate decision with numbers behind it.

## Alternatives considered

1. **Runtime-selected resource** (`std::pmr::memory_resource*` chosen by config): one type, but a
   virtual call on every node allocation and no way to specialise the capacity check. Rejected
   by the hot-path rule (no virtual dispatch).
2. **Catch `std::bad_alloc` at the book boundary** and turn it into a reject: needs exceptions on
   the hot path and leaves the book half-modified if the throw happens mid-operation (the
   modify-loss bug). Rejected; the pre-check makes the throw unreachable instead.
3. **Pre-warm M0** with the default upstream: removes the allocations seen in W2-drift up to the
   warmed size, but has no bound, so a larger peak allocates again. M1 is that idea with a hard
   bound.
4. **Own intrusive tree / skip list** instead of `std::pmr::map`: possibly faster, but much more
   code to verify; revisit only if A3b shows the tree itself (not its allocation) dominates.

## Consequences

- Bounded books (`OrderBookM1`, `OrderBookM2`, `HybridOrderBook`) need `maxLevels` (and the hybrid
  a window) per instrument; sizing is a configuration concern, documented with defaults.
- A full side rejects new price levels instead of slowing down or allocating; the reject is a
  typed, countable event.
- The zero-allocation test now covers five book types; all bounded ones show zero allocations in
  every workload.
- `PriceLevel` gained one public, O(orders) operation (`adoptFrom`), used only by the hybrid
  ladder's recentering.
