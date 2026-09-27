# TX power: target `0x0646`, per-rate offsets, idle-TSSI

How the port derives:

- the per-core power target in `PHY 0x0646[7:0]` (and `0x0846`, `0x0a46`);
- the per-rate offsets it writes to shared memory;
- table `0x21`;
- the idle-TSSI base index.

The open residuals are in `retrace-todo.md`.

The sources are the captures, the boards' NVRAM, and the GPL `brcmsmac` sources
in `drivers/net/wireless/broadcom/brcm80211`. The stock object is used only to
read data tables.

## What the register is

`0x0646` is the **TX power target** in quarter-dBm, not a "max index". The
stock driver's writer takes the value as an argument. The ceiling lives
elsewhere, at `0x0b46`, written by the power-control enable path.

## The chain

`brcmsmac` carries the computation as `wlc_phy_txpower_recalc_target()`
(`phy/phy_cmn.c`), and b43 already ports it for the N-PHY as
`b43_nphy_op_recalc_txpower()` on its `struct b43_ppr`:

```
clear -> load_max_from_sprom -> apply_max(regulatory) -> add(-6)
      -> apply_min(8 dBm) -> get_max
```

`src/ppr_ac.{c,h}` is that PPR for SROM rev 11:

- the same primitives, and OFDM and MCS rows at 20/40/80 MHz;
- loaded from the minimum of `maxp5ga` over the active chains, plus the per-core
  delta, with the `mcsbw*po` nibbles per width;
- a 40 MHz channel also loads the 20 MHz row (20-in-40), and an 80 MHz channel
  loads the 20 and 40 MHz rows. The maximum over rates therefore takes the
  smallest offset among the contained widths.

The regulatory stage is `b43_phy_ac_reg_ceiling()`. It is
`QDB(max_power) − antenna_gain`, with the antenna gain from `aga0`
(`antenna_gain_qdb[1]`, decoded by `patches/0001`). cfg80211's `max_power` is
per 20 MHz channel, so a bonded block is bounded by its lowest channel.

`b43_phy_ac_txpwr_recalc()` computes the target. `recalc_txpower` returns
`NEED_ADJUST` only when the target changed, and `adjust_txpower` re-runs
`txpwrctrl_setup()` under `mac_suspend`, as `phy_n` does.

### Units, easy to conflate

| quantity | unit | source |
| --- | --- | --- |
| `maxp5ga[]`, `0x0646` | quarter-dBm | SROM |
| `mcsbw*po` nibbles | half-dB, hence `2 *` | SROM |
| `ch->max_power` | whole dBm, hence `QDB()` = `* 4` | regulatory |

The `−6` margin is a saturating subtraction, `(x > 6) ? x − 6 : 0`.

### What the captures pin down

**Same value cold and hot.** The value written is the same cold and hot on all
43 configurations (43 cold segments, 44 `up` segments), while RX-IQ and
idle-TSSI change between the two conditions. It is not closed-loop.

**The ceiling belongs to the locale, not the board.** The D6220 and the agcombo
have different SROMs yet write the same values where the SROM model would
differ:

| configuration | D6220 SROM | agcombo SROM | written, both |
| --- | --- | --- | --- |
| ch36–48 bw20 | 66/64 | 68 | **56** |
| ch60 bw40 | 64 | 68 | **60** |
| ch100 bw40 | 80 | 76 | **68** |
| ch100 bw20 and bw80 | 80 | 76 | **76** |

With the 6-unit margin and the boards' 5.5 dB antenna gain these are 21, 22, 24
and 26 dBm. Everywhere else the value is the SROM's.

`gates.sh` feeds the expressible part as
`AC_MAX_POWER_MAP=36:21,40:21,44:21,48:21,100:26`, in both conditions. The
ceilings at ch60/40 and ch100/40 hold at 40 MHz only, and cfg80211 cannot
express them.

**It is not an echo of any read.** Over the 87 D6220 segments, the only address
read before the first write whose value determines it is `RAD 0x08dc`: the PLL
word the driver itself wrote from the channel table, an echo of the channel.
On the same chip and configuration (ch52/20), `wl` 6.30 on the DSL-3580L writes
56 where 7.14 writes 62. A number that changes with the binary and not with the
chip lives in the binary: it is the CLM.

**80 MHz uses the `bw80` nibbles.** Only the D6220 has a `bw80` word that
differs from `bw20`, and its three 80 MHz observations score 1 of 3 with either
choice. The `bw80` branch errs **below** the stock driver (62 against 66); the
`bw20` branch errs **above** (80 against 76). With equal scores, the side that
does not push the PA harder is chosen.

## Per-rate offsets in shared memory

**The rule.** The per-rate field `+0x0e` of each rate block
(`b43_phy_ac_prb_rsp_rate_po()`) is `(max − ppr[rate]) * 4` on the finished
table.

**Which row.** Legacy OFDM rates take the row of the **operating width**. A
legacy OFDM frame on a bonded channel goes out duplicated over the whole block,
so it spends that width's budget.

**Where the ceiling applies.** It cuts the finished rows. The DSL-3580L's
`wl curpower` (`router-data/dsl3580l/wl1_curpower_ch52-bw80.txt`) shows every
target as `min(board limit, regulatory limit) − 1.5 dB`.

**`0x00ce`**, the beacon power offset (`b43_phy_ac_beacon_pwr_offset()`), has
the same form, always on the 20 MHz row.

**The block address.** It is read through the direct-map table, as
`brcms_b_rate_shm_offset()` does:

    block = 2 * shm_read(DIRMAP + index * 2)

- `M_RT_DIRMAP_A` = `0x01c0`, `M_RT_DIRMAP_B` = `0x0200`.
- The index is the low nibble of the PLCP SIGNAL field, so 6, 9, 12, 18, 24, 36,
  48 and 54 Mbit/s sit at indices 11, 15, 10, 14, 9, 13, 8 and 12.

**The PLCP and duration fields** at `+8/+10/+12` are computed as
`brcms_c_compute_ofdm_plcp()` and `brcms_c_calc_frame_time()`:

```
tmp      = len << 5
plcp     = nibble_rate | (tmp & 0xff), tmp >> 8, tmp >> 16
duration = 20 + ceil((len * 8 + 22) / NDBPS) * 4 + SIFS
```

`len` is the probe-response length, 277/278/279 at 20/40/80 MHz plus the SSID
length (`AC_SSID_LEN` in the harness). On hardware it will come from `PRTLEN`
(`0x004a`).

## Table `0x21`: the power-detector offsets

The 24 `u32` of table `0x21` are one byte per core (`0x0202` on the D6220,
`0x020202` on the agcombo). Over the 139 segments of the four sweeps they take
four payloads and no others:

- entries 1, 5 and 6 carry the sub-band nibble of `pdoffset40ma[core]`. All
  boards have `0x3222`: 2 up to U-NII-2C, 3 on U-NII-3;
- entry 10 carries the sub-band nibble of `pdoffset80ma[core]`. The agcombo
  alone has `0x0100`, hence 1 from ch100 up;
- everything else is zero.

The 80 MHz link is measured on two boards and three sub-bands. The 40 MHz link
follows from the same encoding and is not discriminated by the data.

## Idle-TSSI base index

`0x?645[9:0]` is written as:

    0x200 + sum(meas >> 2 over the passes with a non-zero measurement) / passes

`meas` is `0x0012` masked to its measurement field, the division truncates, and
the divisor is the total pass count. This is exact on 312 of 312 writes of
`0x0645`/`0x0845` over the sweep. Each detail is needed:

- **Skipping zero passes** (ch140 at 20 MHz otherwise misses three times).
- **Dividing by the total.** Dividing by the useful count instead gives 301 of
  312, all at 80 MHz, where 226–243 of the 256 passes carry a reading.
- **Truncating.** Rounding also gives 301.

At 20 and 40 MHz a block holds one usable pass, so a single reading and the
mean coincide. At 80 MHz there are 256 passes per core, and the arm is repeated
before every pair of reads.

The `0x200` is bit 11 of the readback surviving the shift. Core 1 measures zero
on every pass of every capture, which is why it writes `0x200` flat.

## Why the read-perturbation test cannot see some bugs

`test/unit/consumed_reads.sh` perturbs one address in the oracle and checks
whether the emitted trace changes. It has four kinds of false negative:

1. **The read's own trace line.** It carries the value, so the perturbed address
   must be filtered out of the diff.
2. **Masked bits.** A one-bit flip can fall in a bit the consumer drops
   (`0x0012` is consumed as `>> 2`), so several masks are needed.
3. **Magnitude.** The RX-IQ accumulators are sums over `0x4000` samples feeding
   a rounded quotient; only a large perturbation moves the output.
4. **Coverage.** Two mechanisms that agree on the configurations the gate runs
   are indistinguishable by construction. This is how the first-reading vs mean
   difference of the idle-TSSI would have hidden at 20 MHz.

There is also a category the test conflates with a bug: a read consumed only on
a branch never taken. `0x06a0`/`0x06a1` feed `rxiqcal_comp_update`'s give-up
path, which needs the measured power below `B43_PHY_AC_MIN_RXIQ_PWR` — never
true in any capture.
