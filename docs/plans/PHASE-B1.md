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

## B1.2 Participant, ownership, trade id

As built (differs from the first draft of this plan; reasons inline):

- `ParticipantId` (uint32) and `TradeId` (uint64) strong types. `Order::participant` at offset 60;
  every field offset is static-asserted. Offsets 58–59 stay free: the flags field arrives with
  B1.3, where it has a meaning (an unused private field also trips Clang's
  `-Wunused-private-field`).
- `OrderRequest` / `ModifyRequest` gain `participant` (last member, default 0, so existing
  positional and designated initialisers still compile).
- Participant cancel is `cancelOrder(OrderId, ParticipantId)` → `NOT_OWNER`. A
  `cancelOrder(const CancelRequest&)` overload next to `cancelOrder(OrderId)` would make every
  existing `cancelOrder({7})` call ambiguous (both are aggregates), so the request struct waits for
  the sequencer's command types (B3). `cancelOrder(OrderId)` stays as the administrative cancel.
- Modify checks ownership first (before quantity checks).
- `Fill` gains `tradeId` (per book, from 1, +1 per execution) and `makerParticipant` (the maker is
  gone once a full fill returns, so its execution report needs the owner from the fill).
- Deferred: a per-book output sequence on `Fill` / `ExecutionResult`. Cancels return only a
  quantity, so a sequence on results alone would have gaps nobody can see; the numbering of every
  input and output belongs to the sequencer (B3) and market data (B8).

## B1.3 Time in force and qualifiers

`TimeInForce` {DAY, GTC, IOC, FOK}, `postOnly`, `minQuantity` in `OrderRequest`; TIF and post-only
stored in `Order` flags. FOK and minimum quantity use a read-only liquidity pre-check
(`forEachLevelWhile` on every ladder) and never mutate when they fail; IOC / FOK remainders are
cancelled; post-only that would trade is rejected (`POST_ONLY_WOULD_TRADE`); invalid combinations
rejected (`INVALID_QUALIFIERS`). DAY / GTC expiry belongs to the session layer (B4); the book stores
the TIF.

As built, decisions worth recording:

- A FOK or minimum-quantity order whose condition fails is *accepted* with `cancelled = quantity`,
  not rejected: it was a valid order that expired on arrival, like an IOC that found nothing. Its
  id is abandoned (an issued handle goes back to the table). Post-only is the opposite case: the
  participant asked for "never take", so taking would break the request, and it is a reject.
- MARKET with DAY / GTC keeps its old meaning (remainder cancelled) instead of becoming invalid, so
  existing callers keep working.
- IOC / FOK limit orders get the price checks (band, range, notional) but not the room checks
  (level quantity, level capacity, `maxOrders`): they never rest.
- The check order is quantity, quantity limit, qualifiers, id admission, price, room, post-only;
  the liquidity look for FOK / minimum quantity runs last, just before matching.
- The pre-check counts whole level aggregates. That is exact today; once self-trade prevention
  (B7) can skip or cancel resting orders, it must subtract them, or a FOK could pass the check
  and then fill only partly.

## B1.4 Invariant checker

`checkInvariants()` on the book and every ladder: not crossed; every level non-empty; level
aggregate = Σ order quantities; level count = list length; every indexed order linked in exactly
one level at its price; FIFO order = priority sequence; bitmap bit ⇔ level non-empty; cached best =
true best; pool live count = index size. `BookLimits::auditEvery`: 1 = after every operation
(default in builds without `NDEBUG`), N = every N-th operation, 0 = off (default in release). A
violation halts the book: every further request is rejected with `INSTRUMENT_HALTED`. The
"cycle budget" of the brief is expressed as the sampling interval (a full check costs
O(orders + levels)); an incremental checker is left for later and noted.

As built, where it differs from the paragraph above:

- `auditEvery` defaults to 0 in every build. Defaulting to 1 without `NDEBUG` would make debug and
  release books behave differently (only one of them halts on a corrupt state) and would make
  every debug run O(n²); the differential tests call the full check every 4,096 steps instead.
- A halt does not reject every request: cancels still work, as on a real venue, so members can
  pull their orders. A failed audit is the only halt that `resume()` refuses.
- `checkInvariants()` returns the first `InvariantViolation` (or nothing) instead of a bool, and is
  `noexcept` and read-only; `audit()` is the call that halts.
- The dense ladder's check walks every tick, O(ticks + orders), not O(orders + levels).
- Corruption for the tests is simulated with public `PriceLevel` operations applied behind the
  book's back (`const_cast`); the checker also runs over long random histories in the
  differential tests, where it must find nothing.

## Risks

- API growth of the request structs: new fields go last with defaults, so existing designated
  initialisers keep compiling.
- Conservative pre-checks (capacity, level quantity) reject some orders that would have traded in
  full; documented per check.
- Checks add branches to the add path; the A2–A4 benchmarks are re-run for one workload to see the
  cost (measured, not assumed).
