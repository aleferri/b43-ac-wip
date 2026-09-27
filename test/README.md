# Tests

Two suites. They measure different things and neither replaces the other.

## `unit/` — the PHY on its own

The harness stands in for b43:

- `unit/main.c` calls the PHY entry points in the order it chooses, with
  `unit/stubs/b43.h` faking the structures;
- the output is compared against a stock capture.

This is the suite that produces the quotable number. It measures **the PHY
driver**: how much of `src/phy_ac.c`, `radio_2069.c`, `tables_phy_ac.c`,
`rxiqcal_phy_ac.c`, `helpers_phy_ac.c` and `ppr_ac.c` reproduces the stock
driver. See `unit/README.md`.

Its limit is its shape. Operations b43 emits from elsewhere — `MAC.*`, the
core's `OBJ.*`, `TPL.RAMW`, the address match table — cannot come from the
harness. Where the comparison needs them in place, they are mirrored in
`unit/main.c` (`emit_core_*`); the rest are declared in `PERIMETER` of
`unit/compare.py`.

## `integration/` — the whole of b43 on AC

In b43 the order of events cannot be read off the source:

- `main.c` has dozens of gates on `core_rev`, `fw.rev`, `phy->type` and
  `phy->rev`, with free `else` branches;
- the PHY entry points are reached through `b43_phy_init()`, `b43_chip_init()`
  and `b43_wireless_core_init()`.

Reconstructing that sequence by reading produces errors; running it does not.

So this suite does not stand in for b43. It compiles the **real** `b43/main.c`,
`phy_common.c` and the rest, with the `patches/` series and the port inside, and
stubs the layer below. The driver drives itself, gates and `else` branches
included, and the trace is the whole of b43's.

See `integration/README.md`.
