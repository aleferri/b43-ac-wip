# Tempsense wiring evidence (agcombo, BCM4360, AC-PHY rev 1)

Everything gathered so far to turn the raw AC-PHY tempsense the port already
collects into a usable temperature, and to feed it wherever b43 needs it. The
port currently stops at the raw samples; this is the material to close the gap.

## What the port does today

`b43_phy_ac_tempsense()` replays the vendor's measurement — RX gain block and
radio put into a measurement configuration, the block restored afterwards — and
`b43_phy_ac_tempsense_chain()` reads the raw ADC from PHY `0x0013`, eight
samples per step, four steps, on every wired chain, into
`tempsense_samples[core][step][i]`. Nothing consumes those samples: there is no
raw->degree conversion and no compensation driven by them yet. So the number a
user reads from wl's `phy_tempsense` iovar is the vendor's decoded output, not
anything the port computes.

Three sites run the measurement, all with the MAC suspended: the bss-up
(ahead of the full calibration), the on-`up` channel calibration, and the
watchdog every `temps_period` turns.

## Raw structure

Per chain, four steps toggle radio `0x000e` bits 1 and 2 (the `step[]` table in
`b43_phy_ac_tempsense_chain`), eight `0x0013` reads each:

    step 0: bit1 set,   bit2 clear
    step 1: bit1 clear, bit2 clear
    step 2: bit1 set,   bit2 set
    step 3: bit1 clear, bit2 set

The samples are 12-bit two's complement. The bit-1-set steps sit just above
zero and, as the chip warms, dip below it (`0x0ff9`, `0x0ff1`); the
bit-1-clear steps sit near `0x0c70`, which is negative. Read as signed, the
thermal signal is

    diff = ((mean(step1) - mean(step0)) + (mean(step3) - mean(step2))) / 2

one code per chain, near -1250 cold and -900 warm, rising with temperature.
Read as unsigned 12-bit the same difference comes out 4096 higher (2851 is
-1245) as long as no bit-1-set sample goes negative; once one does, the
unsigned mean jumps by hundreds of counts. In the up-nobss session below that
happens on chains 0 and 2 from the third minute on, so the signed reading is
the one to use.

## No SROM calibration on this board

The agcombo NVRAM (`router-data/agcombo/wl1_nvram.txt`) carries the tempsense fields all unprogrammed (every bit set),
so there is nothing to read from SROM and the vendor falls back to the AC-PHY
firmware defaults:

    rawtempsense=0x1ff  tempsense_slope=0xff  tempoffset=255
    tempthresh=255      tempcorrx=0x3f        phycal_tempdelta=255
    tempsense_option=0x3 temps_hysteresis=15  temps_period=15

Consequence for wiring: on this board the raw->degree conversion must use the
AC-PHY built-in defaults, not SROM constants. The empirical fit below should be
cross-checked against those defaults when they are pulled from the driver.

## Matched anchor points (raw differential vs vendor decoded output)

Cold, just powered on at ambient -- bss-up on ch100/80, from `bss-up.zip`:

    diff per core (0/1/2): -1255 / -1228 / -1253   (avg -1245)
    vendor phy_tempsense:  ~39-40

Warm, US session with the chip up and no BSS (`up-nobss-ioctl.zip`, the
capture with the `wlc_ioctl` hook): ch36/80 then ch100/80, two
`wl phy_tempsense` calls. A GET is not recorded, but each call runs its own
tempsense block off the watchdog's ten-turn grid, which pairs it with the
printed value:

    t=1272.7  diff -953.8 / -907.6 / -947.4   (avg -936.2)   printed 54
    t=1327.3  diff -906.9 / -889.5 / -919.0   (avg -905.1)   printed 56

An earlier version of this page paired 54 with the first watchdog block of
that session (t=1219.3, avg -1071.6, 3024 read unsigned), 53 s and some 4
degrees before the call.

## Three-point estimate

The printed values are integers, so each point is a 1-degree interval, and
the two warm points are 2 degrees apart: they fix the slope only with the cold
point. On the chain average:

    cold -> 54:  14.5 degC over 309 counts   0.047 degC per count
    cold -> 56:  16.5 degC over 340 counts   0.049 degC per count
    54 -> 56:    2 (1..3) degC over 31 counts, 0.032..0.096 degC per count

    T[degC] ~= 0.0485 * diff + 100

which gives 54.6 and 56.1 for the two warm blocks and 39.6 for the cold one.
Chain 0 alone gives the same slope (0.048, and 0.043 for the warm pair);
chain 1 alone does not fit the warm pair (0.11 against 0.045), so the vendor
reads the average or chain 0, not chain 1. More points over a wider span
decide it.

## To finalize the wiring

- More matched (raw_diff, phy_tempsense) pairs across a wider temperature span,
  each a `0x0013` block and a `phy_tempsense` read taken together, to pin the
  slope beyond two points.
- The AC-PHY default tempsense slope/offset built into the stock driver (read
  as data from `.rodata`; brcmsmac has no AC-PHY), to compare against the
  empirical fit, since this board provides no SROM calibration.
- The per-core offset handling: chains differ by ~30 raw counts at the same
  state (see the anchors), so decide whether b43 averages the chains or keeps a
  per-core reading, as the vendor does.

## Capture references

    router-data/agcombo/bss-up.zip!bss-up-ch100-bw80.txt   cold block, ch100/80
    router-data/agcombo/up-nobss-ioctl.zip!iotctl.txt       warm blocks, 54 and 56
    router-data/agcombo/stats.txt                           decoded phy_tempsense
