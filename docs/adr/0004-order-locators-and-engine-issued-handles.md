# ADR 0004: Order locators; engine-issued handles for the matching thread

Status: Accepted (Phase A4a). The default locator stays `OrderIndex` until the owner decides.

## Context

Cancel and modify find a resting order through `OrderIndex`: open addressing with linear probing,
hash = the splitmix64 finalizer. Phase A2's W4-collide workload showed what that means when
clients choose their own ids: the finalizer is a public bijection, so ids that all share one home
slot can be computed offline (`bench::unhash`). With 4096 such ids, a cancel took about 17 µs at
p50 instead of about 0.3 µs. Erase also scans the rest of the cluster, so there is no O(1) worst
case even without an attacker.

Exchanges avoid the problem at the source: the matching engine assigns its own order reference
(Nasdaq ITCH's per-add "order reference number" is one public example), and client order ids are
mapped to it at the gateway.

## Decision

- **`OrderLocator` concept** (`book/locator/order_locator.hpp`) with a three-step protocol for new
  orders: `admit(id)` before any matching (may reject), `insert(id, order)` if the remainder rests
  (cannot fail after a successful admit), `abandon(id)` otherwise; plus `find`, `erase`, `unbind`
  (leave the book but keep the id: modify re-entry). `BasicOrderBook<Ladder, Locator = OrderIndex>`.
- **`DirectOrderTable`**: handles = generation (high 32 bits) ∥ slot (low 32 bits), issued by the
  book (`reserveOrderId()`); `find` = one index + one generation compare; every release bumps the
  generation so stale and duplicated handles fail. For the matching thread.
- **`HardenedOrderIndex`**: for deployments that must take client ids: keyed hash with a
  per-instance random seed, a bound on probe distance (`maxProbe`, default 128; over-limit ids
  are refused with `ORDER_ID_REJECTED` and counted), fixed capacity (full = `CAPACITY_EXCEEDED`).
- New reject reasons `INVALID_ORDER_ID`, `ORDER_ID_REJECTED`.

## Alternatives considered

1. **Keep client ids, switch to a keyed cryptographic hash (SipHash)** in the default index:
   removes offline collision building but costs more per lookup and keeps the probing (no O(1)
   worst case). The hardened index takes the cheaper keyed mix and bounds the probing instead.
2. **Robin Hood hashing / tombstones**: reduce variance but neither gives a worst-case bound
   against chosen keys.
3. **Use the order pool slot as the handle** (no separate table): fewer structures, but couples
   the pool's layout to the public id format; kept separate so the pool can change.

## Consequences

- The direct book changes the add flow: reserve a handle, then add. Gateways (Part B) translate
  client references to handles off the matching thread; the benchmark harness does the same
  translation outside the timed region and times `reserveOrderId()` + `addOrder` together.
- Generations are 32 bits: a handle could match again after 2^32 − 1 reuses of the same slot.
- The default `OrderIndex` is unchanged (identical code for the default build), so existing
  results stay comparable; whether the matching engine defaults to the direct table is the
  owner's decision after the A4b numbers.
- `maxProbe` is a capacity-style parameter: too small refuses legitimate ids at natural load
  (32 did, at load 1/2 with 131,071 orders).
