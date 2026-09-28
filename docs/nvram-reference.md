# NVRAM / SROM rev 11 reference — meaning and use on the PHY side

This file does two things:

1. it gives the meaning of the NVRAM variables in the dumps under
   `router-data/*/`;
2. it records **where** each value ends up in the PHY/radio programming, as far
   as established by comparing the driver with the stock traces.

The field semantics are those of Broadcom's SROM rev 11 layout (`bcmsrom_tbl.h`;
see [`../src/sprom-rev11/cross_check.md`](../src/sprom-rev11/cross_check.md)). The
correlations come from the boards with register traces.

## Confidence legend

| mark | meaning |
|---|---|
| **✓ verified** | reproduced by the driver and confirmed op-for-op in the trace |
| **~ standard** | meaning known from `bcmsrom_tbl.h`; footprint not checked here |
| **⚠ SALAME** | plausible but not confirmed by the available data |
| **TODO** | encoding, unit or formula still to derive or verify |

## Naming convention

Almost every variable is an indexed variant of a few families. The suffix is
enough to read it:

- `a0` / `a1` / `a2`: core (chain) 0, 1, 2;
- `2g` / `5g`, or `5gl` / `5gm` / `5gh`: band (2.4 GHz / 5 GHz, or low/mid/high
  U-NII);
- `bw20` / `bw40` / `bw80` / `bw160`: channel width;
- `po`: power offset (backoff), almost always packed per rate in nibbles;
- `ma0..2` in `pdoffset*`: per core.

Comma-separated arrays are indexed per sub-band (four values, the four 5 GHz
sub-bands) or per core.

## What the driver reads from `bus_sprom`

The PHY driver reads these fields; every other NVRAM value is either consumed
by the b43 core outside the AC-PHY path, or not consumed at all.

| field | use |
|---|---|
| `rxchain` | `& 0x07` → coremask |
| `subband5gver` | sub-band boundaries of `b43_phy_ac_pa5g_group()` |
| `core_pwr_info[].pa5ga` | est_pwr transfer function |
| `core_pwr_info[].maxp5ga` | PPR maximum per core and sub-band |
| `mcsbw{20,40,80}5g{l,m,h}po` | PPR per-rate offsets |
| `sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*`, `mcslr5gpo` | read to warn: zero on every board, not applied |
| `pdoffset40ma*`, `pdoffset80ma*` | table `0x21` |
| `antenna_gain_qdb` (`aga0`) | regulatory ceiling |
| `rxgains_5gl` | RX gain init |
| `tssifloor5g` | PHY `0x0724 + c·0x200` |
| `femctrl` | guard on the FEM control table (`femctrl=6` only) |
| `pdgain5g`, `boardflags3`, `avvmid` | selection of the AvVmid set |
| `temps_period` | tempsense cadence in the watchdog |
| `gpio0` | LED pins (harness) |

## Verified correlations

| NVRAM | transformation | destination | confidence |
|---|---|---|---|
| `pa5ga{c}[grp·3..]` | est_pwr transfer function (128 entries) | tables `0x40`/`0x60`/`0x80` (core 0/1/2) | ✓ 128/128 on D6220 and agcombo |
| `rxgains_5gl.triso[c]` | `((triso+4)<<1)+2` | reg `0x06f9 + c·0x200`, bits 14:8 (`0x1600`) | ✓ per core, both boards |
| `rxgains_5gl.elnagain[c]` | `(elnagain+3)<<1` | table `0x44 + c·0x20`, offset 0 | ✓ |
| `maxp5ga`, `mcsbw*po`, `aga0` | the PPR chain (see `txpwr-target-derivation.md`) | reg `0x0646 + c·0x200` bits 7:0; per-rate SHM offsets | ✓ on 43 cold and 44 hot configurations |
| `pdoffset40ma`, `pdoffset80ma` | sub-band nibble per core | table `0x21`, entries 1/5/6 and 10 | ✓ on 139 segments |
| `rxchain` | `& 0x07` → coremask | number of per-core blocks | ✓ structural |
| `subband5gver` | boundaries 5250/5500/5745 → pa5g group | slice of `pa5ga`/`maxp5ga` | ✓ indirect |
| `tssifloor5g` | `& 0x3ff`, per chain | reg `0x0724 + c·0x200` | ⚠ SALAME: `0xffff` on every board, so the destination is inferred |
| idle-TSSI (not NVRAM) | runtime measurement | reg `0x0645 + c·0x200` (mask `0x03ff`) | ✓ measured; not derivable from NVRAM |

## Details per family

### Board and host identity

- `sromrev`: SROM format (11). ~
- `boardrev`: board revision, for example `0x1355` P355 and `0x1353` P353. ~
- `boardtype`: board id (`0x668` DSL/D6220, `0x633` agcombo, `0x6d8`
  TG789vac v2). ~
- `boardflags`, `boardflags2`, `boardflags3`: capability/config bitmasks. `boardflags3`
  carries `BFL3_AvVim`, which selects the AvVmid source. ~
- `subvid`: subsystem vendor (`0x14e4` = Broadcom). ~
- `boardnum`: board serial. ~
- `macaddr`: MAC. ~
- `devid`: PCI device id (`0x43b3` / `0x43a2`). ~
- `ccode` + `regrev`: regulatory domain; empty means world, regrev 0. It is the
  locale the first bring-up runs under. ~
- `ledbh*`: LED behaviour per GPIO. `ledbh0-3` sit in the SROM, `ledbh4-15`
  reach `struct ssb_sprom` through `patches/0016`. Consumed by the core
  (`leds.c`), not the AC-PHY. ~
- `watchdog`: watchdog period in ms (3000 on D6220 and agcombo, 70000 on
  TG789vac); whether it governs the turn cadence is not verified. ~
- `xtalfreq`: crystal frequency (`0xffff` = default). ~

### Antenna and chain topology

- `aa2g`, `aa5g`: available-antenna bitmasks per band. ~
- `txchain`, `rxchain`: active-chain bitmasks; `rxchain & 7` → coremask (3 =
  2×2, 7 = 3×3). ✓ for rxchain
- `antswitch`: antenna switch config. ~
- `agbg0..2`: 2.4 GHz antenna gain per antenna, in brcmsmac's encoding: whole dB
  in bits [5:0], quarters in bits [7:6] (`71` = 7.25 dB). ✓ encoding
- `aga0..2`: 5 GHz antenna gain, same encoding (`133` = 5.5 dB = 22 quarters).
  It enters the regulatory ceiling as `QDB(max_power) − antgain`. Which of the
  three `wl` reads is not distinguishable on the reference boards, all at 133;
  `patches/0001` decodes `aga0` into `antenna_gain_qdb[1]`. ✓ 2 boards

### Front end and path configuration

- `femctrl`: front-end module control scheme. The driver ports `femctrl` 6's
  control table (the only value seen, all four boards) and stops with a warning
  on any other value. ✓
- `epagain{2g,5g}`: external PA gain. ~
- `pdgain{2g,5g}`: power-detector gain; 10 on three boards, 19 on the TG789vac.
  Selects the AvVmid set. ✓
- `papdcap{2g,5g}`: PAPD capability. ~
- `tssiposslope{2g,5g}`: **sign** of the TSSI slope (1 bit; `1` means TSSI grows
  with power). Constant everywhere. ~
- `tworangetssi{2g,5g}`: two-range TSSI (1 bit; `0` everywhere). ~
- `gainctrlsph`: gain-control mode. ~ (low confidence)
- `paparambwver`: format version of the width-specific PA parameters. ~
- `subband5gver`: version of the 5 GHz sub-band split; fixes the boundaries used
  by `pa5g_group()`. ✓ indirect

### PA coefficients (pdet polynomial)

Each group is a triplet `(a1, b0, b1)` of the pdet model.

- `pa2ga0..2`: 2.4 GHz per core. ~
- `pa2gccka0`: CCK variant. ~
- `pa5ga0..2`: 5 GHz, 12 values = 4 sub-bands × 3 coefficients per core. Feeds
  the est_pwr transfer function (`a1/b0/b1 = pa5ga[grp·3 .. grp·3+2]`), written
  to tables `0x40/0x60/0x80`. ✓ 128/128
- `pa5gbw40a0`, `pa5gbw80a0`, `pa5gbw4080a{0,1}`: the same coefficients for 40/80
  MHz channels. ~ (not ported)

### Maximum power and per-rate offsets

- `maxp2ga0..2`: 2.4 GHz maximum power per core (quarter-dBm; 66 = 16.5 dBm). ~
- `maxp5ga0..2`: 5 GHz maximum power per core, 4 sub-band values; the PPR
  maximum. `maxp5ga0[3] = 0` on the D6220 is a sub-band capped to zero, to be
  handled as "unavailable on that chain", not as 0 dBm. ✓
- `mcsbw{20,40,80}5g{l,m,h}po`: per-MCS offsets packed per band and width, read
  as **unsigned** nibbles in half-dB. The per-rate SHM offsets are
  `(max − ppr[rate]) * 4`. ✓
- `mcsbw160...po`: 80+80/160 MHz offsets. ~
- `mcslr5g{l,m,h}po`: low-rate offsets. ~ (read, zero)
- `cckbw202gpo`, `cckbw20ul2gpo`, `dot11agofdmhrbw202gpo`, `ofdmlrbw202gpo`:
  2.4 GHz offsets. ~
- `sb20in40...po`, `sb20in80and160...po`, `sb40and80...po` (`hr`/`lr`),
  `dot11agdup{hr,lr}po`: sub-channel offsets inside wider widths. Zero on every
  board; not applied, because which nibble goes to which rate is not in any open
  source. TODO
- `sar2g`, `sar5g`: SAR limits. ~
- `txidxcap{2g,5g}`: TX power index cap. ~

### Power detector, TSSI, temperature

- `pdoffset40ma0..2`, `pdoffset80ma0..2`: power-detector offset per core and
  width, one nibble per sub-band → table `0x21`. ✓
- `pdoffset2g40ma*`, `pdoffsetcckma*`: 2.4 GHz and CCK variants. ~
- `tssifloor{2g,5g}`: TSSI floor per sub-band (10 bits). ⚠ SALAME: the value is
  `0xffff` everywhere, so the destination `0x0724` is inferred from value and
  structure; it needs a board with a programmed floor.
- `measpower{,1,2}`: measured reference power (`0x7f` = unset). ~
- `tempthresh`, `tempoffset`, `rawtempsense`, `tempsense_slope`, `tempcorrx`,
  `tempsense_option`: temperature sensor (`0xff`/`0x1ff`/`0x3f` = default on the
  reference boards). ~
- `phycal_tempdelta`: temperature delta that triggers a recalibration (`0xff` =
  disabled). The runtime instance value can differ from NVRAM (40 on the
  TG789vac and the DSL-3580L). ~
- `temps_period`, `temps_hysteresis`: tempsense cadence and hysteresis;
  `temps_period` is read. ✓

### RX gain and noise

- `rxgains{2g,5g,5gm,5gh}{elnagain,triso,trelnabyp}a0..2`, per core and band:
  - the fields are the eLNA gain index, T/R isolation and eLNA bypass in T/R;
  - they are packed as `(trelnabyp<<7)|(triso<<3)|elnagain`;
  - the driver uses **only** `rxgains_5gl`, which the stock driver freezes at
    attach for every band. ✓ for 5gl
- `rxgainerr{2g,5g}a0..2`: per-core/sub-band gain error correction; flat
  `31,31,31,31` on the D6220. ~
- `noiselvl{2g,5g}a0..2`: noise level per core and sub-band. ~
- `eu_edthresh{2g,5g}`: EU energy-detect threshold (regulatory). ~

### Path calibrations

- `rpcal2g`, `rpcal5gb0..3`: RX path calibration coefficients per 5 GHz band. ~
- `pcieingress_war`: PCIe ingress workaround (DSL-3580L only). ~

## Open

- Exact encoding of the `pdoffset*` families beyond the nibbles observed.
- Destination of `tssifloor5g`, which needs a board with a programmed floor.
- `tssiposslope5g` / `tworangetssi5g`: constant on every board; a board with
  `tssiposslope5g=0` or `tworangetssi5g=1` would show their footprint.
- The meaning of individual `boardflags*` bits and of `gainctrlsph`.
