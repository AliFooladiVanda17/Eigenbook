# Phase B1: hot-path correctness and safety hardening

Four commits, each with all tests green on MSVC and Clang (Debug, Release, sanitizers).

## B1.1 Limits, checked arithmetic, all-or-nothing modify

| File | Change |
|---|---|
| `core/book_limits.hpp` (new) | `BookLimits`: `reserveOrders` (preallocated), `maxOrders` (0 = storage may grow past the reserve, which allocates; > 0 = hard limit, storage preallocated for it), `maxOrderQuantity`, `maxLevelQuantity`, instrument price band `[minPrice, maxPrice]`, `maxNotional`. Defaults leave real inputs alone but keep every sum below 2^64 |
| `core/checked_math.hpp` (new) | `notionalWithin(price, qty, limit)`: exact, overflow-free (`qty <= limit / |price|`, with the INT64_MIN magnitude handled) |
| `core/reject_reason.hpp` | `ORDER_QUANTITY_LIMIT`, `LEVEL_QUANTITY_LIMIT`, `PRICE_OUT_OF_BAND`, `NOTIONAL_LIMIT` |
| `book/ladder/dense_ladder.hpp` | `accepts` / `indexOf` in unsigned arithmetic: no signed overflow for any int64 price (the A0 UB finding) |
| `book/order_book.hpp` | Constructor `(InstrumentId, LadderConfig, BookLimits = {})`; the old `(…, std::size_t expectedOrders)` stays and means `reserveOrders`. Every check (quantity, band, notional, level quantity, ladder capacity, order capacity, locator admission) runs in one validation step **before any mutation**. Modify validates the re-entry completely (counting the resources the order itself frees) before it detaches the order, then executes a path that cannot reject: modify becomes all-or-nothing (the A0 order-loss bug). Resting orders carry the book's priority sequence in `Order::timestamp` |
| Tests | `limits_test.cpp`: every limit, before matching, on add and modify; modify rejected by any check leaves the order untouched; hard `maxOrders` never grows pool or index; extreme dense-ladder prices (UBSan) |

Known limit: with map ladder M0, a re-entering modify can still lose the order if the tree's node
allocation throws `std::bad_alloc`; M1 / M2 / dense / hybrid cannot allocate there.

## B1.2 Participant, ownership, trade id, sequence

`Order`: `uint16_t flags` at offset 58, `uint32_t participant` at offset 60 (offsets
static-asserted; still 64 bytes). `ParticipantId` strong type; `OrderRequest` / `ModifyRequest`
gain `participant`; `cancelOrder(const CancelRequest&)` checks it (`NOT_OWNER`), `cancelOrder(OrderId)`
stays as the administrative cancel (no ownership check, documented). `Fill` gains `tradeId`
(per-book, monotonic from 1) and `sequence`; `ExecutionResult` gains `sequence` (per-book output
sequence; rejects are numbered by the sequencer in B3).

## B1.3 Time in force and qualifiers

`TimeInForce` {DAY, GTC, IOC, FOK}, `postOnly`, `minQuantity` in `OrderRequest`; TIF and post-only
stored in `Order` flags. FOK and minimum quantity use a read-only liquidity pre-check
(`forEachLevelWhile` on every ladder) and never mutate when they fail; IOC / FOK remainders are
cancelled; post-only that would trade is rejected (`POST_ONLY_WOULD_TRADE`); invalid combinations
rejected (`INVALID_QUALIFIERS`). DAY / GTC expiry belongs to the session layer (B4); the book stores
the TIF.

## B1.4 Invariant checker

`checkInvariants()` on the book and every ladder: not crossed; every level non-empty; level
aggregate = Σ order quantities; level count = list length; every indexed order linked in exactly
one level at its price; FIFO order = priority sequence; bitmap bit ⇔ level non-empty; cached best =
true best; pool live count = index size. `BookLimits::auditEvery`: 1 = after every operation
(default in builds without `NDEBUG`), N = every N-th operation, 0 = off (default in release). A
violation halts the book: every further request is rejected with `INSTRUMENT_HALTED`. The
"cycle budget" of the brief is expressed as the sampling interval (a full check costs
O(orders + levels)); an incremental checker is left for later and noted.

## Risks

- API growth of the request structs: new fields go last with defaults, so existing designated
  initialisers keep compiling.
- Conservative pre-checks (capacity, level quantity) reject some orders that would have traded in
  full; documented per check.
- Checks add branches to the add path; the A2–A4 benchmarks are re-run for one workload to see the
  cost (measured, not assumed).
