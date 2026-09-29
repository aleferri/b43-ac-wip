# Tests

Two suites; they measure different things and neither replaces the other.

- **`unit/`** stands in for b43: `unit/main.c` calls the PHY entry points in an
  order it chooses, with stub structures, and compares the output with a stock
  capture. It measures the PHY driver in `src/` and produces the quotable
  number. What b43 emits from elsewhere (`MAC.*`, the core's `OBJ.*`,
  `TPL.RAMW`, the address match table) is mirrored in `unit/main.c` or declared
  in `unit/compare.py`. See `unit/README.md`.
- **`integration/`** does not stand in for b43. The order of events in b43
  cannot be read off the source (dozens of gates on `core_rev`, `fw.rev` and
  `phy->type` with free `else` branches), so this suite compiles the real
  `b43/` with the `patches/` series and `src/` inside, stubs the layer below,
  and lets it drive itself. See `integration/README.md`.

`board_profile.h` holds the board profiles both suites take.
