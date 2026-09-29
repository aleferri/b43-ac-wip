# TX power: target `0x0646` and per-rate offsets

How the port derives the per-core power target in `PHY 0x0646[7:0]` (and
`0x0846`, `0x0a46`) and the per-rate offsets in shared memory. The open
residuals are in `retrace-todo.md`; the idle-TSSI base, table `0x21` and the
rate-block PLCP carry their evidence next to the code in `src/phy_ac.c`.

`0x0646` is the TX power target in quarter-dBm. The ceiling lives elsewhere,
at `0x0b46`, written by the power-control enable path.

## The chain

It is brcmsmac's `wlc_phy_txpower_recalc_target()`, which b43 already ports for
the N-PHY as `b43_nphy_op_recalc_txpower()`:

```
clear -> load_max_from_sprom -> apply_max(regulatory) -> add(-6)
      -> apply_min(8 dBm) -> get_max
```

`src/ppr_ac.{c,h}` is that PPR for SROM rev 11, one row of eight
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
`wl curpower`: `router-data/agcombo/stats.txt` (7.14.43, ch100/80 and ch36/80)
and `router-data/dsl3580l/wl1_curpower_ch52-bw80.txt` (6.30, ch52, a word whose
eight nibbles all differ, which fixes every row).

## What the captures pin down

- **Same value cold and hot** on all 43 cold and 44 `up` configurations, while
  RX-IQ and idle-TSSI change between the two: it is not closed-loop.
- **The ceiling belongs to the locale, not the board.** The D6220 and the
  agcombo have different SROMs and write the same values where the SROM model
  would differ:

  | configuration | D6220 SROM | agcombo SROM | written, both |
  | --- | --- | --- | --- |
  | ch36–48 bw20 | 66/64 | 68 | **56** |
  | ch60 bw40 | 64 | 68 | **60** |
  | ch100 bw40 | 80 | 76 | **68** |
  | ch100 bw20 and bw80 | 80 | 76 | **76** |

  With the margin and the 5.5 dB antenna gain these are 21, 22, 24 and 26 dBm.
  `gates.sh` feeds the expressible part as
  `AC_MAX_POWER_MAP=36:21,40:21,44:21,48:21,100:26`. On ch52/20 `wl` 6.30 on
  the DSL-3580L writes 56 where 7.14 writes 62 on the same chip: the value lives
  in the binary's CLM, not in any read.
- **At 80 MHz every contained row enters the maximum.** Only the D6220 has
  `bw20`, `bw40` and `bw80` words that differ, and its six 80 MHz segments need
  all three (ch36 takes the 20-in-80 row, ch52 the 40-in-80 row); either word
  alone misses.

## Per-rate offsets in shared memory

- The field `+0x0e` of each rate block (`b43_phy_ac_prb_rsp_rate_po()`) is
  `(max − ppr[rate]) * 4` on the finished table, the ceiling applied to the
  rows. `wl curpower` on the DSL-3580L shows every target as
  `min(board, regulatory) − 1.5 dB`.
- Legacy OFDM rates take the row of the operating width: on a bonded channel
  the frame goes out duplicated over the whole block.
- `0x00ce`, the beacon power offset (`b43_phy_ac_beacon_pwr_offset()`), has the
  same form on the 20 MHz row.
- The block address is `2 * shm_read(DIRMAP + index * 2)`, as
  `brcms_b_rate_shm_offset()`; `M_RT_DIRMAP_A` = `0x01c0`, the index is the low
  nibble of the PLCP SIGNAL field.
