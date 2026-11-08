# b43 AC-PHY / radio 2069 — BCM4352 and BCM4360

Support for AC-PHY rev 1 with radio 2069 rev 4 in the Linux `b43` driver,
reverse-engineered from traces of Broadcom's stock `wl` driver. The PHY code
dispatches on the chip, `{0x4352, 0x4360}`.

The port runs as an access point on routers with MIPS and ARM SoCs, on both
bands, with aggregation and hardware CCMP (see "On hardware"):

- **D-Link DSL-3580L** (BCM6362, MIPS; BCM4352, 2×2, 5 GHz only), OpenWrt.
- **Linksys EA6900** (BCM4708, ARM; two BCM4360, 3×3, femctrl 3).
- **ASUS RT-AC66U B1** (BCM4708, ARM, RT-AC68U hardware; two BCM4360,
  femctrl 3).

and as a station on a **MacBookAir6,1** (BCM4360, x86).

The traces of the stock driver come from more boards, each with its role:

- **Netgear D6220** (BCM4352, 2×2, `wl` 7.14.89.14): the reference. Every gate
  runs against its sweeps.
- **D-Link DSL-3580L** (`wl` 6.30.102.7, ucode 784.2). Its `wl` is older, so
  it is never an oracle; its ucode is the one b43 loads (below).
- **agcombo** (BCM4360, 3×3, `wl` 7.14.43.21): separates chip from driver
  version.
- **Technicolor TG789vac v2** (BCM4360, 3×3, `wl` 7.14.89.14): the D6220's `wl`
  on the other chip, which separates chip and ucode from driver version.
- **Netgear R7000** and **ASUS RT-AC66U B1** (BCM4360, `wl` 7.14.164.18 under
  DD-WRT): both bands, femctrl 3 and the board data of the NVRAM-based
  Northstar routers; on the RT-AC66U B1 also beamforming and radar.
- **archer-t5e** and **MacBookAir6,1** (hybrid `wl` 6.30 on x86): bus
  captures, 2.4 GHz included.

What each board's data holds is in its `router-data/` directory.

## Chips

| Field | Value |
|---|---|
| chip / corerev | `0x4352` or `0x4360` / 42 (`0x2a`) |
| radio | 2069 rev 4, AC-PHY rev 1 |
| PCI ID | `0x14e4:0x43b3` for the BCM4352, added by `patches/0001`; the BCM4360's `0x43a0` is already in bcma |
| board data | SROM rev 11, or the same fields from NVRAM on boards without a SROM |
| FEM control | `femctrl` 6 sub 0, 2 sub 1, 1 sub 0, 3 sub 3 (2.4 GHz) and 3 sub 0 (5 GHz); see [`docs/driver-status.md`](docs/driver-status.md) |

## Approach

The first milestone, every hardware block configured and a beacon on air on
every 5 GHz channel and width, is behind: data TX/RX, aggregation, hardware
encryption and 2.4 GHz came after it, and the port carries traffic on the
boards above. A block is implemented because the stock driver emits it, not
because association needs it. The exceptions are deliberate and listed
under "What is missing": the probe-response offload (its template and SSID
in shared memory) and the power management queue.

The cold bring-up comes first; a bring-up on an interface that was already up
(hot) is deferred, not dropped. The `FIRST_BRINGUP` branches in `b43/` and the
`AC_FIRST_INIT` lever of the harness stay, and where hot differs from cold the
hot behaviour goes in [`docs/retrace-todo.md`](docs/retrace-todo.md).
The TX power model -- locale caps, legacy and CCK rules, chain choice -- is in
[`docs/txpwr-target-derivation.md`](docs/txpwr-target-derivation.md).

## Current state

Measured on 2026-11-03.

| gate | result |
|---|---|
| unit, cold `cold01` ch36/20 | **99.87%** (29818/29857): 1 wrong value, 26 missing (core cells outside the PHY), 11 extra |
| unit, cold, all 43 segments | min 99.77% (ch60/40 and the six weather-radar segments), median 99.87%, max 99.93% (ch132/80); 1 wrong value on 38 of 43 |
| unit, hot `up` ch36 / ch52 / ch104 | 98.50% / 99.00% / 98.94% |
| unit, cold agcombo `cold01` ch36/20 | 89.05% |
| unit, cold TG789vac v2 `cold01` ch36/20 | 99.90% |
| unit, cold TG789vac v2, all 43 segments | min 99.73% (ch149/40), median 99.90%, max 99.98%; 1 wrong value on 27 of 43 |
| unit, periodic watchdog tick | **`MATCH`** |
| integration, cold `cold01` | `probe: 0`, `start: 0`; 70.31% (26753/38050) |
| integration, agcombo ch36/80 at the bus | 70.57% (89993/127530), with the 561 interrupts and 283 received frames of the capture replayed |
| SROM rev 11 extractor | 77/82/83 PASS, 0 FAIL (DSL-3580L, D6220, agcombo; 2026-10-09) |
| SROM rev 11 from NVRAM | 116/123/123/123 PASS, 0 FAIL (DSL-3580L, D6220, agcombo, TG789vac v2; 2026-10-09) |

The wrong value and one extra op on the cold rows are one port decision:
HOSTF2 bit 0, which keeps the microcode from answering probe requests, set
on a first bring-up where the stock driver clears it (`docs/retrace-todo.md`,
"Probe-response offload"); the bit is masked from the cell's value, not from
the host-flag record or the write it adds. The other ten extra ops are the
`0x034d` ladder, which no D6220 capture writes (`docs/retrace-todo.md`, "The
`0x034d` ladder"). The agcombo unit row runs with
the capture's SSID length (7, read off
`PRSSIDLEN` by `gates.sh`). The integration rows write the 48-bit table
cells as the stock driver does on the bus, which a `wl-diag` capture, taken
at the accessors, does not show: that costs `cold01` against its capture
and is right on the bus. On the
agcombo bus row the replayed interrupts count: b43 reads and acknowledges
the five DMA channels on every interrupt where the stock driver touches one.
Since the key table init left the PHY its 548 words per `up` are missing on
both integration rows.

The number to quote is the `grezzo` line of `cmp_skip.py`, with its two
parameters, `--min-block 8 --gap-tol 2`; how to reproduce and read it is in
[`test/unit/README.md`](test/unit/README.md) and
[`test/integration/README.md`](test/integration/README.md). The six
weather-radar segments (ch120/124/128 at 20 MHz, ch116/124 at 40, ch116 at
80) end before their availability check completes and measure a partial
attach.

## On hardware

Throughput is TCP, in Mbit/s, as reported where it was measured, which
says what else was running and how.

| board | band, width | client | down | up | source |
|---|---|---|---|---|---|
| EA6900 | 5 GHz VHT80 | two clients, 16 h soak, average | 362 | 379 | `e31e2e2` |
| EA6900 | 5 GHz VHT80 | ath11k | | 381-386 | `5774865` |
| EA6900 | 2.4 GHz HT20 | two clients, 16 h soak, average | 143 | 157 | `e31e2e2` |
| RT-AC66U B1 | 5 GHz ch36/80 | OnePlus Nord 2, next to the router | 213-243 | | `c9ad2cc` |
| RT-AC66U B1 | 5 GHz ch36/80 | OnePlus Nord 2, two rooms away | 191-203 | 112-134 | `f1e5eee` |
| RT-AC66U B1 | 5 GHz ch36/80 | rtl8821ae (1×1), close range | 168 | 166-169 | `5950ab5` |
| RT-AC66U B1 | 2.4 GHz ch6/40 | a 1×1 client | 91 | 60 | `ff49890` |
| DSL-3580L | 5 GHz VHT80 | one-stream phone, MCS 9 | 69 | 55 | `docs/retrace-todo.md` |

### Linksys EA6900 and ASUS RT-AC66U B1 (Northstar, ARM)

Both are a BCM4708 with two BCM4360, one per band, and their board data in
NVRAM. Both bands carry traffic with WPA2-CCMP in hardware, 5 GHz up to
VHT80, 2.4 GHz at 20 MHz and, on the RT-AC66U B1, at 40. On the EA6900 the
build with the calibration, femctrl 3 and DMA changes passed a 16 h soak
with two clients on both bands, with no b43 error and no CCMP replay. The RT-AC66U B1 runs
two APs on one radio at the throughput of one (128/158 Mbit/s up/down on
each, `12028df`).
At 5 GHz HT40 a two-stream HT station gets MCS 15 at 40 MHz, with the
primary in either half (ch36 HT40+, ch40 HT40-): 198-204 Mbit/s down
against 99 at HT20, with all of minstrel's attempts at 40 MHz (84-100 %
  at MCS 15). VHT80
with the primary on ch40 runs as on ch36 (281 Mbit/s down).

What brought the throughput there, each change with its profile or its
measurement in its commit: the TX and
RX DMA burst and prefetch of bgmac (`ab26a16`, `5774865`), the received
frames and the TX statuses handed to mac80211 outside `wl->mutex` (`b91542b`,
`72a9a3f`), the RX sync limited to the received bytes (`f38bf54`), the
2.4 GHz calibration radio setup (`b3cbed4`), the `0x034d` ladder at 80 MHz
(`82420f5`), LDPC and A-MSDU reception (`4625a8b`, `f1e5eee`), the TX path
pulling from the mac80211 queues (`496b540`), RX buffers from a page pool
(`5950ab5`) and AQL (`c9ad2cc`).

### D-Link DSL-3580L (BCM6362, MIPS)

The DSL-3580L runs the port under OpenWrt with `ucode42.fw` 784.2 from
`broadcom-wl-6.30.163.46`, byte for byte the `d11ucode42` of the board's own
`wl`, as an AP on channel 36.

- **20 MHz without HT, HT20, HT40, VHT20, VHT40 and VHT80 work**: the
  beacon goes out, stations find the AP on passive and active scans,
  associate, complete the WPA2 4-way handshake and carry traffic
  ([`bringup-log-2026-10-11-ht20-40.txt`](bringup-log-2026-10-11-ht20-40.txt)
  for HT20 on 36 and HT40 on 36/40). A one-stream VHT phone runs at VHT80
  MCS 9.
- **Throughput**: the 69 Mbit/s down and 55 up of the table were measured
  with CCMP in hardware, without aggregation and before TX block ack
  sessions were taken, through the board's Ethernet, which frees its TX
  descriptors only from the RX poll (`docs/retrace-todo.md`, "On
  hardware"). No later measurement is in this tree.
- **RX**: with A-MPDUs from the phone, 128 slots underrun; the 816-02 sets
  192. See `docs/retrace-todo.md`, "On hardware".

### MacBookAir6,1 (x86)

The BCM4360 of a MacBookAir6,1 runs the port as a station: it scans,
associates and passes traffic on 2.4 GHz (`docs/retrace-todo.md`, "2.4
GHz").

### Log

The log of a normal build has one `set_channel` line per channel switch and
the warnings: TX errors (`PHY transmission error`, a frame dropped for a
rate the header cannot carry, a suppressed TX status), RX descriptor
underruns, unknown cookies. The bring-up steps, the `AC-PHY:` section
markers and the PHY and radio values are debug: they need
`CONFIG_PACKAGE_B43_DEBUG` and `verbose=3`, the markers never rate-limited.
After `B43_STAT_STARTED` every other `b43dbg`/`b43info`/`b43warn`/`b43err`
goes through `net_ratelimit()`, ten per five seconds; the cause of a
controller restart and its `Controller RESET` line go through
`b43err_restart()`, which is not. `sysctl -w net.core.message_cost=0` turns
the limit off.

## What is ported

- **SROM rev 11** (`bcma/drivers/bcma/sprom.c`, in `patches/0001`), and the
  same fields from NVRAM for a board without a SROM, such as the Northstar
  routers (`bcma/drivers/firmware/broadcom/bcm47xx_sprom.c`), with a test
  harness for both in [`bcma/harness/`](bcma/harness/).
- **The PHY** (the AC-PHY files of `b43/`, `patches/0003`): `op_init`,
  `software_rfkill`, `switch_analog`, `switch_channel` on every row of the
  2069 table, 5170–5825 MHz at 20, 40 and 80 MHz and 2412–2484 MHz at 20 and
  40, the post-switch calibrations, TX power from the SROM, the watchdog on
  `pwork_1sec`, the CRS threshold on `pwork_60sec`, the radar poll and the
  bss-up.
- **The core** (the rest of `b43/`, `patches/0002`): channel set, DMA ring
  addressing and burst settings, TX/RX path, address match table, shared
  memory, key layout, the `channel_calibrate` hook, LEDs above GPIO 3, the
  1 s tick, the noise sample, the availability check and the radar work, the
  PHY bandwidth clock, the rev 42 MAC init, register read-backs and the
  template layout of each AC ucode. On the AC cores also: up to four APs on
  one radio, TX pulled from the mac80211 queues with AQL, A-MSDU on TX (of
  full-size MSDUs) and on RX, LDPC reception, RX buffers from a page pool,
  and received frames and TX statuses handed to mac80211 outside
  `wl->mutex`.
- **bcma/ssb** (`bcma/`, `patches/0001`): PMU init, the PCI ID,
  `ledbh4..15`, `boardflags3`, `AvVmid` and `pdoffset2g40ma`.

How the pieces fit is in [`docs/driver-status.md`](docs/driver-status.md).

## What is missing

- Part of the 2.4 GHz board data and calibrations: 20 MHz is checked against
  two boards (`test/unit/band_gate.sh`), on the MacBookAir6,1 and against
  the R7000's and the RT-AC66U B1's captures (`docs/retrace-todo.md`, "2.4
  GHz"); the 40 MHz values come from read-backs under `wl` on the
  RT-AC66U B1.
- Radar detection on other boards: `CONFIG_B43_DFS` arms the detector as
  the stock driver does and matches the pulses against the types of the
  regulatory domain's DFS region, ETSI EN 301 893 (1-6) or FCC KDB 905462
  (0-6) (`b43/radar_ac.c`), at 20, 40 and 80 MHz, tested with an SDR on
  the RT-AC66U B1 only. FCC 5 is matched by its burst count alone, and a
  carrier that fills the FIFOs hides a burst (`docs/retrace-todo.md`,
  "Radar pulses").
- Probe-response offload and the power management queue: deliberately off, see
  `docs/retrace-todo.md`.
- A-MSDUs inside A-MPDUs on RX: HT MCS go
  out to an HT station and VHT MCS 0-9 to a VHT one, on up to the board's
  TX chains, laid out as the stock drivers lay them out, A-MPDUs in both
  directions, and A-MSDUs on TX from their `frag_list`. A-MSDUs inside
  A-MPDUs are not announced: joining them costs more CPU than they save on
  the Northstar boards. SU beamforming is opt-in (`txbf=1`): it starts with
  the station's pairwise key, or on an open BSS when the station is
  authorized, as the stock driver does (`docs/retrace-todo.md`, "HT and
  VHT").
- Hardware encryption: CCMP and WEP keys go to the hardware, those of
  every AP included; TKIP stays with mac80211, and so do protected
  management frames on TX (`docs/retrace-todo.md`, "Hardware encryption");
  `nohwcrypt=1` leaves all of it to mac80211.
- Temperature: converted on a three-point fit from one board; it runs the
  calibrations again after a move of `phycal_tempdelta` (40 deg C above 64,
  so when unprogrammed) and drops TX to one chain at `tempthresh` (150 deg C
  unprogrammed, 120 at most on a BCM4360) until it falls by
  `temps_hysteresis` (5)
  ([`docs/tempsense-wiring-evidence.md`](docs/tempsense-wiring-evidence.md)).

## Build and hardware test

- a kernel with the `patches/` series applied (`git am`) and
  `CONFIG_B43_PHY_AC=y`; on OpenWrt the 816 patches in
  `package/kernel/mac80211/patches/brcm/` and `PACKAGE_B43_PHY_AC` in
  `broadcom.mk`, with the board selecting `DRIVER_11AC_SUPPORT`, without
  which hostapd is built without 802.11ac and rejects `ieee80211ac` and the
  `vht_*` options;
- firmware in `/lib/firmware/b43/`, a 5 GHz AP on channel 36, a serial console
  or netconsole.

```sh
modprobe b43                  # dmesg reports the chip, 0x4352 or 0x4360
ip link set wlan1 up          # op_init + rfkill(unblocked) + switch_channel
iw wlan1 scan freq 5180
```

`B43_DEBUG=y` gives the per-phase `b43dbg` messages.

## Repository layout

```
b43/                 the b43 files the port changes or adds, whole
bcma/                the bcma, ssb and bcm47xx files it changes, at their kernel
                     paths, and the SROM rev 11 test harness
patches/             the three kernel patches on v7.2, the OpenWrt ones (816-*
                     on backports 7.2, 880-* on OpenWrt's kernel) and the trees
                     they were generated from; see docs/driver-status.md
test/unit/           the PHY alone against the captures
test/d11sim/         the ucode run on the b43-tools interpreter
test/integration/    the whole of b43 with the port, against real kernel headers
docs/                technical notes; northstar-guide.md, captures on the
                     ARM routers
docs/ucode42/        the corerev 42 microcode: maps, subsystems, interpreter
                     model, timetable (index in 00-INDEX.md)
reverse-tools/       trace pipeline, analysis and extraction scripts
wl-diag/             on-device accessor tracer for the stock wl driver, MIPS
                     and ARM
wl-mmio-trap/        on-device MMIO trap for the stock wl driver
wl-cc-dump/          on-device ChipCommon PMU state dump
wl-capture-scripts/  device-side capture sweeps
router-data/         captures and static dumps per board
scripts/             patch regeneration and helpers
bringup-log-*.txt    bring-up logs from the DSL-3580L, by date
```

## Next

- Split the core and AC-PHY patches into reviewable changes before submitting
  to `linux-wireless`.
- HT/VHT: the init tables cover OFDM; the late PHY writes need an audit.
- Transmit beamforming, from the RT-AC66U B1's `mmio-txbf-5g36-80.zip`.
- Radar pattern matching, from the RT-AC66U B1's `radar-fifo.zip`.
- Submit the SROM rev 11 part of `bcma/`; the preconditions are in
  `bcma/README.md`.
- Co-loading with the integrated N-PHY (`wl0`).
