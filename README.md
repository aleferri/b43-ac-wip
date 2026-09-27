# b43 AC-PHY / radio 2069 — BCM4352-family bring-up

Support for AC-PHY rev 1 with radio 2069 rev 4 in the Linux `b43` driver,
reverse-engineered from traces of Broadcom's stock `wl` driver. The PHY code
dispatches on the chip, `{0x4352, 0x4360}`.

The traces come from four boards, each with a distinct role:

- **Netgear D6220** (BCM4352, 2×2, `wl` 7.14.89.14) is the reference. Every gate
  runs against its cold and hot sweeps.
- **D-Link DSL-3580L** (BCM4352, 2×2, 5 GHz only, `wl` 6.30.102.7) is the board
  the port is meant to run on. Its `wl` is older, so some phases differ from the
  reference; it is never used as an oracle.
- **agcombo** (BCM4360, 3×3 dual-band, `wl` 7.14.43.21) separates chip
  differences from driver-version differences.
- **Technicolor TG789vac v2** (BCM4360, 3×3, `wl` 7.14.89.14) runs the same `wl`
  as the D6220 on the other chip, which separates chip and ucode from driver
  version.

## Target chip

| Field | Value |
|---|---|
| PCI ID | `0x14e4:0x43b3` |
| chip / corerev | `0x4352 / 42 (0x2a)` |
| radio | 2069 rev 4, AC-PHY rev 1 |
| SROM | rev 11, `boardtype=0x668`, `femctrl=6`, `subband5gver=0x4` |
| antennas (DSL-3580L) | `aa5g=3`, `aa2g=0` → 5 GHz only; `txchain=rxchain=3` |

## Goal

The MVP is **every hardware block configured, nothing left out, and a beacon on
air on every 5 GHz band and channel.**

The work proceeds by comparing traces, and that method has no useful
intermediate target. Cutting out a subset ("associate on ch36 at 6 Mbit/s")
would need the whole sequence understood anyway, to know what can be dropped
without breaking the rest. So the criterion for implementing a block is not
"is it needed to associate" but "does the stock driver emit it". The AP phases
— beacon, probe-response template, SSID in shared memory — are in scope,
because the beacon on air is part of the goal.

Out of scope, and the next steps after the MVP: actual data TX/RX, and 2.4 GHz.

**The first cold bring-up comes first.** A bring-up on an interface that was
already up (the "hot" case) is not out of scope, only deferred. That gives three
working rules:

- nothing is removed: the `FIRST_BRINGUP` branches in `src/` and the
  `AC_FIRST_INIT` lever in the harness stay, because the hot step needs them;
- `op_switch_channel` does not refuse a second call;
- where hot would differ from cold, cold is implemented and the hot behaviour
  is recorded in `docs/retrace-todo.md`.

## Current state

Measured on commit `8b1d680`; `src/` is unchanged since `41dba7e`.

| gate | result |
|---|---|
| cold, `cold01` ch36 bw20 | **98.40%** (29578/30059): 2 wrong values, 269 missing, 208 extra |
| cold, all 43 segments | min 96.75%, median 98.68%, max 99.01% |
| hot, the three default `up` segments | ch36 96.01%, ch52 97.39%, ch104 97.27% |
| periodic watchdog tick | **`MATCH`**, position by position |
| integration (whole b43) | `probe: 0`, `start: 0`; 79.62% on the `cold01` gate window |
| SROM rev 11 extractor | 77/0 (DSL-3580L), 74/0 (D6220), 75/0 (agcombo) PASS/FAIL |

The cold number is the raw line of `cmp_skip.py`. Its denominator is the union
of both streams, so it reaches 100% only when the port emits exactly the stock
driver's operations: no fewer, no more, no different values. The procedure that
produces a quotable number is in [`test/unit/README.md`](test/unit/README.md).

**What dominates the residual.** Every one of the 43 segments carries 262–318
missing and 208–226 extra operations. That is the address-match/key-table
clearing block (`ADDRM.SET`/`AMT.WR` plus the row read/write):

- the stock driver emits it inside the channel setup;
- b43 emits it from `b43_wireless_core_init()` after the first channel switch;
- the harness mirrors b43's position, so the displaced block counts twice.

On `cold01` it is the first positional divergence, at `@11017`. The block and
its three open differences are in [`docs/retrace-todo.md`](docs/retrace-todo.md).

**The six lowest segments.** They are the weather-radar configurations: ch120,
ch124 and ch128 at 20 MHz, ch116 and ch124 at 40, ch116 at 80. Their vendor
capture ends before the channel availability check completes (≈16.9k operations
against 29–48k elsewhere), so they measure a partial attach.

**The integration suite.** It builds the whole of b43 with the port inside,
against real kernel headers, and runs it. Attach and bring-up complete. The
remaining gap is mostly core work the stock driver does and b43 does not (see
[`test/integration/README.md`](test/integration/README.md)).

**On hardware** there is no recent run. The state of the current tree on a real
board is unknown.

## What is ported

- **SROM rev 11** (`patches/0001`), with a userspace test harness in
  [`sprom-rev11/`](sprom-rev11/).
- **`op_init`**: BCMA only, chips `0x4352`/`0x4360` (anything else returns
  `-EOPNOTSUPP`). It covers the PLL check, core probe, frontend pre-init, PMU
  regctl, GPIO, `mode_init`, the table load in the stock driver's order,
  `init_regs` and the host-flag configuration.
- **`software_rfkill`**: `radio_2069_init`, power-on, `rccal` (LPF and DACBUF
  caps derived from it), `afe_lpf_stage`. Blocking the radio runs
  `b43_phy_ac_down()`.
- **`switch_analog`**: the AFE arm unit. The cold preamble is emitted once, on
  the first entry that has a channel.
- **`switch_channel`**: every row of the 2069 channel table (50 rows,
  5170–5825 MHz, keyed on the tuned centre frequency) at 20, 40 and 80 MHz.
  - 2.4 GHz returns `-EOPNOTSUPP`.
  - A radio other than 2069 rev 4, or a frequency with no row, returns `-ESRCH`.
  - Calling it again with the same channel and width is a no-op.
- **`channel_calibrate`** (a phyop added by `patches/0015`): the post-switch
  calibrations — RX IQ, AFE, TX IQ/LO, gain control. They are skipped while a
  radar-duty channel waits for its availability check.
- **TX power**: `recalc_txpower`/`adjust_txpower` build the per-rate table from
  the SROM (`src/ppr_ac.c`) and program the per-core target `0x0646`.
- **Periodic work**: `pwork_15sec` runs a watchdog turn; `pwork_60sec`
  re-evaluates the CRS minimum-power threshold.
- **Event entry points** — `b43_phy_ac_watchdog()`, `b43_phy_ac_radar_poll()`,
  `b43_phy_ac_bss_up()` — are what the stock driver runs after bring-up. The
  unit harness drives them from the capture's timestamps; on hardware they are
  not wired yet (see [`docs/driver-status.md`](docs/driver-status.md)).
- **Core b43 and bcma**: `patches/0003`, `0004` and `0007`–`0018`. The list is
  in [`docs/driver-status.md`](docs/driver-status.md).

## What is missing

| Block | State |
|---|---|
| 2.4 GHz | `op_switch_channel` returns `-EOPNOTSUPP` |
| CAC / radar / bss-up on hardware | Event entry points exist but are not wired. Nothing in the kernel sets `cac_pending`, b43 does not advertise radar detection, and the core does not yet define `b43_ac_cac_match_gate()` or `b43_ac_beacon_reload()` |
| Watchdog cadence | The stock driver ticks every 1 s; b43's `pwork_15sec` runs every 15 s |
| Probe-response offload | Deliberately off (b43 writes `PRMAXTIME=1`); declared in `SOLO_VENDOR` |
| LEDs above GPIO 3 | `patches/0016`–`0017`; exercised by `test/integration`, untested on hardware |
| Temperature | Raw tempsense samples are collected, but there is no conversion and no consumer (see [`docs/tempsense-wiring-evidence.md`](docs/tempsense-wiring-evidence.md)) |

### Open defects

- **`do_full_init` does not tell cold from hot on b43.** `b43_phy_exit()` sets
  it back to `true` on every `ifconfig down`. The comments in `src/phy_ac.c`
  that treat it as "cold attach only" are therefore wrong on the real path. The
  fix depends on the deferred hot work.
- **The third `PHY.MOD 0x02e4`.** On channels with radar duty the stock driver
  writes that field three times; the port writes it twice.

Note for readers of the table descriptor: `est_pwr_lut_core*` and
`papd_comp_rfpwr_tbl_core*` share id and offset **by construction**. The stock
driver writes est_pwr and then papd_comp_rfpwr to the same cells in the same
init sequence. It is not a bug, and removing either would diverge.

## Reproducing the numbers

```sh
cd test/unit
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
make
./gates.sh                                    # cold01
./gates.sh /tmp/cold/cold[0-9][0-9]-ch*.txt   # all 43

unzip -d /tmp/hot ../../router-data/d6220/hot-sweep.zip
./gates.sh --hot                              # three hot segments

AC_READ_ORACLE=../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > /tmp/p.out
python3 compare.py \
    ../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt /tmp/p.out
# MATCH
```

The periodic gate is the most sensitive regression detector in the repository;
run it after every change.

## Build and hardware test

Prerequisites:

- a kernel with the `patches/` series applied (`git am`);
- `BROKEN` removed from `B43_PHY_AC` in `drivers/net/wireless/broadcom/b43/Kconfig`
  (no patch does this);
- `CONFIG_B43_PHY_AC=y`;
- firmware in `/lib/firmware/b43/`;
- a 5 GHz AP on channel 36;
- a serial console or netconsole.

```sh
modprobe b43                  # dmesg must report 0x4352/0x43b3, sromrev=11
ip link set wlan1 up          # op_init + rfkill(unblocked) + switch_channel
iw wlan1 scan freq 5180
```

Build with `B43_DEBUG=y` for the per-phase `b43dbg` messages.

## Repository layout

```
src/            driver sources (the source of truth; target drivers/net/wireless/broadcom/b43/)
patches/        the kernel patch series; 0006 is regenerated from src/
test/unit/      userspace harness: the PHY alone against the captures
test/integration/  the whole of b43 with the port, against real kernel headers
docs/           technical notes
sprom-rev11/    SROM rev 11 draft patch and its test harness
reverse-tools/  trace pipeline, analysis and extraction scripts, on-device tracer
router-data/    captures and static dumps per board
scripts/        patch regeneration and helpers
```

Start from [`docs/driver-status.md`](docs/driver-status.md) for how the driver
is put together and [`docs/retrace-todo.md`](docs/retrace-todo.md) for what is
open.

## After the MVP

- Split `patches/0006` before submitting to `linux-wireless`; the planned split
  is in `docs/driver-status.md`.
- HT/VHT: the init tables already cover OFDM; the late PHY writes need an audit.
- Submit `sprom-rev11/`; the preconditions are in its README.
- Co-loading with the integrated N-PHY (`wl0`): both PHYs probe, the rest is
  testable only with a complete bring-up.
- Data TX/RX and 2.4 GHz.
