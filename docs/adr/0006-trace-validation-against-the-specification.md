# ADR 0006: Trace validation of the C++ book against the TLA+ specification

Status: Accepted (Phase B2.2).

## Context

ADR 0005 put the matching rules into a TLA+ specification and model-checked it. That shows the
rules are consistent with each other within the bounds; it says nothing about the C++ book. The
book has its own tests, but they compare it with other C++ (the reference book of the
differential tests) or with expectations written by hand.

Trace validation closes the gap: record what the implementation does, and let the model checker
confirm that the recording is a behaviour of the specification. The model checker then acts as
an oracle that shares no code with the book.

Three things had to be decided: where the trace comes from, how TLC reads it, and how to know
that a passing check is not vacuous.

## Decision

1. **A recorder outside the book** (`tests/trace/trace_recorder.hpp`). After each request it
   reads the book's public state (levels, queue order, resting orders) and writes one NDJSON line
   in the specification's vocabulary. The book gets no tracing branch, no build flag and no I/O,
   so the hot path is unchanged in every build. This departs from "debug builds emit a trace" in
   the brief: tracing applies to any build, from the outside.
2. **The abstraction function is applied by the recorder** and written down once
   (`formal/README.md`): book ids to model ids (a real translation for engine-issued handles),
   DAY to GTC, queues per side and model price, and per resting order its open quantity,
   priority stamp, time in force and post-only flag.
3. **A seeded generator** (`eigenbook_trace_gen`) drives every book type with streams inside the
   model's vocabulary. Three in four limit orders are passive, so queues form and time priority
   inside a level is exercised; a first version with uniform prices drained the book and never
   formed a queue of two orders. Each trace prints coverage counters (trades, multi-level
   sweeps, longest queue, post-only rejects, FOK orders that did not fill, cancels, reduces,
   re-entries). Any reject other than `POST_ONLY_WOULD_TRADE` stops the generator, because it
   means the stream left the model.
4. **A converter, not the Json module.** `eigenbook_trace_to_tla` turns one NDJSON trace into a
   TLA+ module (`Trace == << records >>`) plus the TLC configuration for that trace's bounds,
   mapping JSON the way the CommunityModules `Json` module does (object → record, array →
   tuple).
5. **One step per event** (`formal/tla/EigenbookTrace.tla`). Step `l` applies the
   specification's own action for event `l`, with the specification (not the trace) deciding
   between reduce and re-entry, and requires the resulting queues, resting orders, trades and
   reject flag to equal the recorded ones. The trace is accepted when TLC violates
   `TraceIncomplete` (every event matched). It is rejected when TLC deadlocks, and the deadlocked
   state names the first event that does not match. The specification's 14 invariants are also
   checked on every state the book reached.
6. **Negative controls in every run.** The check script corrupts one trace in five ways (a trade
   price, a dropped trade, a priority stamp, two queued orders swapped, a flipped reject flag)
   and requires each corrupted trace to be rejected **at the corrupted step**.
7. **CTest integration that the build never downloads for.** The test `formal_trace_validation`
   (label `formal`) is registered only when `EIGENBOOK_TLA_TOOLS_JAR` points at the pinned jar
   and Java 11+ and bash are found. On Windows, Git for Windows' bash is preferred over
   `System32\bash.exe`, which launches WSL.

## Alternatives considered

1. **CommunityModules `Json` module (`ndJsonDeserialize`)**: the usual way for TLC to read
   NDJSON. It needs a second pinned binary (`CommunityModules-deps.jar`). Downloading it failed
   repeatedly (Windows could not reach the certificate revocation server for GitHub's release
   host), and turning revocation checks off was not acceptable. The converter and its JSON
   reader are about 450 lines of test code with no dependency; they keep the toolchain to one
   jar and produce the same TLA+ values. If the Json module is adopted later, the NDJSON format does not change.
2. **Tracing inside the book behind a compile-time flag**: sees internal states the public API
   does not show, but adds a code path to the matching loop and a second build of the book.
   Everything the specification talks about is observable through the public API, so the
   decorator is enough.
3. **A C++ executable refinement of the next-state relation** (the brief's fallback if TLC is
   too slow): fast, but a re-implementation of the specification in C++ is one more
   implementation of the rules, not the rules themselves. TLC is fast enough at these sizes
   (below).
4. **Letting TLC search for any matching behaviour** (actions quantified over all parameters,
   constrained by the trace): more general (it would also cope with partial traces), but it
   multiplies the work per step. The trace records every parameter, so a deterministic step is
   enough.

## Consequences

- The C++ book is now checked against the written rules, not only against other C++. The 21
  traces of the recorded run (3 seeds × 7 book types, 500 events each) are accepted, and all five
  corruptions are rejected where they were made (`formal/README.md`).
- For one seed, all seven book types produce byte-identical abstract traces. That is a
  refinement result in itself (under the abstraction function the implementations cannot be told
  apart), and it means the recorded run validates 3 distinct behaviours, each on seven
  implementations.
- Runtime grows faster than linearly with trace length: about 3 s per 200-event trace, about
  20 s per 500-event trace (each state evaluates sums and quantifiers over every id the trace
  uses). Long traces need a larger JVM stack for the recursive sum (`-Xss64m`).
- Outside the model's vocabulary (limits, capacity, participants, halts, general minimum
  quantity, DAY expiry) nothing is validated; those stay with the C++ tests until the
  specification grows.
- The recorder reads only prices `1..P`; a book that rested an order outside that range would
  produce a trace that misses it. The generator never sends such prices, and the book's own
  `checkInvariants()` runs after every step of the generator.
