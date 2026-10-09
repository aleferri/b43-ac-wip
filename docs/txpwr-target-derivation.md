# TX power: target `0x0646` and per-rate offsets

How the port derives the per-core power target in `PHY 0x0646[7:0]` (and
`0x0846`, `0x0a46`) and the per-rate offsets in shared memory. The open
residuals are in `retrace-todo.md`; the idle-TSSI base, table `0x21` and the
rate-block PLCP carry their evidence next to the code in `b43/phy_ac.c`.

`0x0646` is the TX power target in quarter-dBm. The ceiling lives elsewhere,
at `0x0b46`, written by the power-control enable path.

## The chain

It is brcmsmac's `wlc_phy_txpower_recalc_target()`, which b43 already ports for
the N-PHY as `b43_nphy_op_recalc_txpower()`:

```
clear -> load_max_from_sprom -> apply_max(regulatory) -> add(-6)
      -> apply_min(8 dBm) -> get_max
```

`b43/ppr_ac.{c,h}` is that PPR for SROM rev 11, one row of eight
modulation-class groups per width. It loads the minimum of `maxp5ga` over the
active chains plus the per-core delta, with the `mcsbw*po` nibbles per width. A
40 MHz channel also loads the 20 MHz row, an 80 MHz channel the 20 and 40 MHz
rows, so the maximum takes the smallest offset among the contained widths.

| quantity | unit |
| --- | --- |
| `maxp5ga[]`, `0x0646` | quarter-dBm |
| `mcsbw*po` nibbles | half-dB, hence `2 *` |
| `ch->max_power` | whole dBm, hence `QDB()` = `* 4` |

The `−6` margin saturates at 0. The regulatory stage is
`b43_phy_ac_reg_ceiling()`, `QDB(max_power) − antenna_gain` with the gain from
`aga0`; cfg80211's `max_power` is per 20 MHz channel, so a bonded block is
bounded by its lowest channel.

### The nibble → rate mapping

Each `mcsbw{20,40,80}5g{l,m,h}po` word is eight nibbles, one per modulation and
coding class:

| nibble | class | OFDM | MCS |
|---|---|---|---|
| 0 | BPSK, QPSK | 6, 9, 12, 18 | 0, 1, 2 |
| 1 | 16-QAM 1/2 | 24 | 3 |
| 2 | 16-QAM 3/4 | 36 | 4 |
| 3 | 64-QAM 2/3 | 48 | 5 |
| 4 | 64-QAM 3/4 | 54 | 6 |
| 5 | 64-QAM 5/6 | — | 7 |
| 6 | 256-QAM 3/4 | — | 8 |
| 7 | 256-QAM 5/6 | — | 9 |

The evidence is the stock driver's own decode, the "Board Limits" of
`wl curpower`: `router-data/vd625-agcombo/stats.txt` (7.14.43, ch100/80 and
ch36/80) and `router-data/dsl3580l/wl1_curpower_ch52-bw80.txt` (6.30, ch52, a
word whose eight nibbles all differ, which fixes every row).

## What the captures pin down

- **Same value cold and hot** on all 43 cold and 44 `up` configurations, while
  RX-IQ and idle-TSSI change between the two: it is not closed-loop.
- **The ceiling belongs to the locale, not the board.** The D6220, the
  agcombo and the TG789vac have different SROMs and antenna gains (5.5, 0,
  4.25 dB) and write the same target where a limit binds:

  | configuration | D6220 | agcombo | TG789vac | written, all | cap before margin |
  | --- | --- | --- | --- | --- | --- |
  | ch36–48 bw20 | 66/64 | 68 | 80/78 | **56** | 62 |
  | ch64 bw20 | 62 | 68 | 82 | 62 / 68 / **76** | 82 |
  | ch100 bw20 and bw80 | 80 | 76 | 86 | **76** | 82 |
  | ch104–128 bw20 | 80 | 76 | 86 | 80 / 76 / **84** | 90 |
  | ch132–144 bw20 | 80 | 76 | 86 | 80 / 76 / **80** | 86 |
  | ch36–44 bw40, ch36 bw80 | 66 | 68 | 82 | 66 / 68 / **68** | 74 |
  | ch60 bw40 | 64 | 68 | 82 | **60** | 66 |
  | ch100 bw40 | 80 | 76 | 86 | **68** | 74 |

  The caps are conducted (the same number on three gains) and per width, so
  cfg80211's one EIRP per 20 MHz channel cannot express them. The port
  carries them as `b43_phy_ac_locale_ceiling()`, the tighter of the two
  with the cfg80211 ceiling, as brcmsmac carries `locale_5g_*` in
  `channel.c`. On ch52/20 `wl` 6.30 on the DSL-3580L writes 56 where 7.14
  writes 62 on the same chip: the value lives in the binary's CLM, not in any
  read.
- **At 80 MHz every contained row enters the maximum.** Only the D6220 has
  `bw20`, `bw40` and `bw80` words that differ, and its six 80 MHz segments need
  all three (ch36 takes the 20-in-80 row, ch52 the 40-in-80 row); either word
  alone misses.

## Per-rate offsets in shared memory

- The field `+0x0e` of each rate block (`b43_phy_ac_prb_rsp_rate_po()`) is
  the rate's distance from the target in sixteenths of a dB, and it is read
  on the SROM spacing, not on the finished table: the D6220 in UNII-3 has
  `maxp5ga` 0, the target at the floor, and still writes 16/32/48 on the
  three upper rates, and on ch100/20 it keeps 16/32/48 with the target 1 dB
  under the top row. So the general ceiling moves the target and leaves the
  spacing. `txpwr_spacing` is the same PPR laid out from 0x7f so that no
  entry saturates; `b43_phy_ac_rate_po()` reads it.
- **The legacy OFDM rates carry their own limit** (`b43_phy_ac_legacy_cap()`:
  a single-chain limit per channel and width less the CDD offset of the
  chains in 0x05d6), which shows in this field alone since the target takes
  the maximum over MCS: 76 after the margin on ch52–144 at 20 MHz, 72 on
  ch100–128 at 80, 84 on ch108–140 at 40 in the stock driver. On the D6220 and the TG789vac every legacy rate the
  spacing would put higher sits at that value (ch104/20: `target − 76` on the
  eight legacy fields on both, targets 80 and 84). The field is
  `max(spacing, target − limit)`.
- On the 4352 (D6220) the legacy rows keep their SROM distance from a
  capped target, as above (`b43_phy_ac_rate_po()`); on the 4360 (TG789vac,
  agcombo) each rate sits at its own entry of the finished table
  (`b43_phy_ac_rate_po_capped()`, under `b43_phy_ac_legacy_capped()`). What
  still misses is in `retrace-todo.md`, "Per-rate field `+0x0e`".
- **The CCK rates sit at the 1 dBm floor**: their field
  (`b43_phy_ac_cck_rate_po()`) is `min(0xf8, 4 · (target − 4))` on all 86
  cold segments of the D6220 and the TG789vac, every width and sub-band
  (0xd0 at 56, 0xe0 at 60, 0xe8 at 62, 0xf0 at 64, 0xf8 from 66). The one
  exception is the D6220's UNII-3, `maxp5ga` 0, where the vendor writes 0xf8
  and the rule gives 0.
- `0x00ce`, the beacon power offset (`b43_phy_ac_beacon_pwr_offset()`), has the
  same form on the 20 MHz row, the legacy limit included.
- The block address is `2 * shm_read(DIRMAP + index * 2)`, as
  `brcms_b_rate_shm_offset()`; `M_RT_DIRMAP_A` = `0x01c0`, the index is the low
  nibble of the PLCP SIGNAL field.

## Chain masks `0x05d6` / `0x05d8` / `0x05da`

The three cells of the `0x05d4`–`0x05dc` block that are not coremask are the
chain choice of the rate classes, and they follow the regulatory headroom,
not the sub-band. Per class the stock driver takes the chain count `n` with
the highest total power

    min(board, L − offset[n]) + 10·log10(n)

fewer chains on a tie, with the `wl curpower` offsets (CDD on 2 and 3 chains
3 and 5 dB under one chain, TXBF 6 and 9.75), `board` the top row of the
operating width and `L` the locale's Local Max for the channel and width less
the board's antenna gain. `0x05d6` behaves as the TXBF rows and `0x05d8` as
the CDD rows; in closed form, with `d = L − board` in quarter dB:

| cell | 1 chain | 2 chains | 3 chains |
| --- | --- | --- | --- |
| `0x05d8` (CDD) | d ≤ 0 | 1 ≤ d ≤ 13 | d ≥ 14 |
| `0x05d6` (TXBF) | d ≤ 12 | 13 ≤ d ≤ 32 | d ≥ 33 |

`0x05da` follows `0x05d8` where that keeps more than one chain and is
coremask where it drops to one (7.14.89; the agcombo on 7.14.43 writes
coremask there throughout). Two of three chains is the outer pair, 0x5.

The Local Max is neither the cap that binds the target (at ch64/20 the target
caps at 20.5 dBm and the masks need 30 or more) nor in cfg80211. It is
measured from the masks: per channel and width, the range of `L` that
reproduces each board's pair, intersected over the D6220 (2 chains, 5.5 dB),
the TG789vac (3, 4.25) and the agcombo (3, 0). On 18 of the 19 channel/width
pairs the three boards see, the ranges intersect, and
`b43_phy_ac_local_max()` takes a value inside: 84 on ch36–48/20, 120 on
ch52–64/20, 124 on ch100–144/20, 106 on ch36–44/40, 136 on ch108–140/40,
104 on ch36/80, 132 on ch100–132/80 (quarter-dBm EIRP). A channel not in the
table takes coremask. The one miss is the agcombo's ch36/40 first pass; the
antenna gain the agcombo's driver reports (0) is not its SROM's (5.5), so
with the SROM gain the port is wrong on that board and right on the other
two.

**The stock limits**, from the three `curpower` dumps
(`router-data/vd625-agcombo/stats.txt` at ch100/80 and ch36/80,
`router-data/dsl3580l/wl1_curpower_ch52-bw80.txt`, two driver branches), per
rate class, chain count and width; their structure is the same in all three
to the quarter dB, only the level moves:

| rows | offset |
|---|---|
| 1 chain (OFDM, MCS0-7, VHT8-9), 20in80 | Local Max − 2 dB |
| 1 chain, 40in80 and 80 | Local Max − 1 dB |
| CDD on 2 chains, STBC | 1 chain − 3 dB |
| CDD on 3 chains | 1 chain − 5 dB |
| TXBF on 2 chains | 1 chain − 6 dB |
| TXBF on 3 chains | 1 chain − 9.75 dB |

## The second pass

The TX power site (`b43_phy_ac_txpwr_adjust()`) writes the block a second
time, between two passes with the channel's own numbers. On every 40 and
80 MHz segment of the three boards that pass is the first one except where
a 20-in-40/80 cap of the primary channel binds, ch36–48 at 68 quarter-dBm,
and it is not the 20 MHz computation of the primary: on ch100/40 that would
give 76 where every pass writes 68. Where the cap binds (ch36 and ch44 at
40 MHz, ch36 at 80, the D6220 and the agcombo) the pass differs from the
first in two things:

- the target takes the operating row under that cap: D6220 `min(72, 68) − 6
  = 62`, agcombo `min(74, 68) − 6 = 62`, against 66 and 68 on the passes
  around it;
- the legacy OFDM rates take the limit of the primary's **20 MHz
  configuration**, its cap and its chain pair: 62 − 6 = 56 on the D6220 (one
  chain) and 56 − 12 = 44 on the agcombo (two chains, the CDD offset), each
  board's own ch36/20 legacy power. The CCK field and the beacon cell follow
  the target (0xe8, 0x18).

In the port the cap is `b43_phy_ac_locale_20in[]`, applied to the ceiling
under `ac->txpwr_pass2` around that pass, and `b43_phy_ac_legacy_cap()`
takes the width of the limit and of the chain pair separately, so the pass
can evaluate the 20 MHz configuration while the chain masks of the block
itself stay the operating width's. The TG789vac leaves its 68 on that pass
(its own 40 MHz row 88 under the cap 74) and moves the masks only
(ch108–140/40: 5 to 7); it has the same `wl` as the D6220 and the same
empty `ccode`, so what distinguishes it is not known and the port follows
the D6220 (`retrace-todo.md`). The masks on that site take the Local Max
1 dB higher above 20 MHz, which reproduces every board but is a fit, not the
mechanism.
