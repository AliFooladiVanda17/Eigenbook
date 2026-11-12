# Contributing to Eigenbook

Priorities, in order: **correctness, then safety and recoverability, then measured performance,
then features.** A change that trades an earlier item for a later one needs a strong reason
written in the pull request (and usually an ADR).

## Workflow

1. Branch from `main`.
2. Make one logical change per commit, using
   [Conventional Commits](https://www.conventionalcommits.org/) (`feat(book): …`,
   `fix(index): …`, `refactor(layout): …`, `build(cmake): …`, `docs: …`, `test: …`).
3. **Pure moves first.** Moving or renaming a file is its own commit, with no content changes
   other than the include paths and CMake lists needed to keep the build green. Content edits to
   the moved file come in a later commit. This keeps git's rename detection, and so the file's
   history, intact.
4. Before a phase of work, write `docs/plans/PHASE-<id>.md` (file-by-file changes, risks, test
   plan). Record architecture decisions in `docs/adr/NNNN-title.md` (context, decision,
   alternatives, consequences).
5. Open a pull request that describes the change and its motivation, and update
   [CHANGELOG.md](CHANGELOG.md) under *Unreleased*.

## Building and testing

```bash
cmake --preset dev-debug && cmake --build --preset dev-debug && ctest --preset dev-debug
```

Before sending a change, all of these must pass where your platform supports them:

| Preset | Why |
|---|---|
| `dev-debug` / `msvc-debug` | Assertions on |
| `dev-release` / `msvc-release` | Optimised build; also runs `package_consumer` |
| `asan-ubsan` (GCC/Clang) or `msvc-asan` | Memory errors and undefined behaviour |
| `tsan` (Linux/macOS) | Once threaded code exists |

Configure with `-DEIGENBOOK_WARNINGS_AS_ERRORS=ON`. Our targets use the `eigenbook_warnings` set
(see [cmake/CompilerWarnings.cmake](cmake/CompilerWarnings.cmake)); fix warnings in the code
rather than weakening the flags. A suppression must be local (`#pragma` push/pop), commented, and
listed under NFR-09 in [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md).

Without network access, point FetchContent at a local checkout of the pinned commit:
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest`.

## Requirements traceability

Every behaviour change updates [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md):

- New behaviour gets a new requirement ID and at least one test; fill in **Verified by** with the
  test names.
- Existing IDs are stable. A requirement that no longer applies is marked retired; its ID is
  never reused.
- Tests that move or are renamed keep their requirement links up to date.

## Code style

- C++23, `namespace eigenbook`, `#pragma once`, `snake_case.hpp` / `.cpp` file names, includes
  by full path (`<eigenbook/book/order_book.hpp>`). See
  [ADR 0002](docs/adr/0002-file-layout-and-naming.md).
- Formatting: [.clang-format](.clang-format). Run `clang-format -i` on the lines you change. The
  tree has not yet been formatted as a whole; that happens in one separate `style:` commit, so
  until then do not reformat code you are not otherwise changing.
- Static analysis: [.clang-tidy](.clang-tidy), e.g. `clang-tidy -p build/dev-debug <files>`.
- Editors: [.editorconfig](.editorconfig).

## Hot-path rules

Code on the matching, cancel and modify paths (ladders, index, pool, and later rings and the
sequencer fast path) must not use: locks, atomics inside a book, virtual dispatch,
`std::function`, thrown exceptions, heap allocation after warm-up, wall-clock reads (time comes
from the sequencer), logging or I/O, or string formatting. Prefer `noexcept`. A capacity problem
is a `RejectReason`, never an allocation.

## Performance claims

A performance claim in code, docs or a pull request must be measured before and after, and
state the machine, OS, compiler and flags, CPU pinning, number of runs and spread. Report
percentiles, not means. Never estimate or round up a number you did not measure; if something
cannot be measured, say so.

## Dependencies

The libraries have no third-party runtime dependencies. Test and benchmark dependencies come
through `FetchContent` in [cmake/Dependencies.cmake](cmake/Dependencies.cmake), pinned to a
commit SHA, never a branch or tag.
