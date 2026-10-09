# Cross-check against the canonical SROM rev 11 layout

The extractor's offsets were pinned by value-matching `wl srdump` against
`wl nvram_dump` on the reference boards. The authoritative source for the same
offsets is Broadcom's `bcmsrom_tbl.h` / `bcmsrom_fmt.h`, released under GPLv2
with the linking exception in bcmdhd, asuswrt-merlin and several vendor trees.
Every `SSB_SPROM11_*` constant should match the corresponding `SROM11_*` entry.
This file is the ledger of those checks.

## Settled

### FEM/PA control block

`femctrl` cannot be placed by value-matching. The block carries the same values
on both reference boards, which leaves 17 candidate words, all better explained
as other fields.

`bcmsrom_tbl.h` places `femctrl` in `SROM11_FEM_CFG1` with mask `0xf800`
(bits 15:11). With the NVRAM values the expected word is
`1 | (10<<4) | (6<<11)` = `0x30A1`. In the raw dumps it sits at word 85, byte
**`0x0AA`**, with `FEM_CFG2` = `0x00A1` in the next word. All twelve fields
decode to the NVRAM values on two boards, adjacent to `SSB_SPROM11_TXRXC`
(`0x0A8`).

### MAC and country code

From `bcmsrom.h` (word indices):

    SROM11_MACHI    = 72   (byte 0x090)
    SROM11_MACMID   = 73   (byte 0x092)
    SROM11_MACLO    = 74   (byte 0x094)
    SROM11_CCODE    = 75   (byte 0x096)
    SROM11_REGREV   = 76   (byte 0x098)

`SSB_SPROM11_IL0MAC = 0x0090` is the start of the three-word MAC. The country
code sits at `SSB_SPROM11_CCODE = 0x0096`, not at the rev 8 offset `0x92`, which
is the MAC's middle word. The tree and the harness use `0x0096`.

### Rxgains bit packing

A packed byte decodes as `(trelnabyp<<7) | (triso<<3) | elnagain`. The
bcm4360usb synth run encodes the non-saturated triplets `(3, 9, 1)` (5 GHz) and
`(4, 9, 1)` (2.4 GHz) and round-trips them on both chains. This is
self-consistency, not external truth; the canonical masks
(`0x07 / 0x78 / 0x80`) are listed in `bcmsrom_tbl.h` as
`SROM11_RXGAINS*{ELNAGAINA,TRISOA,TRELNABYPA}_MASK`.

### ANTAVAIL, TXRXC, SUBBAND5GVER

Checked on the bcm4360usb synth run with non-degenerate inputs:

    aa2g=3, aa5g=3      → ANTAVAIL=0xA0  word 0x303 → PASS both halves
    txchain/rxchain=3   → TXRXC=0xA8     packed     → PASS
    subband5gver=0x4    → 0xD6                      → PASS

### Per-chain power-info stride

`0xD8`, `0x100` and `0x128`, stride `0x28`:

- the bcm4360usb vector populates two chains with different `pa2ga`/`pa5ga`
  arrays, and both round-trip;
- the D6220 populates all three chains with distinct `pa5ga[12]` and an
  asymmetric `maxp5ga0`, and chain 2 at `0x128` parses cleanly against the NVRAM
  oracle.

## Status table

| constant | value | checked by | canonical |
|---|---|---|---|
| `SSB_SPROM11_IL0MAC` | 0x0090 | DSL-3580L, D6220 (region zero), synth vectors | `SROM11_MACHI` |
| `SSB_SPROM11_CCODE` / `REGREV` | 0x0096 / 0x0098 | synth vectors | `SROM11_CCODE` / `SROM11_REGREV` |
| `SSB_SPROM11_ANTAVAIL` | 0x00A0 | DSL-3580L, D6220, bcm4360usb | not yet read |
| `SSB_SPROM11_TXRXC` | 0x00A8 | DSL-3580L, D6220, bcm4360usb | not yet read |
| `SSB_SPROM11_FEM_CFG1` | 0x00AA | DSL-3580L, D6220 (`0x30A1`) | yes, `SROM11_FEM_CFG1` |
| `SSB_SPROM11_FEM_CFG2` | 0x00AC | DSL-3580L, D6220 (`0x00A1`) | yes, `SROM11_FEM_CFG2` |
| `SSB_SPROM11_PDOFFSET40MA` | 0x00CA | DSL-3580L, D6220 | not yet read |
| `SSB_SPROM11_PDOFFSET80MA` | 0x00D0 | D6220, DSL (zero); agcombo NVRAM `0x0100` | not yet read |
| `SSB_SPROM11_SUBBAND5GVER` | 0x00D6 | DSL-3580L, D6220, bcm4360usb | not yet read |
| `SSB_SPROM11_PWR_INFO_*` | stride 0x28 from 0xD8 | DSL-3580L, D6220 (3 chains), bcm4360usb (2) | not yet read |
| `SSB_SPROM11_PWR_RXGAINS{0,1}` | 0x08 / 0x0A in the chain block | bcm4360usb | masks not yet read |
| `SSB_SPROM11_PWR_MAXP5GA` | 0x0C | DSL-3580L, D6220, bcm4360usb | — |
| `SSB_SPROM11_PWR_PA5GA` | 0x10 | DSL-3580L, D6220, bcm4360usb | — |
| `SSB_SPROM11_CCKBW202GPO` | 0x150 | DSL-3580L, D6220 | — |
| `SSB_SPROM11_MCSBW{20,40,80}5G{L,M,H}PO` | 0x150–0x18A | DSL-3580L, D6220 | — |
| `SSB_SPROM11_RPCAL_{2G,5GL,5GM,5GH,5GU}` | 0x16C, 0x16E, 0x17C, 0x17E, 0x18C | D6220, agcombo | `SROM11_RPCAL_*` = words 182, 183, 190, 191, 198 |
| sub-band offsets (`SB20IN40*`, …) | words 200–218 | zero on every board | `bcmsrom_fmt.h` |
| reused `SSB_SPROM8_BOARDREV` | 0x0082 | DSL-3580L, D6220, bcm4360usb | revmask covers rev 11 |

## Filling the canonical column

1. Clone one of:
   - `https://github.com/RMerl/asuswrt-merlin`
     (`release/src-rt-7.x.main/src/include/bcmsrom_tbl.h`);
   - `https://android.googlesource.com/kernel/common.git`, branch `bcmdhd-3.10`;
   - `https://github.com/StreamUnlimited/broadcom-bcmdhd-4359`
     (`include/bcmsrom_tbl.h`).
2. Grep `bcmsrom_tbl.h` for entries whose `revmask` has bit 11 set (`0x00000800`
   rev 11 only, `0xfffff800` rev 11+).
3. Resolve each entry's `off` macro through `bcmsrom_fmt.h` into a word offset,
   multiply by 2, and compare with the constant.
4. On a mismatch, fix `include/linux/ssb/ssb_regs.h`, `harness/ssb_regs.h` and
   the two extractors, rerun the harness on every vector and regenerate
   `patches/0001`.
