# ADR 0002: File layout, naming and namespace

Status: Accepted (Phase A1, steps 1–3)

## Context

The original tree mixed `PascalCase.h` and `lowercase.h` headers under `include/tradingengine/`,
put everything in the global namespace, used include guards inherited from another project
(`HPC_QUEUE_*`), kept one test file per area in a flat `tests/` directory, and committed IDE
settings (`.idea/`). The project is going to grow by a sequencer, journal, sessions, auctions,
market data and an engine, so the layout has to scale and be predictable.

## Decision

- **Pitchfork-style separation** (in the spirit of the C++ project-layout proposal P1204):
  `include/eigenbook/` public headers, `src/` private implementation, `apps/` executables,
  `tests/{unit,differential,support,package_consumer,…}`, `bench/`, `docs/`, `cmake/`.
  Directories are created only when they get content.
- **Public headers grouped by component:** `core/` (types, order, reject reasons, later config),
  `book/` with `ladder/` and `locator/` sub-directories for the interchangeable strategies,
  `util/`, later `platform/`, `sequencer/`, `journal/`, … Each strategy family has a concept
  header one level up (`book/price_ladder.hpp`).
- **Names:** `snake_case` file names, `.hpp` for headers, `.cpp` for sources, `_test.cpp` for
  test files. Class and function names are unchanged by this decision.
- **Includes** use the full path from `include/`: `#include <eigenbook/book/order_book.hpp>`.
- **Namespace** `eigenbook` for everything; sub-namespaces only where they add meaning
  (`eigenbook::platform` from A2). `std::hash` specialisations stay at global scope.
- **Include guards:** `#pragma once` (supported by MSVC, GCC and Clang, which are the only
  supported compilers).
- **Pure moves first:** a rename or move is its own commit, before any content change to the
  moved file, so git's rename detection keeps the history.
- IDE settings are not tracked (`.idea/`, `.vscode/`, `.vs/` are git-ignored).

## Alternatives considered

- **Keep `include/tradingengine/`:** the project is called Eigenbook; the old name only caused
  confusion with the deleted `TradingEngine` placeholder.
- **Flat `include/eigenbook/`:** simpler today, but ladders, locators and the later modules would
  end up in one directory of 40+ files.
- **`EIGENBOOK_<PATH>_HPP_` guards:** portable to every compiler, but verbose and easy to get
  wrong when files move; every supported compiler handles `#pragma once`.

## Consequences

- Downstream code includes `<eigenbook/...>` and names `eigenbook::...`; the old
  `<tradingengine/...>` paths are gone (no compatibility headers: there were no external users).
- Splitting a file (e.g. the ladders out of `price_ladder.hpp`) cannot keep history for the new
  files; `git log --follow` on the original and `git blame -C` recover it.
