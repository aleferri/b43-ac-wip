# SROM rev 11 fields on the PHY side

Which fields of `struct ssb_sprom` the AC-PHY reads, and where each one ends up
in the programming, as established against the stock traces. The field
layout is Broadcom's (`bcmsrom_tbl.h`, see
[`../bcma/cross_check.md`](../bcma/cross_check.md)).
Every other NVRAM variable is consumed by the core or not at all.

## What the driver reads

| field | use |
|---|---|
| `rxchain` | `& 0x07` → coremask |
| `subband5gver` | sub-band boundaries of `b43_phy_ac_pa5g_group()` (5250/5500/5745) |
| `core_pwr_info[].pa5ga` | est_pwr transfer function |
| `core_pwr_info[].maxp5ga` | PPR maximum per core and sub-band; `0` is a sub-band unavailable on that chain, not 0 dBm |
| `mcsbw{20,40,80}5g{l,m,h}po` | PPR per-rate offsets |
| `sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*`, `mcslr5gpo` | read to warn: zero on every board, not applied |
| `pdoffset40ma*`, `pdoffset80ma*` | table `0x21` |
| `antenna_gain_qdb` (`aga0`) | regulatory ceiling |
| `rxgains_5gl` | RX gain init on 5 GHz: the stock driver freezes it at attach |
| `rxgains_2g` | RX gain init on 2.4 GHz |
| `core_pwr_info[].pa2ga` | est_pwr transfer function on 2.4 GHz |
| `rpcal2g`, `rpcal5gb[4]` | table `0x11`: loaded when any is non-zero, filled with the phasor of the sub-band's word |
| `tssifloor5g` | PHY `0x0724 + c·0x200` |
| `femctrl` | guard on the FEM control table (`femctrl=6` only) |
| `pdgain5g`, `boardflags3`, `avvmid` | selection of the AvVmid set |
| `temps_period` | tempsense cadence in the watchdog |
| `phycal_tempdelta` | whether a full calibration opens with a temperature reading |

## Correlations

| field | transformation | destination | status |
|---|---|---|---|
| `pa5ga{c}[grp·3..]` | est_pwr transfer function, 128 entries | tables `0x40`/`0x60`/`0x80` | 128/128 on D6220 and agcombo |
| `rxgains_5gl.triso[c]` | `((triso+4)<<1)+2` | PHY `0x06f9 + c·0x200` bits 14:8 | both boards, per core |
| `rxgains_5gl.elnagain[c]` | `(elnagain+3)<<1` | table `0x44 + c·0x20`, offset 0 | verified |
| `maxp5ga`, `mcsbw*po`, `aga0` | the PPR chain ([`txpwr-target-derivation.md`](txpwr-target-derivation.md)) | PHY `0x0646 + c·0x200` bits 7:0, per-rate SHM offsets | 43 cold and 44 hot configurations |
| `pdoffset40ma`, `pdoffset80ma` | sub-band nibble per core | table `0x21`, entries 1/5/6 and 10 | 139 segments |
| `tssifloor5g` | `& 0x3ff` per chain | PHY `0x0724 + c·0x200` | **SALAME**: `0xffff` on every board, so the destination is inferred |

The rxgains byte packs `(trelnabyp<<7)|(triso<<3)|elnagain`. The antenna gain
fields keep brcmsmac's encoding, whole dB in bits [5:0] and quarters in
[7:6] (`133` = 5.5 dB).

## Open

- The destination of `tssifloor5g`, which needs a board with a programmed
  floor.
- `tssiposslope5g`, `tworangetssi5g`: constant on every board.
- Which of `aga0..2` the stock driver reads: all three are 133 on the
  reference boards.
