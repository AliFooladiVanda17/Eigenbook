# Security policy

Eigenbook is research and development software. It has no releases yet and is **not intended
for production use or for untrusted input** in its current state.

## Reporting a vulnerability

Please report suspected vulnerabilities privately through GitHub's
[private vulnerability reporting](https://github.com/AliFooladiVanda17/Eigenbook/security/advisories/new)
for this repository, not in a public issue. Include the affected commit, a description, and a
reproducer if you have one.

There is no service-level commitment for responses at this stage.

## Supported versions

Only the current `main` branch receives fixes.

## Known weaknesses

The following are known and scheduled (see [README: Known limitations](README.md#known-limitations)
and [docs/plans/](docs/plans/)); reports about them are welcome but not needed:

- The default order index (`OrderIndex`) hashes ids with a public, invertible mixer: colliding ids
  can be constructed to slow down the cancel path (HashDoS) when ids are client-chosen. Mitigations
  exist as alternative locators: `HardenedOrderIndex` (keyed hash with a per-instance seed, bounded
  probe distance) and `DirectOrderTable` (engine-issued handles, no hashing); see ADR 0004.
- Unchecked `Price` / `Quantity` arithmetic on extreme inputs.
- No ownership check on cancel / modify.
- Allocation (and `std::bad_alloc`) is possible on the insert path beyond the reserved capacity.

A full threat model is planned (`docs/design/security.md`, Phase B12).
