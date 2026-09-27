# bcma_sprom_extract_r11 — offline differential test harness

A userspace harness that compiles the rev 11 extractor against a small kernel
shim, and diffs every populated field against the matching NVRAM value. It
validates the parser without bringing up the radio. The comparison is between
two independent decoders of the same raw SROM: the open-source parser, and the
Broadcom decoder that produced `nvram_dump`.

## Build and run

```
$ make                     # builds ./test
$ make check               # ../../router-data/dsl3580l/wl1_*.txt (raw mode)
$ make check-d6220         # ../../router-data/d6220/wl1_*.txt (raw mode)
$ make check-bcm4360usb    # synth-mode round trip on vectors/bcm4360usb.nvram
$ make check-agcombo       # synth-mode round trip on ../../router-data/agcombo/wl1_nvram.txt
```

Against another vector:

```
$ ./test path/to/srom_dump.txt path/to/nvram_dump.txt
$ ./test --synth path/to/nvram.txt
```

The output is one PASS/FAIL/INFO line per field, then a summary. The exit status
is 0 if nothing failed.

| vector | result | the INFO lines |
|---|---|---|
| DSL-3580L (raw) | 77 PASS / 0 FAIL / 2 INFO | SROM-vs-NVRAM source differences for `il0mac` and `country_code` |
| D6220 (raw) | 74 PASS / 0 FAIL / 5 INFO | the same two, plus three `mcsbw1605g{l,m,h}po` keys the NVRAM does not declare |
| agcombo (synth) | 75 PASS / 0 FAIL / 4 INFO | `country_code`, plus the three 160 MHz keys |
| bcm4360usb (synth) | 29 PASS / 0 FAIL / 50 INFO | keys missing from the minimal NVRAM template |

## Synth mode

When no `wl srdump` is available but the board's NVRAM is (for example a
`defaultsromvars_*[]` string in a vendor's `bcmsrom.c`), `--synth` synthesises a
raw SROM image from the NVRAM using the extractor's offsets and encoding. It
then runs the extractor on it and diffs.

It checks:

- **structural completeness**: every NVRAM key the parser declares is
  recoverable;
- **encoding self-consistency**: bit packs survive the round trip on
  non-saturated values;
- **per-chain stride and per-band byte assignment**: a stride error fails at
  once.

It does **not** cross-check offsets against an external source, because synth
and parse share offsets by construction. For that, see `../cross_check.md`.

The two synth vectors:

- `vectors/bcm4360usb.nvram`: BCM4360 USB defaults from asuswrt-merlin /
  landonf `bhnd_nvram_fmt`, with non-saturated `triso=9` and `aa2g=3`;
- `router-data/agcombo/wl1_nvram.txt`: a real BCM4360 3×3 dual-band dump, which
  exercises chain 2 and the full 2.4 GHz PA chain.

## Contributing a vector

On a router with Broadcom's `wl` tool:

```
$ wl -i wl1 srdump     > srom_<board>.txt
$ wl -i wl1 nvram_dump > nvram_<board>.txt
$ ./test srom_<board>.txt nvram_<board>.txt
```

The most useful boards have:

- non-zero values in the `0x190`–`0x1B0` region;
- non-saturated rxgains;
- a chip other than the BCM4352.

## Files

- `extract_r11.c`: the parser body (the head of the file lists its differences
  from the patch).
- `synth_srom.{h,c}`: the encoder counterpart, used by `--synth`.
- `kernel_shim.h`: typedefs, `cpu_to_be16`, `BUILD_BUG_ON`, `ARRAY_SIZE`, and
  `SPOFF/SPEX/SPEX32` byte-identical to `drivers/bcma/sprom.c`.
- `ssb_sprom.h`: a minimal mock of `struct ssb_sprom` and `struct bcma_bus`.
- `ssb_regs.h`: `SSB_SPROM*` byte offsets, rev 11 additions included.
- `data_load.{h,c}`: text parsers for the two dump files.
- `test.c`: the table of field checks and the diff/summary driver.
- `vectors/`: NVRAM-only test vectors.
