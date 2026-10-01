# b43 AC-PHY / radio 2069 — BCM4352-family bring-up

Support for AC-PHY rev 1 with radio 2069 rev 4 in the Linux `b43` driver,
reverse-engineered from traces of Broadcom's stock `wl` driver. The PHY code
dispatches on the chip, `{0x4352, 0x4360}`.

The traces come from four boards:

- **Netgear D6220** (BCM4352, 2×2, `wl` 7.14.89.14): the reference. Every gate
  runs against its sweeps.
- **D-Link DSL-3580L** (BCM4352, 2×2, 5 GHz only, `wl` 6.30.102.7): the board
  the port is meant to run on. Its `wl` is older, so it is never an oracle.
- **agcombo** (BCM4360, 3×3, `wl` 7.14.43.21): separates chip from driver
  version.
- **Technicolor TG789vac v2** (BCM4360, 3×3, `wl` 7.14.89.14): the D6220's `wl`
  on the other chip, which separates chip and ucode from driver version.

Two bus captures of the hybrid `wl` 6.30 on x86, the archer-t5e and a
MacBookAir6,1, are the only ones with 2.4 GHz channel sets.

## Target chip

| Field | Value |
|---|---|
| PCI ID | `0x14e4:0x43b3` |
| chip / corerev | `0x4352 / 42 (0x2a)` |
| radio | 2069 rev 4, AC-PHY rev 1 |
| SROM | rev 11, `boardtype=0x668`, `femctrl=6`, `subband5gver=0x4` |
| antennas (DSL-3580L) | `aa5g=3`, `aa2g=0` → 5 GHz only; `txchain=rxchain=3` |

## Goal

The MVP is **every hardware block configured and a beacon on air on every
5 GHz channel and width.** A block is implemented because the stock driver
emits it, not because association needs it, so the AP phases (beacon,
probe-response template, SSID in shared memory) are in scope. Data TX/RX and
2.4 GHz come after.

The cold bring-up comes first; a bring-up on an interface that was already up
(hot) is deferred, not dropped. The `FIRST_BRINGUP` branches in `b43/` and the
`AC_FIRST_INIT` lever of the harness stay, and where hot differs from cold the
hot behaviour goes in [`docs/retrace-todo.md`](docs/retrace-todo.md).
The TX power model -- locale caps, legacy and CCK rules, chain choice -- is in
[`docs/txpwr-target-derivation.md`](docs/txpwr-target-derivation.md).

## Current state

Measured on 2026-09-30 and 2026-10-01, see below.

| gate | result |
|---|---|
| unit, cold `cold01` ch36/20 | **99.89%** (29817/29849): 2 wrong values, 27 missing (26 of them core cells outside the PHY), 1 extra |
| unit, cold, all 43 segments | min 99.77% (ch36/80), median 99.91%, max 99.95% |
| unit, hot `up` ch36 / ch52 / ch104 | 98.45% / 99.00% / 98.93% |
| unit, cold agcombo `cold01` ch36/20 | 91.05% |
| unit, cold TG789vac v2 `cold01` ch36/20 | 99.91% |
| unit, cold TG789vac v2, all 43 segments | min 99.77% (ch149/40), median 99.91%, max 99.98% |
| unit, periodic watchdog tick | **`MATCH`** |
| integration, cold `cold01` | `probe: 0`, `start: 0`; 82.61% (28765/34819) |
| integration, agcombo ch36/80 at the bus | 67.53% (89699/132825), with the 561 interrupts and 283 received frames of the capture replayed |
| SROM rev 11 extractor | 77/82/83 PASS, 0 FAIL (DSL-3580L, D6220, agcombo) |

The agcombo unit row was not re-measured on this date; the integration rows
were, on 2026-10-01. The agcombo bus row measured 70.07% before the
interrupts were replayed: the union grew by the five DMA channels b43 reads
and acknowledges on every interrupt where the stock driver touches one. The
cold01 integration row measured 90.90% (29003/31905) while the whole cold
preamble ran in `b43_phy_init()`: the window opens at the first PHY read of
the attach AFE arm, which then came after b43's core init, so the core init
fell outside it. With the arm back in the attach it is inside, and its 2970
extra ops are the drop; 250 more vendor ops are matched. Since the key
table init left the PHY its 548 words per `up` are missing on both rows.

The number to quote is the `grezzo` line of `cmp_skip.py`, with its two
parameters; how to reproduce and read it is in
[`test/unit/README.md`](test/unit/README.md). The integration rows and the
cold `cold01` unit row are measured with `--min-block 8 --gap-tol 2` on
2026-10-01; the other unit rows were measured on 2026-09-30, before the two
parameters, with blocks of at least two and no tolerance, which on `cold01`
gives the same count. The integration rows measured 82.30% and 64.86% under
that rule, which counted every equal op of an aligned block at the bus,
however short the run around it between two wrong values. The six
weather-radar segments (ch120/124/128 at 20 MHz, ch116/124 at 40, ch116 at
80) end before their availability check completes and measure a partial attach.

There is no recent run on hardware.

## What is ported

- **SROM rev 11** (`bcma/drivers/bcma/sprom.c`, in `patches/0001`), with a
  test harness in [`bcma/harness/`](bcma/harness/).
- **The PHY** (the AC-PHY files of `b43/`, `patches/0003`): `op_init`,
  `software_rfkill`, `switch_analog`, `switch_channel` on every row of the
  2069 table (5170–5825 MHz) at 20, 40 and 80 MHz, the post-switch
  calibrations, TX power from the SROM, the watchdog on `pwork_1sec`, the CRS
  threshold on `pwork_60sec`, the radar poll and the bss-up.
- **The core** (the rest of `b43/`, `patches/0002`): channel set, DMA alignment,
  TX/RX path, address match table, shared memory, key layout, the
  `channel_calibrate` hook, LEDs above GPIO 3, the 1 s tick, the noise sample,
  the availability check and the radar work, the PHY bandwidth clock, the
  rev 42 MAC init, register read-backs and the AC template layout.
- **bcma/ssb** (`bcma/`, `patches/0001`): PMU init, the PCI ID,
  `ledbh4..15`, `boardflags3` and `AvVmid`.

How the pieces fit is in [`docs/driver-status.md`](docs/driver-status.md).

## What is missing

- 2.4 GHz: `switch_channel` returns `-EOPNOTSUPP`. The channel set is in the
  port and checked against two boards (`test/unit/band_gate.sh`, with the
  driver built with `ALLOW_24`); board data and calibrations are open
  (`docs/retrace-todo.md`, "2.4 GHz").
- Radar detection without pattern matching: `CONFIG_B43_DFS` reports every
  pulse as a radar.
- Probe-response offload and the power management queue: deliberately off, see
  `docs/retrace-todo.md`.
- Temperature: raw tempsense samples are collected and not converted
  ([`docs/tempsense-wiring-evidence.md`](docs/tempsense-wiring-evidence.md)).

## Build and hardware test

- a kernel with the `patches/` series applied (`git am`);
- `BROKEN` removed from `B43_PHY_AC` in `drivers/net/wireless/broadcom/b43/Kconfig`
  (no patch does this), and `CONFIG_B43_PHY_AC=y`;
- firmware in `/lib/firmware/b43/`, a 5 GHz AP on channel 36, a serial console
  or netconsole.

```sh
modprobe b43                  # dmesg must report 0x4352/0x43b3, sromrev=11
ip link set wlan1 up          # op_init + rfkill(unblocked) + switch_channel
iw wlan1 scan freq 5180
```

`B43_DEBUG=y` gives the per-phase `b43dbg` messages.

## Repository layout

```
b43/                 the b43 files the port changes or adds, whole
bcma/                the bcma, ssb and bcm47xx files it changes, at their kernel
                     paths, and the SROM rev 11 test harness
patches/             the three kernel patches, generated from b43/ and bcma/
test/unit/           the PHY alone against the captures
test/integration/    the whole of b43 with the port, against real kernel headers
docs/                technical notes
reverse-tools/       trace pipeline, analysis and extraction scripts
wl-diag/             on-device accessor tracer for the stock wl driver
wl-mmio-trap/        on-device MMIO trap for the stock wl driver
wl-cc-dump/          on-device ChipCommon PMU state dump
wl-capture-scripts/  device-side capture sweeps
router-data/         captures and static dumps per board
scripts/             patch regeneration and helpers
```

## After the MVP

- Split the core and AC-PHY patches into reviewable changes before submitting
  to `linux-wireless`.
- HT/VHT: the init tables cover OFDM; the late PHY writes need an audit.
- Submit the SROM rev 11 part of `bcma/`; the preconditions are in
  `bcma/README.md`.
- Co-loading with the integrated N-PHY (`wl0`).
