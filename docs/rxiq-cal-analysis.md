# RX IQ calibration

What the AC-PHY RX IQ calibration does, as the captures show it and as the port
implements it (`b43_phy_ac_iq_solve()` and its callers in `b43/phy_ac.c`; the
estimator that validates the solve against the captures is the harness's,
`test/unit/rxiqcal_phy_ac.c`). The residuals are in `retrace-todo.md`; the LO
LUT bases and the five series of the 80 MHz eleven-tap bank are tabulated, with
their evidence, next to the code.

## Structure

The block runs inside the post-switch calibrations on the hardware estimator:
command `0x0270`, sample count `0x0272`, accumulators `0x?c0`–`0x?c5` (`ii`,
`qq`, `iq`, three 32-bit hi/lo pairs per chain, read hi first).

1. **Loopback gain search**, `SAMCNT = 0x0400`. The index in
   `0x0734 + core*0x200` starts at 4 on 5 GHz and halves (4, 2, 1, 0) while
   `round(ii/1024) + round(qq/1024)` is above `0x169e`; it never rises. The
   lower edge `0xb57`, read at the start, has no observed effect.
2. **Measurement tones**, `SAMCNT = 0x4000`. Two tones up to 40 MHz, six at
   80 MHz: one 20-entry period sampled at a step of +1/−1, then +3/−3/+4/−4 at
   80 (`b43_phy_ac_tone_steps[]`), the step being the tone frequency in units of
   rate/20.
3. **Coefficients and teardown.** Two 10-bit signed coefficients per chain,
   `0x?a0` (`a`) and `0x?a1` (`b`), as on the N-PHY; then the compensation
   registers are restored and the tone engine stopped.

The writes to `0x0720`–`0x0729` after the first precision measurement are the
save/restore of the mute state of the chains not being measured, the AFE page
`switch_analog` also saves, not coefficients.

## The solve

The stock driver's RX IQ calibration on the AC-PHY is the frequency-dependent
one: the math starts `wlc_phy_cal_rx_fdiqi_acphy`, called by `wlc_phy_cals_acphy`. 
Per tone it runs `wlc_phy_rx_iq_est_acphy` and `wlc_phy_calc_iq_mismatch_acphy`; 
the latter and the main routine use `wlc_phy_nbits`, `wlc_phy_sqrt_int`, 
`wlc_phy_inv_cordic` and `wlc_phy_cordic`, the last two on one arctangent table 
in `.rodata`, the 18 entries of `lib/math/cordic.c`. `wlc_phy_cordic` is that 
function with the angle already in degrees Q16; `wlc_phy_inv_cordic` is the same 
rotation in vectoring mode on inputs scaled by 16, the sign test on `y >= 0`;
`wlc_phy_sqrt_int` rounds to nearest. The port has all three (`b43_phy_ac_cordic()`, 
`b43_phy_ac_inv_cordic()`, `b43_phy_ac_sqrt_near()`).

The mismatch of one tone (`b43_phy_ac_iq_mismatch()`), with `nb()` the bit
length and every division rounded half away from zero:

```
s        = 30 - max(nb(ii), nb(qq))
root     = sqrt_int(qq << s) * sqrt_int(ii << s)          ~ sqrt(ii*qq) * 2^s
sin_q16  = (-iq << (30 - nb(iq))) / (root >> (nb(iq) + 16 - max(nb(ii), nb(qq))))
cos_q16  = sqrt_int(2^30 - (sin_q16 >> 1)^2) << 1
angle    = inv_cordic(sin_q16, cos_q16)                     degrees Q16

d        = qq - ii;  e = nb(d) rounded up to even
mag      = sqrt_int(2^20 + (d << (30 - e)) / (ii >> (e - 10)))   = 2^10 sqrt(qq/ii)
```

The solve (`b43_phy_ac_iq_solve()`) averages angle and magnitude over the
tones, half away from zero, and puts the mean angle through the CORDIC:
`a = round(mag * sin / 2^16)`, `b = round(mag * cos / 2^16) - 2^10`.

Two things in that arithmetic decided the last coefficients. The magnitude
is rounded to an integer per tone and averaged afterwards, which the plain
mean of the per-tone `(a, b)` was missing; and the divisor `ii >> (e - 10)`
keeps only `nb(ii) - e + 10` bits, 11 to 13 here, so the ratio carries an
upward bias of up to a few hundredths of a unit that depends on the low
bits of `ii` and on the parity of `nb(qq - ii)`. On 316 points
(`reverse-tools/rxiq_points.py`) the accumulator sum gives `a`/`b` exact on
268/249, the per-tone mean on 299/257, this on 316/316. The two tones at ±f
up to 40 MHz are symmetric, so a line fit of the angle over the tones and its
mean are the same thing at the centre; the slope goes to the eleven-tap bank.

## Table `0x0c` and the TX LO LUTs

Table `0x0c` (IQLOCAL) holds the TX IQ `{a, b}` at `0x60 + 4*core` and the LO
leakage word, two signed bytes, at `0x62 + 4*core`; `rxiqcal_finalize()` reads
`0x60`–`0x66` back and rewrites them, as for the LO DAC registers
`0x?002`–`0x?005`.

Tables `0x42`/`0x62`/`0x82` are the LO leakage LUTs of TX power control, one
entry per power index. The first pass fills all 128 indices with the core's
fresh LO word plus a per-core base from `afe_res[1]`, `[3]`, `[5]`; the second
writes the bare base. The base depends on the pa5g sub-band and each byte of the
word is subtracted separately.

## Not covered by the data

- The scale of `B43_PHY_AC_MIN_RXIQ_PWR`: the captured powers are orders of
  magnitude above it, so the give-up path fed by `0x06a0`/`0x06a1` never runs.
- Rounding at an exact 0.5, absent from the points.
- The paths of the mismatch for accumulators of 31 bits or more, for
  `nb(qq - ii) < 11` and for a zero divisor: transcribed, never exercised.
- What the stock driver does if the gain search is still above the window at
  index 0.
