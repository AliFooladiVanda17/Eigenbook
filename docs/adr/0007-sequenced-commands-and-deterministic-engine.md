# ADR 0007: Sequenced commands and a deterministic engine core

Status: Accepted (Phase B3.1). The journal (B3.2), state hash (B3.3) and snapshots (B3.4) build
on it.

## Context

Until now, callers drove a book directly: each call took caller-supplied data, and nothing
recorded the order in which calls happened or the time the engine should believe. That is enough
for a library. It is not enough for an engine that must be recoverable and auditable: rebuilding
the state after a crash, proving that a replay did what the original run did, and answering
"what happened, in what order, at what time" all need one authoritative order of inputs.

Engines of the LMAX / Nasdaq INET / Pillar family solve this in one way. Every input goes through
a sequencer, which gives it a gap-free number and a timestamp, the only clock the engine sees.
The business logic is a deterministic function of that sequence. Journaling and replication work
on the sequenced stream, and recovery replays it.

## Decision

1. **`Command`** (`engine/command.hpp`): one fixed-layout record for every kind of input (new
   order, participant cancel, modify, halt, resume), 64 bytes, trivially copyable, every field
   offset static-asserted, and **no padding** (`std::has_unique_object_representations`), so two
   equal commands are equal byte for byte. Unused fields are zero. Factory functions build each
   kind from the existing request types; `orderRequest()` and `modifyRequest()` convert back.
2. **`Sequencer<Clock>`** (`engine/sequencer.hpp`): consecutive sequence numbers (from 1, or
   after the last journaled input on restart), and a timestamp from an injected clock, clamped
   so that it never decreases. The clock is a template parameter (`EngineClock` concept):
   `ManualClock` for tests, simulation and replay; `SteadyClock` for live use. The book and the
   engine never read a clock.
3. **`Engine<Book>`** (`engine/engine.hpp`): one book per instrument, registered on the cold
   path and indexed by `InstrumentId` in a flat table. `apply(in, sink)` is the hot path:
   - it accepts exactly the next input sequence; a gap or a repeat is refused as an
     `ApplyError`, emits nothing and changes nothing (that is a fault of the input stream, not a
     business outcome);
   - every accepted input produces its fills (if any), in execution order, followed by exactly
     one summary event: `ACCEPTED`, `MODIFIED`, `CANCELLED`, `REJECTED` (with the reason),
     `HALTED` or `RESUMED`;
   - every event carries a gap-free output sequence, the input sequence that caused it and that
     input's sequencer time.
4. **`OutputEvent`** (`engine/output_event.hpp`): 96 bytes, fixed layout, no padding, like
   `Command`, so an output stream can be compared with `memcmp`.
5. **The sink is a template parameter** (`OutputSink` concept), not a virtual interface or
   `std::function`: no indirect call and no allocation on the hot path.
6. The engine lives in the header-only `Eigenbook::book` target for now; it needs nothing
   compiled.

Two deferrals of Phase B1.2 are resolved here. The participant cancel request is the `CANCEL`
command. The per-book output sequence is the engine's output sequence, which also numbers cancels
and rejects.

## Alternatives considered

1. **`std::variant` of per-type command structs**: more type-safe, but its storage includes
   padding and a discriminator chosen by the library implementation, so the bytes are not
   portable across standard libraries and cannot be journaled as they are. A tagged record with
   zeroed unused fields is portable and trivially serialisable.
2. **Emit the summary first, then the fills** (as many exchange protocols send an
   acknowledgement before executions): the book reports fills while it matches, before it
   returns the result. Buffering the fills to reorder them would need storage on the hot path.
   The summary closes the request instead, so a consumer knows when a request's events are
   complete.
3. **Skip or tolerate gaps** (apply the input, log the gap): a replay would then not reproduce
   the original. A gap means the input stream is broken, and refusing it keeps state and output
   a function of the complete log.
4. **One `Engine` per instrument**: simpler, but the global order across instruments is the
   point of a sequencer (and B10's shards partition instruments, not sequences).

## Consequences

- The same input log gives a byte-identical output stream on a fresh engine and on every book
  type (`Determinism.*`: 20,000 inputs over three instruments, map M0 / M2, dense and hybrid
  books).
- `Command` is the journal payload of B3.2: any change to its layout is a journal schema change.
- `OutputEvent::reason` is meaningful only for `REJECTED`; elsewhere it is zero, which as a
  `RejectReason` value reads as `INVALID_QUANTITY`. Consumers must look at the type first.
- Books with engine-issued handles (`DirectOrderTable`) need the handle reserved before the
  `NEW_ORDER` command is built. That is a gateway step and is not part of the engine yet.
- `addInstrument` allocates; it is a cold-path call and must not run while the engine is
  trading.
