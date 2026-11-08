# Tempsense: from raw samples to a temperature (BCM4360, BCM4352)

The port replays the stock measurement and converts it on the fit below
(`b43_phy_ac_temp_update()`); the watchdog runs the calibrations again when
the temperature has moved by `phycal_tempdelta` since the last ones
(`b43_phy_ac_temp_recal()`), and throttles TX on heat (below). This is the
material to refine the fit.

## Defaults of the SROM fields

From the D6220's `wl.o` (7.14, MIPS, not stripped), for an AC-PHY:

- `phycal_tempdelta` (`wlc_phy_read_tempdelta_settings()`): 0 stays 0, no
  temperature-driven calibration; above 64, 255 included, it becomes 40;
  otherwise the SROM's value. The 40 matches what the DSL-3580L's
  `wl phycal_tempdelta` reports for its 255.
- `tempthresh` (`wlc_phy_attach()`): 1 to 254 as programmed, 0 and 255 give
  150 on the HT and AC PHYs (115 on the others). On a BCM4360 of boardtype
  273 or 279 (0x111, 0x117; the MacBookAir6,1's SROM has 0x117), 120
  whatever the SROM says; the tg789vac programs 120 itself.
- `temps_hysteresis`: 0 and 15 give 5. The attach keeps
  `tempthresh - temps_hysteresis` beside them and the full chain mask, which
  `wlc_phy_stf_chain_temp_throttle_acphy()` and
  `wlc_phy_stf_chain_active_get()` use: at or above the threshold the TX
  chain mask becomes the lowest chain of the hardware's (a table
  `{1, 1, 2, 1, 4, 1, 2, 1}`), RX keeping all of them, and at or below
  `tempthresh - temps_hysteresis` the configured mask comes back. The port
  does the same after each watchdog reading (`b43_phy_ac_temp_throttle()`),
  with 120 at most on every BCM4360, its readings being a fit, and sends
  every MCS on one stream meanwhile.

## What the port collects

`b43_phy_ac_tempsense()` programs the RX gain block `0x?720`–`0x?73e` and the
radio, and `b43_phy_ac_tempsense_chain()` reads PHY `0x0013` eight times in
each of four steps of radio `0x000e` bits 1 and 2, on every wired chain, into
`tempsense_samples[core][step][i]`; then it restores the 14 PHY and 7 radio
registers. It runs, with the MAC suspended, at the bss-up before the full
calibration, in the channel calibration on `up`, and in the watchdog every
`temps_period` turns.

| step | bit 1 | bit 2 |
|---|---|---|
| 0 | set | clear |
| 1 | clear | clear |
| 2 | set | set |
| 3 | clear | set |

The samples are 12-bit two's complement. Read as signed, the thermal signal is

    diff = ((mean(step1) - mean(step0)) + (mean(step3) - mean(step2))) / 2

one code per chain, near −1250 cold and −900 warm. Read unsigned it jumps by
hundreds of counts as soon as a bit-1-set sample dips below zero, which the
warm session does from the third minute on: use the signed reading.

The agcombo NVRAM leaves every tempsense field unprogrammed, so the stock
driver runs on the AC-PHY built-in defaults and the conversion must too.

## Anchor points

| capture | state | diff, cores 0/1/2 | average | `wl phy_tempsense` |
|---|---|---|---|---|
| `bss-up.zip!bss-up-ch100-bw80.txt` | cold, bss-up ch100/80 | −1255 / −1228 / −1253 | −1245 | 39–40 |
| `up-nobss-ioctl.zip!iotctl.txt`, t=1272.7 | warm | −953.8 / −907.6 / −947.4 | −936.2 | 54 |
| same, t=1327.3 | warm | −906.9 / −889.5 / −919.0 | −905.1 | 56 |

A `phy_tempsense` GET is not recorded, but each call runs its own tempsense
block off the watchdog's ten-turn grid, which pairs it with the printed value.

On the chain average these three gave `0.0485 * diff + 100` (39.6, 54.6 and
56.1), which the port used first. The warm pair alone is 2 degrees over 31
counts and pins nothing, and the cold point turned out to mislead: see the
next section.

## A ramp on a BCM4352 (RT-AC56U, wl 6.30.163)

An RT-AC56U under DD-WRT r66477, with `wl_mmio_trap` on the BCM4352 (eth2,
two chains). Each `wl -i eth2 phy_tempsense` call is bracketed by its own
trace mark, so every printed value is paired with the samples of that very
call, three calls a second apart per point:

- start: both radios up, idle;
- cooling: both radios down for 5 minutes, `wl up`, three calls, `wl down`,
  ten times;
- heating: both radios up and two CPU spinners, three calls every 5 minutes,
  ten times.

That covered 42 to 53 deg C. On the chain average, `round(diff / 15) + 117`
reproduces 58 of the 63 printed values and is within one degree of the
other five; a least-squares fit gives 0.0666 a count and 117.0 (rms 0.30,
which is the integer rounding), per chain 0.0660 / 116.6 and
0.0672 / 117.3.

| point | diff, chains 0/1 | average | `wl phy_tempsense` | `diff / 15 + 117` |
|---|---|---|---|---|
| start-1 | -1011.0 / -1006.0 | -1008.5 | 50 | 49.8 |
| c50-1 (radios down 5 min, first call after `up`) | -1127.2 / -1114.0 | -1120.6 | 42 | 42.3 |
| c50-3 (two seconds later) | -1086.8 / -1080.0 | -1083.4 | 45 | 44.8 |
| h10-1 | -988.5 / -982.0 | -985.2 | 51 | 51.3 |
| h50-3 (radios up, CPU loaded, 50 min) | -965.5 / -962.0 | -963.8 | 53 | 52.8 |

The cooling rows show why the agcombo's cold point does not fit: after
`wl up` the die warms by about 40 counts (2-3 degrees) in two seconds, and
the stock driver follows it call by call, with no smoothing. A bss-up
reading paired with a value printed at another moment can be degrees off.
The agcombo's two warm points, each from its own call, land within a degree
of the BCM4352 line (54.6 for 54, 56.7 for 56), the cold one does not (34.0
for 39-40).

The port converts with `DIV_ROUND_CLOSEST(sum, 16 * 15 * n) + 117`.

## To close it

- pairs outside 42-56 deg C (a heat gun or a cold chamber), since the
  thresholds at 120-150 deg C are far out on the line;
- a capture where the stock driver recalibrates on a temperature change, for
  its trigger and for what it runs then (the port runs the bss-up's block);
- the AC-PHY default slope and offset from the blob's `.rodata`, to check the
  fit against;
- whether b43 averages the chains or keeps one reading per core, since they
  differ by about 30 counts at the same state.
