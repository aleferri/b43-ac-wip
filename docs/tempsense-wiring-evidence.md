# Tempsense: from raw samples to a temperature (agcombo, BCM4360)

The port replays the stock measurement and converts it on the fit below
(`b43_phy_ac_temp_update()`); the watchdog runs the calibrations again when
the temperature has moved by `phycal_tempdelta` since the last ones
(`b43_phy_ac_temp_recal()`). Nothing else consumes it yet. This is the
material to refine the fit.

## Defaults of the SROM fields

From the D6220's `wl.o` (7.14, MIPS, not stripped), for an AC-PHY:

- `phycal_tempdelta` (`wlc_phy_read_tempdelta_settings()`): 0 stays 0, no
  temperature-driven calibration; above 64, 255 included, it becomes 40;
  otherwise the SROM's value. The 40 matches what the DSL-3580L's
  `wl phycal_tempdelta` reports for its 255.
- `tempthresh` (`wlc_phy_attach()`): 1 to 254 as programmed, 0 and 255 give
  150 on the HT and AC PHYs (115 on the others). On a BCM4360 where a field
  of the shared info (offset 68, not identified) reads 273 or 279, 120
  whatever the SROM says; the tg789vac programs 120 itself.
- `temps_hysteresis`: 0 and 15 give 5. The attach keeps
  `tempthresh - temps_hysteresis` beside them and the full chain mask, which
  `wlc_phy_stf_chain_temp_throttle_acphy()` and
  `wlc_phy_stf_chain_active_get()` use to drop TX chains above the threshold
  and give them back below the lower one. The port does not throttle yet.

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

On the chain average this gives

    T[degC] ~= 0.0485 * diff + 100

(54.6 and 56.1 for the warm blocks, 39.6 for the cold one). The warm pair alone
is 2 degrees over 31 counts and pins nothing. Chain 0 alone gives the same
slope; chain 1 alone does not fit the warm pair, so the stock driver reads the
average or chain 0.

## To close it

- more (diff, `phy_tempsense`) pairs over a wider span;
- a capture where the stock driver recalibrates on a temperature change, for
  its trigger and for what it runs then (the port runs the bss-up's block);
- the AC-PHY default slope and offset from the blob's `.rodata`, to check the
  fit against;
- whether b43 averages the chains or keeps one reading per core, since they
  differ by about 30 counts at the same state.
