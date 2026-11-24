# Phase B2: formal model of matching

Goal: a TLA+ specification of the book's matching rules, model-checked with TLC on small bounds,
and a way to check that what the C++ book does is a behaviour of that specification.

## Steps

### B2.1 Specification and model check

`formal/tla/EigenbookMatching.tla` and `EigenbookMatching.cfg`.

- State: per side, per price, a FIFO queue of order ids; per id the resting order (side, price,
  open quantity, priority stamp, time in force, post-only); the priority sequence; ghost variables
  for conservation (`requested`, `filled`, `cancelled` per id) and for the request type; a record
  of the last action and the trades it produced.
- Actions: `AddLimit`, `AddMarket`, `Cancel`, `ModifyReduce`, `ModifyReenter`, with IOC, FOK and
  post-only from B1.3. Matching is a recursive sweep over the opposite side, best level first,
  FIFO inside a level, at the maker's price.
- Invariants: `TypeOK`, `NoCross`, `QuantityConservation`, `PriceTimePriority`, `MakerPrice`,
  `NoTradeThroughLimit`, `MarketNeverRests`, `ModifyPriorityRule`, `NoEmptyLevel`, plus
  `QueueConsistent`, `PriorityStampsIncrease`, `FokAllOrNothing`, `PostOnlyNeverTakes`,
  `FilledMatchesTrades` (added after the mutation check, see below).
- Bounds: 2–3 prices, 3–4 ids, quantity ≤ 3. Ids are not reused inside one behaviour (reuse after
  a fill or cancel, FR-05, does not change matching and would only multiply states), and the
  priority sequence is capped (`MaxSeq`) so that repeated re-entries cannot grow the state space
  without end.
- Record: TLC version, distinct states, diameter, runtime, for each configuration.

Not modelled in B2.1, and why:

- Minimum quantity (FR-38): the FOK case (minimum = quantity) is modelled; a general minimum adds
  one more dimension to every add for the same read-only gate.
- DAY vs GTC: identical until sessions exist (B4/B8); the model uses GTC for resting orders.
- MARKET with DAY / GTC: the book treats it as IOC (B1.3), so the model offers IOC and FOK only.
- Limits, capacity, participants, halts: rejects before any mutation; they do not change matching.
  Self-trade prevention arrives in B7 and will extend the model there.

### B2.2 Trace validation

Debug builds emit an NDJSON trace (action and abstract post-state). A checker confirms that each
trace is a behaviour of the specification: TLA+ trace validation, or a C++ executable refinement
of the spec's next-state relation if TLC is too slow. It runs on differential-test traces in CI.
`formal/README.md` gives the abstraction function from the C++ state to the spec state.

## Tooling

TLC needs Java (installed here: Java 25) and `tla2tools.jar`, which is not in the repository and
not in the build: the formal tools are a check run beside the build, like a linter (ADR 0005).
The version is pinned (`v1.7.4`, the latest stable release when this was written; `v1.8.0` was
still a pre-release) and the SHA-256 of the copy used is recorded in `formal/README.md`.

## Outcome of B2.1

- SANY first rejected the specification: three quantifiers bound a variable over a set that
  depended on another variable of the same quantifier (`\A s \in Sides, p \in Levels(q, s)`),
  which TLA+ does not allow. They are nested quantifiers now.
- The bounds of the first configuration (3 prices, `MaxSeq` 5) did not finish within 10 minutes;
  the default model is 2 prices, and the 3-price model became `EigenbookMatching_large.cfg`.
- The mutation check found a gap in the specification. A fault that reported limit orders as
  filled without any trade was not caught: no invariant said that fills come from trades. The
  specification gained a ghost `volume` and the invariant `FilledMatchesTrades` (fills of all ids =
  2 × traded volume), which catches it; the mutant meant for `NoCross` became
  `limit-never-matches`.
- Default model, final specification: no violation of 14 invariants, 4,886,533 distinct states,
  depth 11, 3 min 20 s on 20 workers (unchanged state counts: the ghost adds no states).
- Mutation check: 14 of 14 seeded faults caught.
- Large model (3 prices, `MaxSeq` 5): no violation, 64,807,213 distinct states, depth 13,
  35 min 03 s; fingerprint-collision estimate 1.1 × 10⁻⁴ (from the actual fingerprints).
- Not run: 4 ids, quantity ≤ 3 (see `formal/README.md`, "Bounds not yet checked").
- Recorded runs and environment: `formal/README.md`.

## Outcome of B2.2

As built (decisions in ADR 0006):

- Tracing is a recorder outside the book (`tests/trace/trace_recorder.hpp`), not a debug-build
  branch: the book's code is unchanged, and the trace is written in model terms (abstraction
  function applied by the recorder).
- TLC reads the trace through a converter (`eigenbook_trace_to_tla`) instead of the
  CommunityModules `Json` module. Downloading that second jar failed repeatedly (Windows could not
  reach the certificate revocation server); the converter has no dependency and maps JSON the same
  way.
- `EigenbookTrace.tla` follows a trace with one deterministic step per event; TLC's verdict is
  read as accepted (`TraceIncomplete` violated), rejected at `l` (deadlock) or rule broken
  (another invariant).
- The first generator drained the book (uniform prices, nearly every order crossed) and never
  formed a queue of two orders, so time priority inside a level was not exercised; the
  `queue-order` negative control exposed it. The generator now builds a book and prints coverage
  counters per trace.
- Long traces overflowed the JVM stack in the recursive sum over ids; TLC runs with `-Xss64m`.
- Recorded run: 3 seeds × 7 book types × 500 events, 21 of 21 traces accepted in 427 s; the 5
  corruptions rejected exactly at the corrupted step. For each seed the seven abstract traces are
  byte-identical, so the run checks 3 distinct behaviours on seven implementations.
- CTest: `formal_trace_validation` (label `formal`, 2 seeds × 200 events, about 53 s), registered
  only when `EIGENBOOK_TLA_TOOLS_JAR` is set.

Not done in B2.2:

- CI itself: Phase B12 sets up CI; the CTest test is ready for it.
- Traces of the existing differential-test streams, as the brief suggests. The generator uses its
  own streams of the same request kinds. Recording the differential streams would need the
  recorder to map their prices (around 1,000) to model prices with an offset; a trace check does
  not enumerate states, so their larger quantities would cost nothing.
- The general minimum quantity, limits, capacity, participants and halts: outside the model.

## Risks

- State explosion: the bounds above are the starting point; record the counts and shrink if
  needed.
- The spec checks itself, not the C++: until B2.2 runs, a passing model check says the rules are
  consistent, not that the book follows them.
