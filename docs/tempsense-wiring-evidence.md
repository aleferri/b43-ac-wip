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

The thermal signal is the difference between the bit-1-clear and bit-1-set
steps. Taking `diff = ((mean(step1) - mean(step0)) + (mean(step3) - mean(step2))) / 2`
gives one differential code per chain. The bit-1-set steps sit near ~350 counts
and the bit-1-clear steps near ~3200, so the differential lands near ~2850-3050
and moves with temperature.

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

Two thermal states, each with the raw `0x0013` block and the vendor's decoded
`phy_tempsense` from the same session.

Cold, just powered on at ambient — bss-up on ch100/80, from `bss-up.zip`:

    raw diff per core (0/1/2): 2841 / 2868 / 2843   (avg 2851)
    vendor phy_tempsense:      ~39-40

Warm, chip already exercised — US session on ch36/80, from an earlier
`up-nobss` capture that is not in the repository (clean tempsense block found
mid-trace):

    raw diff per core (0/1/2): 3005 / 3048 / 3020   (avg 3024)
    vendor phy_tempsense:      ~54

Per-core the warm-minus-cold rise is 164 / 180 / 177 counts, i.e. consistent
across chains. Both the raw differential and the decoded output rise with
temperature (positive slope).

## Two-point estimate (rough, to be refined)

From the averages, cold (2851, 40) and warm (3024, 54):

    slope  ~= 0.081 degC per raw count   (~= 12.4 counts per degC)
    T[degC] ~= 0.081 * raw_diff - 190

This is a two-point fit and must be treated as a starting point only: the cold
decoded value is a range (see the series below), the warm decoded value's pairing
to the exact block is approximate, and there is per-core spread in the raw. It
is enough to bring the temperature up in b43 and sanity-check the sign and scale,
not to trust to a degree.

## Decoded series (vendor phy_tempsense)

Cold, repeated reads just after power-on at ambient (climbing as even the idle
chip warms; the leading 43 looks like a stale first read):

    43 39 40 40 41 41 41 41 42 43 44 44

Warm, US session: 54 at 36/80, 56 at 100/80.

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
    router-data/agcombo/stats.txt                           decoded phy_tempsense
