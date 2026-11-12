# ADR 0001: The order book is a header-only INTERFACE target

Status: Accepted (Phase A1, step 4)

## Context

Until Phase A1, `orderbook.h` declared `extern template class BasicOrderBook<MapPriceLadder>;`
and `…<ArrayPriceLadder>;`, `src/book/orderbook.cpp` held the matching explicit instantiations,
and everything was linked into a `TradingEngine` STATIC library (together with an empty
`TradingEngine` class). The README nevertheless described the headers as "header-only".

Consequences of that setup:

- Every consumer had to link a compiled library for code that is entirely templates.
- The `extern template` declarations suppress implicit instantiation, so a consumer that did not
  link the library got undefined references for the non-template members (constructor,
  `cancelOrder`, `rest`, `removeResting`).
- A new ladder type could not be used with `BasicOrderBook` without adding an instantiation to
  the library.
- The instantiation did guarantee that every member compiled, which is worth keeping.

## Decision

- `Eigenbook::book` is an `INTERFACE` library. Its headers are listed in a
  `FILE_SET HEADERS` (CMake ≥ 3.23), which also drives the install rules and the include path.
- The `extern template` lines and `src/book/orderbook.cpp` are removed.
- `tests/unit/instantiation_test.cpp` explicitly instantiates both supported books, both ladders
  on both sides and `ObjectPool<Order>`, so every non-template member is compiled on every
  compiler the tests run on (NFR-12).
- Compiled code that is genuinely platform-specific (TSC calibration, affinity, CPU detection)
  goes into a separate STATIC target, `Eigenbook::platform`, added in Phase A2.
  `Eigenbook::eigenbook` is the umbrella that links all public targets.

## Alternatives considered

1. **Keep the explicit instantiations in a STATIC library.** Faster incremental builds for the
   two common books, but it forces a link dependency on users of pure templates, blocks custom
   ladders, and was the source of the README contradiction.
2. **Header-only plus an optional precompiled "instantiations" library.** Possible later if
   build times become a problem; not needed at the current size (the whole test binary builds in
   seconds).
3. **C++20 modules.** Not yet portable across MSVC 19.38, GCC 13 and Clang 17 with CMake's
   install/export support at the level needed here.

## Consequences

- Consumers write `find_package(Eigenbook CONFIG REQUIRED)` and
  `target_link_libraries(app PRIVATE Eigenbook::book)`; nothing is compiled on their behalf.
- Each translation unit that uses a book compiles it. Hot-path code is inlined across the whole
  book, which is what we want for latency.
- The warning set (`eigenbook_warnings`) is applied by our own tests/apps/bench, not exported,
  so consumers are not forced onto our flags.
- The installed package config lives in `<libdir>/cmake/Eigenbook` and is not marked
  arch-independent, because `Eigenbook::platform` joins the package in A2.
