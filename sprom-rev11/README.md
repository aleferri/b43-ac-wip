# SROM revision 11 support — draft patch

**Target tree:** `torvalds/linux` master at commit `6d35786`
(verified: `git apply --check` passes clean).
**Status:** *DRAFT — NOT FOR SUBMISSION.*

The patch is stored here as a single file:

```
0001-ssb-bcma-firmware-SROM-revision-11-support.patch
```

It is a single-file consolidation of three logically independent
changes that nevertheless need to land together to be useful (struct
extension, NVRAM-key path, raw SROM extractor).

It will be `git format-patch`'d and sent to
`linux-wireless@vger.kernel.org` only **after** at least one of the
open verification points below is closed. Sending unvalidated SROM
extraction code to mainline would be irresponsible — none of this
can be exercised end-to-end until a board with rev 11 SROM actually
completes `op_init` under b43, and several offset proposals cannot
be uniquely pinned from the single reference dump.

## Why this patch exists

`drivers/bcma/sprom.c` accepts SROM revision 11 at validation
(`bcma_sprom_valid` allows `revision <= 11` since 2015) but the only
extractor in mainline is `bcma_sprom_extract_r8`, which is called for
rev 11 inputs unchanged. The result is that rev-11-specific fields
(`rxgains*`, `mcsbw80*`, `pdoffset*`, `subband5gver`, the femctrl
block, the per-chain `pa5ga[12]` polynomial coefficients, etc.) are
never read from a rev 11 SROM and remain zero in `struct ssb_sprom`.
Any in-tree driver that needs those fields — including a future b43
ac-PHY backend — sees an effectively empty board calibration.

The complementary path, `drivers/firmware/broadcom/bcm47xx_sprom.c`
(NVRAM-key fallback for ARM/MIPS Broadcom router SoCs), has partial
rev 11 coverage: `mcsbw20*po`, `noiselvl*`, `rxgainerr*`, `cdd/stbc/
cck` are mapped, but `rxgains*`, `sb20in*`, `mcsbw80*/160*`,
`pdoffset*`, `subband5gver`, and the femctrl block have no NVRAM
mapping — and there is no `bcm47xx_fill_sprom_path_r11` to handle
the per-chain rev 11 arrays.

## What the patch covers

Per the commit message, three changes consolidated:

1. **`struct ssb_sprom` extension** — appended fields for rev-11
   coverage (per-band rxgains triplets, FEM/PA control block,
   mcsbw80*po / mcsbw160*po / mcslr*po, sb20in*/sb40and80 hr/lr
   matrix, dot11agdup{hr,lr}po, dot11agofdmhrbw202gpo /
   ofdmlrbw202gpo, per-chain pdoffset*ma). `struct
   ssb_sprom_core_pwr_info` gains rev-11-shaped maxp2ga, maxp5ga[4],
   pa2ga[3], pa5ga[12]. No offset shift on existing fields, no ABI
   break.

2. **`drivers/firmware/broadcom/bcm47xx_sprom.c` NVRAM mapping** —
   `bcm47xx_sprom_fill_auto` gets the rev-11-only `ENTRY()` lines,
   plus a new `bcm47xx_fill_sprom_path_r11` for the per-chain
   comma-separated arrays (`rxgains*`, `pa5ga[12]`, `maxp5ga[4]`).
   Mechanical extension of an existing pattern — same macro, same
   style.

3. **`drivers/bcma/sprom.c` raw extractor** — `bcma_sprom_extract_r11`
   selected from `bcma_sprom_get` when the SPROM revision word reads
   as 11. The extractor decodes:

   - shared header (boardrev, MAC, antennas, txchain/rxchain,
     antswitch) via the existing SSB_SPROM8_* offsets;
   - subband5gver at byte 0xD6;
   - per-chain power info blocks at 0xD8 / 0x100 / 0x128
     (stride 0x28), full layout including the rxgains pack;
   - the rxgains bit-packing
     `byte = (trelnabyp << 7) | (triso << 3) | elnagain` with
     byte→sub-band assignment `RXGAINS0 lo=5gm, hi=5gh; RXGAINS1
     lo=2g, hi=5gl(UNII-1)`. Verified by cross-reference of `wl
     srdump` against `wl nvram_dump`;
   - per-chain pdoffset40ma triplet at byte 0xCA;
   - the rev-11 power-per-rate region (byte 0x150..0x190): a layout
     that differs from rev 9 — each 5 GHz sub-band has four
     consecutive u32 entries (mcsbw20/40/80/160) rather than two,
     and a pair of u16 (dot11agofdmhrbw202gpo, ofdmlrbw202gpo) sits
     between the 2.4 GHz BW40 entry and the 5 GHz BW20 entries.

## Test vector reference

Three vectors are wired into the harness (`harness/`):

```
router-data/dsl3580l/wl1_{nvram,srom_raw}.txt   — primary, D-Link DSL-3580L
                                                  (BCM4352-family, P353)
router-data/d6220/wl1_{nvram,srom_raw}.txt      — second hardware-real board,
                                                  Netgear D6220 (same chip
                                                  family, P355)
harness/vectors/bcm4360usb.nvram                — synth-mode, NVRAM-only
                                                  defaultsromvars from
                                                  asuswrt-merlin
```

Run with `make check`, `make check-d6220`, `make check-bcm4360usb`,
`make check-agcombo`. Current state with the v3 fix scoped in
`extract_r11.c` and `synth_srom.c`: 77/0/2, 74/0/5, synth-clean,
75/0/4. Finding 1 (IL0MAC/CCODE word collision) closed per
`cross_check.md`.

The DSL-3580L is the primary board because every offset pinned in
the patch has been derived by exact byte-match of a NVRAM nominal
value against the raw SROM byte stream of that board. Three categories
of fit:

- **Unique value-match** (high confidence): for each named field
  there exists exactly one byte offset in the raw SROM whose bytes
  match the NVRAM-declared value, and the field's neighbours fit the
  expected struct layout. All offsets pinned in the patch are in
  this category, with one fix scoped for v3 (ccode at
  `SSB_SPROM11_CCODE = 0x0096` instead of the rev-8 reuse at 0x92).

- **Vendor-canonical encoding** (high confidence): the rxgains
  bit-packing `(trelnabyp<<7)|(triso<<3)|elnagain` matches the
  Broadcom `bcmsrom_tbl.h` masks
  (`SROM11_RXGAINS5G{H,L}{TRELNABYPA,TRISOA,ELNAGAINA}_MASK` and the
  2g/5gm pairs at the low byte). Triplet `(3,6,1)` for 5gl on every
  sampled board (DSL/D6220/agcombo) decodes uniformly, runtime
  cross-checked via `phyreg 0x{6,8,a}f9 = 0x1602` on two 7.x builds.

- **Region byte 0x190..0x1B0 reads all zero** (cannot disambiguate
  from a single dump): mcslr*po, sb20in40/sb20in80and160 hr/lr,
  sb40and80 hr/lr, dot11agdup{hr,lr}po, pdoffset80ma. Listed as
  TODO in the extractor; reachable only via the bcm47xx_sprom NVRAM
  fallback until a second rev-11 board pins the offsets.

## Open before sending upstream

1. **Patch v3 regeneration.** `harness/{extract_r11.c,synth_srom.c,
   ssb_regs.h}` carry the v3 fix for ccode (Finding 1, see
   `cross_check.md`). The kernel-mainline patch file
   `0001-*.patch` still encodes v2; regeneration before send is a
   single-block edit:

       +#define SSB_SPROM11_CCODE   0x0096
       +#define SSB_SPROM11_REGREV  0x0098
       -SPEX(country_code, SSB_SPROM8_CCODE, ~0, 0);
       +SPEX(country_code, SSB_SPROM11_CCODE, ~0, 0);

   Held until the bring-up MVP completes: there is no point shipping
   v3 to linux-wireless before the in-tree consumer
   (`b43_phy_ac_rxgain_init`) has been exercised end-to-end on real
   hardware.

2. Cross-board confirmation of the all-zero region's offset
   assignment by collecting a dump pair on a second board with
   non-zero values in that region (`mcslr*po`, `sb20in*`,
   `sb40and80*`, `dot11agdup*po`, `pdoffset80ma`, plus the femctrl
   context block).

3. Bring-up reaches probe + RX path on UNII-1 ch.36 in the b43-ac
   scaffolding. This is the in-tree consumer that exercises the
   extractor end-to-end. The offline harness has already validated
   the decode side against `wl nvram_dump` as oracle (77 PASS / 0
   FAIL on the DSL-3580L); bring-up additionally validates that
   the consumer can act on those values, which is a different and
   independent check.

4. Hauke Mehrtens (bcm47xx maintainer) and Rafał Miłecki (bcma) are
   the natural reviewers. Rafał has historical context on SROM rev
   11 from the 2015 Ian Kent thread (the prior attempt that did not
   land). Reference that thread in the cover letter to avoid
   re-treading.

The harness lives in `harness/`; see its README for build/run and
for how to contribute a test vector from another board.
