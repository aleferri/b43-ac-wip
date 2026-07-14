# bcma_sprom_extract_r11 — offline differential test harness

Userspace harness that compiles `bcma_sprom_extract_r11()` from
`../0001-ssb-bcma-firmware-SROM-revision-11-support.patch` against a
small kernel shim and diffs every populated field against the matching
NVRAM nominal value. Validates the parser without needing to bring up
the radio: the comparison is between two independent decoders of the
same raw SROM (the new open-source parser vs. the Broadcom vendor
decoder that produced `nvram_dump`).

Used during the v1 → v2 round of the patch to pin down that
`SSB_SPROM8_IL0MAC/ANTAVAIL/TXRXC` are NOT correct for rev 11 on the
DSL-3580L, leading to the introduction of the
`SSB_SPROM11_IL0MAC/ANTAVAIL/TXRXC` constants in v2.

## Build and run

```
$ make                     # builds ./test
$ make check               # runs against ../../../router-data/dsl3580l/wl1_*.txt
$ make check-d6220         # runs against ../../../router-data/d6220/wl1_*.txt
                           # (second hardware-real board, same chip family)
$ make check-bcm4360usb    # runs the synth-mode round-trip on the
                           # bcm4360usb reference NVRAM (see below)
$ make check-agcombo       # runs the synth-mode round-trip on the
                           # agcombo NVRAM (BCM4360 reference 3x3,
                           # dual-band, ../../../router-data/agcombo/)
```

To run against another vector:

```
$ ./test path/to/srom_dump.txt path/to/nvram_dump.txt
```

To run a NVRAM-only synth-mode round-trip on any wl1_nvram-style file:

```
$ ./test --synth path/to/nvram.txt
```

Output is line-per-field PASS/FAIL/INFO with a final summary. Exit
status is 0 if no FAIL occurred. Current state on the committed
vectors with the v3 fix scoped in `extract_r11.c` and `synth_srom.c`
(`SSB_SPROM11_CCODE = 0x0096`):

  - DSL-3580L:     77 PASS / 0 FAIL / 2 INFO (raw mode; the two INFO
                   are SROM-vs-NVRAM source divergences for `il0mac`
                   and `country_code`, both legitimate and explained
                   inline in the output).
  - D6220:         74 PASS / 0 FAIL / 5 INFO (raw mode; same two
                   source-divergence INFOs as DSL, plus three
                   NVRAM-missing-key INFOs for `mcsbw1605g{l,m,h}po`
                   — D6220's NVRAM doesn't declare 160 MHz
                   power-per-rate keys, the parser still extracts
                   them from SROM bytes but there is no oracle to
                   diff against).
  - bcm4360usb:    synth mode, Finding 1 collision fixed
                   (`il0mac` no longer corrupted by `ccode` write).
                   Remaining INFOs are nvram-key-missing on the
                   minimal asuswrt-merlin NVRAM template.
  - agcombo:       75 PASS / 0 FAIL / 4 INFO (synth mode; BCM4360
                   reference 3x3 dual-band; `il0mac` PASS, only
                   country_code source divergence and three 160 MHz
                   NVRAM-key-missing INFOs remain).

## Synth-mode round-trip (NVRAM-only second vector)

When a `wl srdump` is not available for a candidate board but its
nominal NVRAM is published (typically as a `defaultsromvars_*[]`
string in some vendor's `bcmsrom.c`), `--synth` synthesizes a raw
SROM image from the NVRAM using the patch's offsets/encoding, then
runs `extract_r11` against it. The diff against the original NVRAM
verifies:

- structural completeness — every NVRAM key the parser declares is
  recoverable;
- encoding self-consistency — bit packs/unpacks survive reflection,
  exposing off-by-one shifts on non-degenerate triplets the
  DSL-3580L's saturated values would mask;
- per-chain stride and per-band byte assignment within a chain
  block — populated chain mismatches FAIL immediately on stride
  errors.

What it does *not* do is cross-validate offsets against an external
source: synth and parse share offsets by construction. For canonical
checks against `bcmsrom_tbl.h`, see `../cross_check.md`.

The committed synth-mode vectors are two:

- `vectors/bcm4360usb.nvram` — BCM4360 USB defaults from
  asuswrt-merlin / landonf/bhnd_nvram_fmt; same chip family as the
  DSL-3580L target with non-saturated `triso=9` and `aa2g=3`. Run via
  `make check-bcm4360usb`.
- `router-data/agcombo/agcombo_nvram.txt` — BCM4360 reference 3x3
  dual-band, hardware-real NVRAM dump. Exercises chain 2 (the
  BCM43b3 boards are 2x2) and the full 2.4 GHz PA chain (`aa2g=7`).
  Run via `make check-agcombo`.

Both runs write a non-zero `macaddr` and a `ccode` value; with the
v2 patch's rev-8 ccode reuse this collided on word 0x92 (Finding 1).
The v3 fix in `extract_r11.c` and `synth_srom.c` reads/writes ccode
at `SSB_SPROM11_CCODE = 0x0096` per Broadcom `bcmsrom.h` rev-11
canonical. After the fix the collision is gone in both vectors
(harness confirms `il0mac` PASS on agcombo synth). See
`../cross_check.md` for the canonical layout reference.

## Contributing a new test vector

The whole point of this harness is to let the OpenWrt / linux-wireless
community contribute dumps from other rev-11 boards without bring-up.
On a router with the Broadcom `wl` tool:

```
$ wl -i wl1 srdump     > srom_<board>.txt
$ wl -i wl1 nvram_dump > nvram_<board>.txt
$ ./test srom_<board>.txt nvram_<board>.txt
```

Post the output (and the dumps if you can share them) to the patch
thread on linux-wireless. Boards with any of the following are
particularly useful — see `../README.md` for the full priority list:

- non-zero values in the `0x190..0x1B0` region;
- non-saturated rxgains;
- chip variants other than BCM4352;
- contradictions to the `IL0MAC=0x90 / ANTAVAIL=0xA0 / TXRXC=0xA8`
  positions pinned in v2.

## Files

- `extract_r11.c` — the parser body, derived from the patch (see
  comment at top of the file for the explicit list of differences).
- `kernel_shim.h` — userspace shim: typedefs, `cpu_to_be16`,
  `BUILD_BUG_ON`, `ARRAY_SIZE`, and `SPOFF/SPEX/SPEX32` byte-identical
  to `drivers/bcma/sprom.c` upstream.
- `ssb_sprom.h` — minimal mock of `struct ssb_sprom` and
  `struct bcma_bus`.
- `ssb_regs.h` — `SSB_SPROM*` byte offsets, including the rev-11
  additions from v2 of the patch. Header documents which constants
  came from upstream verbatim and which were verified via sparse
  checkout.
- `data_load.{h,c}` — text parsers for the two dump files.
- `synth_srom.{h,c}` — encoder counterpart of `extract_r11`. Used by
  `--synth` mode to construct a raw SROM image from a NVRAM-only
  vector. Mirrors the parser's hand-written, bcma-style field handling
  per the patch series' commitment to that convention.
- `vectors/` — additional test vectors (NVRAM-only). `bcm4360usb.nvram`
  ships with the harness as the second reference vector.
- `test.c` — table of field checks plus the diff/summary driver.
  Supports both `srom+nvram` mode (default) and `--synth nvram` mode.
- `Makefile` — `make` builds, `make check` runs against the committed
  DSL-3580L vector at `../../../router-data/dsl3580l/`,
  `make check-d6220` runs against the committed D6220 vector at
  `../../../router-data/d6220/`, `make check-bcm4360usb` runs the
  bcm4360usb synth-mode round-trip, `make check-agcombo` runs the
  agcombo synth-mode round-trip against
  `../../../router-data/agcombo/agcombo_nvram.txt`.
