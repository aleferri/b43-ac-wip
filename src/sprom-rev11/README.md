# SROM revision 11 support — draft patch

**Status: DRAFT — NOT FOR SUBMISSION.**

Two patch files carry this work:

- **`patches/0001-ssb-bcma-add-SPROM-revision-11-extraction.patch`** is the one
  in the series, and what the port builds against. It adds
  `bcma_sprom_extract_r11()` and the fields the AC-PHY reads:
  - `maxp5ga[4]`, `pa5ga[12]`, the `rxgains_*` triplets, `mcsbw*po` at 20/40/80,
    and the five `rpcal` words (182, 183, 190, 191, 198);
  - the sub-band row offsets, `pdoffset40ma`/`pdoffset80ma`, `tssifloor*`, the
    FEM block (`femctrl`, …) and `subband5gver`;
  - antenna gain decoded into `antenna_gain_qdb[]`;
  - `SSB_SPROM11_CCODE` at `0x0096`.
- **`src/sprom-rev11/0001-ssb-bcma-firmware-SROM-revision-11-support.patch`** is a
  wider draft for upstream. It also covers the NVRAM path in
  `drivers/firmware/broadcom/bcm47xx_sprom.c`. It is not a build prerequisite.

## Why

`drivers/bcma/sprom.c` accepts SROM revision 11 at validation, but the only
extractor in mainline is `bcma_sprom_extract_r8`, which runs unchanged on rev 11
input. The rev-11-specific fields therefore stay zero in `struct ssb_sprom`, and
any consumer — the b43 AC-PHY among them — sees an empty board calibration.

The complementary NVRAM path (`bcm47xx_sprom.c`) maps part of rev 11 and has no
path for the per-chain rev 11 arrays.

## What the draft covers

1. **`struct ssb_sprom` extension.** Fields appended for rev 11: per-band rxgains
   triplets, the FEM/PA control block, `mcsbw80*`/`mcsbw160*`/`mcslr*po` (the
   160 MHz words from the NVRAM only), the five `rpcal` words, the
   `sb20in*`/`sb40and80` hr/lr offsets, `dot11agdup{hr,lr}po`, per-chain
   `pdoffset*ma`. `struct ssb_sprom_core_pwr_info` gains rev-11-shaped
   `maxp2ga`, `maxp5ga[4]`, `pa2ga[3]` and `pa5ga[12]`. Existing fields do not
   move.
2. **`bcm47xx_sprom.c` NVRAM mapping.** Rev-11-only `ENTRY()` lines, plus
   `bcm47xx_fill_sprom_path_r11` for the per-chain comma-separated arrays.
3. **`bcma_sprom_extract_r11`**, selected from `bcma_sprom_get` when the revision
   word reads 11. It decodes:
   - the shared header;
   - `subband5gver` at byte `0xD6`;
   - the per-chain power-info blocks at `0xD8`/`0x100`/`0x128` (stride `0x28`),
     including the rxgains pack
     `byte = (trelnabyp << 7) | (triso << 3) | elnagain`, with
     `RXGAINS0 lo=5gm, hi=5gh; RXGAINS1 lo=2g, hi=5gl`;
   - `pdoffset40ma` at `0xCA`;
   - the rev 11 power-per-rate region `0x150`–`0x190`. Its layout differs from
     rev 9: `mcsbw20/40/80` per 5 GHz sub-band followed by `rpcal` words, and
     a pair of `u16` between the 2.4 GHz BW40 entry and the 5 GHz BW20 entries.
     Rev 11 has no 160 MHz per-rate field in the SROM (`bcmsrom_fmt.h`: words
     182/183, 190/191, 198 are `SROM11_RPCAL_*`).

## How the offsets were pinned

- **Unique value match.** For each field there is exactly one byte offset in the
  raw SROM of the DSL-3580L whose bytes match the NVRAM-declared value, and its
  neighbours fit the expected layout.
- **Canonical layout.** `bcmsrom_tbl.h`/`bcmsrom_fmt.h` (GPL, in bcmdhd and
  asuswrt-merlin) confirm the FEM block, the rxgains masks, `CCODE`/`REGREV`,
  and the sub-band offset words 200–218. See `cross_check.md`.
- **All-zero regions** on every board (the sub-band row offsets,
  `pdoffset80ma` on two of three boards) are placed by the canonical layout,
  not by the dumps.

## Test vectors

The harness in `harness/` checks the extractor against the DSL-3580L, D6220
and agcombo dumps and a BCM4360 USB NVRAM template; results in
`harness/README.md`.

## Open before sending upstream

1. **Hardware.** Bring-up must reach probe and the RX path on real hardware.
   This is the in-tree consumer that exercises the extractor end to end; the
   offline harness validates only the decode side.
2. **A second board with non-zero values** in the regions that are zero on every
   dump in the repository (`mcslr*po`, `sb20in*`, `sb40and80*`,
   `dot11agdup*po`).
3. **Reviewers.** Hauke Mehrtens (bcm47xx) and Rafał Miłecki (bcma) are the
   natural ones. Refer to the 2015 rev 11 thread in the cover letter.
