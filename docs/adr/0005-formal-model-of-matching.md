# ADR 0005: A TLA+ model of the matching rules, checked with TLC and a mutation check

Status: Accepted (Phase B2.1). Trace validation against the C++ book is Phase B2.2.

## Context

The book's matching rules are tested by unit tests, typed tests over every ladder, and
differential tests against a naive reference book. All of them compare one implementation with
another (or with hand-written expectations) on the inputs someone thought of or a random stream
happened to produce. None of them states the rules once, independently of any implementation,
and checks every reachable state of a bounded model.

Phase B1 made the rules richer: time in force (IOC, FOK), post-only (also on a modify
re-entry), all-or-nothing modify, a priority sequence owned by the book. Interactions between
those features (a post-only order re-entering, a FOK whose liquidity check and sweep must agree,
a reduce that must keep its place) are where hand-written tests are thinnest.

## Decision

1. **Specify the matching rules in TLA+** (`formal/tla/EigenbookMatching.tla`): one book, FIFO
   queues per side and price, the five request kinds of the book (add limit, add market, cancel,
   modify-reduce, modify-reenter) with IOC, FOK and post-only. Matching is written operationally
   (a recursive sweep, the same shape as `BasicOrderBook::sweep`); the rules are written
   declaratively, as invariants over the state and over a record of the last action's trades.
   Ghost variables (`requested`, `filled`, `cancelled`, `reqType`) make conservation and
   "market never rests" checkable as state predicates.
2. **Check it with TLC** on bounded models: `EigenbookMatching.cfg` (default) and
   `EigenbookMatching_large.cfg`. Every recorded run is listed in `formal/README.md` with the
   tool version, bounds, state counts, depth and runtime.
3. **Prove every invariant falsifiable** (`formal/tla/mutation_check.sh`): one seeded fault per
   invariant, each run with only that invariant, each required to produce a counterexample. A
   clean model-check run counts as evidence only together with a clean mutation run.
4. **Keep the tools outside the build**: `tla2tools.jar` is not vendored and not fetched by
   CMake. It is pinned by version (v1.7.4, the latest stable release) and by the SHA-256 of the
   copy used for the recorded runs. The formal check is a separate job beside the build, like a
   linter.
5. **Abstract deliberately, and write the abstractions down** (`formal/README.md`): ids are not
   reused within a behaviour, DAY = GTC, no general minimum quantity, no limits, capacity,
   participants or halts. Each is either irrelevant to matching (it rejects before any mutation)
   or a special case of something modelled (FOK is the full-quantity minimum).

## Alternatives considered

1. **Property-based tests only** (random requests, invariants checked in C++): already partly
   present (the differential tests now also run `checkInvariants()`). They sample; they do not
   enumerate. Kept, not replaced.
2. **A C++ exhaustive explorer of the real book** (bounded model checking of the
   implementation): checks the code directly but has no independent statement of the rules: a
   wrong rule implemented consistently passes. Reserved for B2.2 as the fallback trace checker
   if TLC trace validation is too slow.
3. **Apalache (symbolic checking)**: handles larger bounds for some specs, but needs type
   annotations and a different toolchain; TLC is the reference tool and is enough at these
   bounds.
4. **PlusCal**: algorithm-style syntax compiled to TLA+. The sweep is a recursive function and
   the rest are single-step actions, so plain TLA+ is shorter and has no translation step.
5. **Vendor the jar in the repository**: makes runs offline-reproducible, but puts a 2.3 MB
   third-party binary in the history and against the "no vendored third-party code" rule. A pin
   (version and hash) gives the same reproducibility.

## Consequences

- The rules have one written statement that is not C++. Disagreement between that statement and
  the book is found only once B2.2 (trace validation) runs; until then, a passing model check
  means the rules are consistent with each other, not that the book follows them. The
  documents say so.
- Every new order type, phase or protection (B4–B7, B13) has to extend the model and add a mutant
  for each new invariant, or say why it is out of the model.
- State space grows quickly with prices and ids. The default model (2 prices, 3 ids,
  quantity ≤ 2) has 4,886,533 distinct states (about 3.5 minutes); three prices has 64,807,213
  (about 35 minutes). The `last` record multiplies states (it distinguishes otherwise equal
  books by how they were reached); it is the price of stating the trade rules as state
  invariants.
- The v1.7.4 jar has no published checksum, so the recorded hash is trust on first use, not a
  verified signature.
