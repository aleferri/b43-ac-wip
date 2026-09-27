# RX IQ calibration

What the AC-PHY RX IQ calibration does, as the captures show it and as the port
implements it (`b43_phy_ac_iq_solve()` and its callers in `src/phy_ac.c`,
`src/rxiqcal_phy_ac.c`). The residual on the `b` coefficient is in
`retrace-todo.md`.

## Structure

The block runs inside the post-switch calibrations. It uses the hardware
estimator (command register `0x0270`, sample count `0x0272`, accumulators
`0x?c0`–`0x?c5`: `ii`, `qq`, `iq` as three 32-bit hi/lo pairs per chain, read
hi first). It has three stages:

1. **Loopback gain search**, `SAMCNT = 0x0400`. The index in
   `0x0734 + core*0x200` starts at 4 on 5 GHz (0 on 2 GHz) and **halves** —
   4, 2, 1, 0 — while the per-sample power `round(ii/1024) + round(qq/1024)` is
   above `0x169e` (5790). The search stops at or below that value, or at
   index 0, and the index never rises.
   - The lower edge `0xb57`, read at the start, has no observed effect.
   - Rounds of this stage are the only ones preceded by a touch of PHY `0x0b22`.
   - Implemented in `b43_phy_ac_loopback_step` and
     `b43_phy_ac_loopback_gain_search`.
2. **Measurement tones**, `SAMCNT = 0x4000`. Two tones up to 40 MHz, six at
   80 MHz. All tones are the same 20-entry period sampled at a step: +1 and −1
   up to 40 MHz, then +3, −3, +4, −4 at 80 (`b43_phy_ac_tone_steps[]`). The
   step is the tone frequency in units of rate/20. The 80 MHz block needs three
   pairs because the band to cover is four times wider.
3. **Coefficients and teardown.** Two 10-bit signed coefficients per chain in
   `0x?a0` (`a`) and `0x?a1` (`b`), the same format as the N-PHY. Then the
   compensation registers are restored and the tone engine is stopped.

**The mute state.** The writes to `0x0720`–`0x0729` after the first precision
measurement are the save/restore of the mute state of the chains not being
measured, not coefficients. That is the AFE page `switch_analog` also saves.

## The solve

Per tone, from the stock driver's `wlc_phy_calc_rx_iq_comp_acphy`:

```
a_r = round(-(iq << 10) / ii)
b_r + 1 = 2^10 * sqrt(qq*ii - iq^2) / ii        (exact a_r under the root)
```

**Averaging.** What goes into `0x?a0`/`0x?a1` is the frequency-independent part
of the imbalance, so the driver **averages the per-tone coefficients**. `a` is
the mean of the `a_r`. `b` is the mean of the `b_r`, rounded half up; on two
values that coincides with the ceiling.

Summing the accumulators over the tones instead weights each tone by its power.
At 80 MHz, where roll-off makes the powers differ and the per-tone `a` spread by
tens of units, that is off by units.

**Scores.** On 118 points (every `0x?a1` write with its rounds, over the sweeps)
`a` matches on 117 and `b` on 90.

- The one `a` miss is `13-up-ch60-bw20` core 0: the tones give −4.592 and
  −14.380, the mean −9.486, and the stock driver writes −10. No rounding of the
  mean gives it.
- **C pitfall.** `-((-b) / n)` is a ceiling in Python and a floor in C. The
  C form is `(b + n - 1) / n`.

## Table `0x0c` (IQLOCAL) and the TX LO LUTs `0x42`/`0x62`/`0x82`

**IQLOCAL.** Table `0x0c` holds the TX IQ `{a, b}` at `0x60 + 4*core` and the
LO leakage word, two signed bytes, at `0x62 + 4*core`.
`rxiqcal_finalize()` reads `0x60`–`0x66` and rewrites what it read, the same
way it does for the LO DAC registers `0x?002`–`0x?005`.

**The LO LUTs.** Tables `0x42`/`0x62`/`0x82` are the LO leakage LUTs of TX
power control, one entry per power index:

- **The first pass** (`rxcal_afe_finalize_gain_luts()`) fills all 128 indices
  with the LO word the TX IQ/LO calibration has just produced for that core,
  plus a per-core base. The driver derives them from `afe_res[1]`, `[3]` and
  `[5]`.
- **The second pass** (`rxiqcal_coeff_tables_reset()`) writes the bare base.

The base depends on the pa5g sub-band. Each field of a word is a separate
signed byte, and the subtraction is done per field:

| | groups 0 and 1 | group 2, ch100–144 | group 3, ch149–165 |
| --- | --- | --- | --- |
| `0x42` | 0 | 0 | 0 |
| `0x62` | 0 | 0 | `f6fb`, `ec00` from `0x1b` |
| `0x82` | `f8fc`, `f6f2` from `0x21` | `fafb`, `f6f6` from `0x1d` | `fafb`, `f6f6` from `0x1d` |

This holds on every segment of both D6220 sweeps that runs the phase. The steps
and their sizes are tabulated, not derived.

## The 80 MHz eleven-tap bank

At 80 MHz only, the stock driver programs per chain an eleven-tap bank at
`0x?6a4`–`0x?6ae`, right after the RX IQ coefficients. It is present on every
80 MHz segment that runs the calibration and on no 20/40 MHz segment.

**The kernel.** The values are constant, identical on two boards, every channel,
cold and hot. Each series is an antisymmetric `1/k` kernel around a centre tap
of `0x0400` (unity in Q10), a group-delay corrector with one free parameter `c`:

| series | c | taps |
| --- | --- | --- |
| A | ~60.5 | `000c fff1 0014 ffe2 003d 0400 ffc4 001e ffec 000f fff4` |
| B | ~182 | `0025 ffd2 003d ffa4 00b8 0400 ff4c 005b ffc4 002d ffdc` |
| C | ~208 | `002a ffcc 0046 ff97 00d3 0400 ff32 0067 ffbb 0034 ffd6` |

**Which series goes where.** What selects `c` is not established, so the table
is indexed on (chains, core): the D6220 takes `{A, B}`, the agcombo `{C, B, A}`.
A chain count outside the table emits nothing and warns. The bank is written by
`b43_phy_ac_bw80_fir_write()`.

## Tempsense uses the same gain block

`b43_phy_ac_tempsense()` programs the RX gain block `0x?720`–`0x?73e`
(`rx_gain_regs_program()`) and the radio (`tempsense_radio_setup()`). Then, per
chain, it steps radio `0x?00e` four times with eight reads of PHY `0x0013`
each, and restores the 14 PHY and 7 radio registers to their values read at the
start. It is 391 operations on two chains and 579 on three.

It is not part of the RX IQ calibration. The bss-up calls it before the full
calibration, and the watchdog every `temps_period` turns.

Four fields of the gain block are selected by width (`b43_phy_ac_rxgain_bw()`),
identical at bss-up and in the watchdog:

| field | BW20 | BW40 | BW80 |
|---|---|---|---|
| `0x?73a` mask `0x0007` | `0x0003` | `0x0002` | `0x0000` |
| `0x?739` mask `0x007e` | `0x007a` | `0x007a` | `0x007e` |
| `0x?73a` mask `0x0008` | `0x0000` | `0x0000` | `0x0008` |
| `0x?73a` mask `0x0060` | `0x0040` | `0x0000` | `0x0000` |

## Not covered by the data

- The scale of `B43_PHY_AC_MIN_RXIQ_PWR`: the captured powers are orders of
  magnitude above it.
- Rounding at an exact fraction of 0.5, absent from the points.
- What the stock driver does if the gain search is still above the window at
  index 0, or below it at 4. The port accepts the extreme index.
