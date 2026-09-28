# Open items

What still separates the port from the stock driver, one item per entry, with
the evidence and the next step. Closed investigations are not kept here: their
conclusions live in the code, next to what they justify.

The items that are core work (`main.c`, `xmit.c`, the `patches/` series)
cannot be verified by `test/unit`, which compiles `src/` only. They need
`test/integration`.

## Core: order and content of shared-memory work

### Key-table clearing (`ADDRM.SET`)

This is the largest residual on every cold segment: 262–318 missing and
208–226 extra operations each, and on `cold01` the first positional divergence
(`@11017`).

For each row `0x00`–`0x37` the stock driver emits:

```
ADDRM.SET idx=0x0000
AMT.WR    idx=0x0000
OBJ.BULKR addr=0x0000 len=8 a5=0x00040000
OBJ.BULKW addr=0x0000 len=8 a5=0x00040000
```

b43 emits the same work from `b43_wireless_core_init()` after the first
channel switch:

- `b43_upload_card_macaddress()` writes the two top rows, `0x3e` with `0x8002`
  and `0x3f` with `0x8008`;
- `b43_security_init()` clears the key rows.

The unit harness emits them at b43's position (`emit_core_init_tail()` in
`test/unit/main.c`); `src/` does not call them. Three differences remain, all
core-side:

- **Order.** The stock driver clears the keys between `OBJ.WR 0x0658..0x0666`
  and `MAC.MHF 0x0000 mask=0x4000`, inside the channel setup. It also writes
  the two top rows twice in the attach. With the block at the stock driver's
  position the `cold01` gate reads 99.89%; at b43's position it reads 98.40%.
- **Row count.** The stock driver clears 56 rows (`0x00`–`0x37`); b43 clears
  50, `B43_NR_PAIRWISE_KEYS`, because the new kidx API starts at slot 4. The
  wide layout has 64 rows, so b43's limit is not the table's.
- **The first pair.** The stock driver writes the station row with `0x8008`
  and the BSSID row without flags first; b43 does not emit that pair.

On the integration suite the key material (`0x10f4`) and the index block
(`0x05e0`) appear twice:

- `src/` emits them as `shm_zero_10f4`/`shm_zero_05e0`;
- `b43_security_init()` writes them again. The core writes the index block as
  `(kidx << 4) | algo` over 54 words, where the stock driver writes zeros over
  68.

Removing them from `src/` costs 548 aligned operations on `cold01` in the
integration suite (75.28% → 73.65% on the documented window).

### `b43_amt_write()` writes a row, not two words

The capture reads and rewrites the whole 8-byte row (`OBJ.BULKR`/`OBJ.BULKW`,
RCMTA routing `a5=0x00040000`, `addr = idx * 8`) on **every** row, including
rows being cleared. `patches/0011` writes two `b43_shm_write32()` and reads the
high word back only for `B43_AMT_KEEP_FLAGS`.

`b43_test_emit_amt()` in the harness emits the stock form, so harness and patch
disagree until the patch follows. Next step: bring `b43_amt_write()` to the
capture's form, which also absorbs the `KEEP_FLAGS` branch.

### Host-flag order

The stock driver never reads the five HOSTF cells. It keeps a shadow and writes
a cell only when clock is up and the shadow changed; the port reproduces this
in `b43_phy_ac_mhf_maskset()`.

b43's own core init writes the host flags between `WLCOREREV` and `MACHW`; the
stock driver writes them much later. The harness follows the stock driver.
Reconciling the two orders is open.

The shadow survives a down/up and resets only with the module. A hot run of
the harness starts from a zero shadow and emits write-throughs the hot capture
does not have. It needs a seed on the same lever as `AC_FIRST_INIT`, set to the
value a cold cycle ends with.

### MAC cells emitted from `src/`

`b43_phy_ac_shm_readback_block()` writes shared-memory cells of the MAC, not of
the PHY:

- the opening accesses (`RD 0x0092`, `WR 0x000c`, `SLOTT`);
- the per-rate block reads and rewrites;
- `PHYTYPE`/`PHYVER`.

They belong in the core. They sit in the PHY because the captures put them
between the PHY write of `0x0339` and the host flag that follows, and the core
has no hook at that point. The same holds for the zeroing runs
`shm_zero_10f4`/`shm_zero_05e0` (see "Key-table clearing" above). Moving them
needs a core entry point at that position of the flow.

### `0x00cc`–`0x00d0`, the per-BSS header

- **`0x00cc`.** Bits 6–8 are the chain mask in force, which follows the
  partial mask of `b43_phy_ac_chainmask_block()` where one applies. Bit 0,
  raised by a second write after the payload, and bit 2, set on every capture,
  are opaque. `gates.sh` passes the captured value as `AC_BSS_CC`; the driver
  does not compose it yet.
- **`0x00ce`** is derived (`b43_phy_ac_beacon_pwr_offset()`). The masked field
  `0x700` the stock driver can overlay has never been seen set.
- **`0x00d0`** is written zero 849 times on three boards; nothing observed sets
  it.

### Other shared-memory cells

- **`0x078c`–`0x0790`** (station MAC, `patches/0010`): whether the ucode needs
  it, and how it relates to the AMT row, is open.
- **`SLOTT`**: the captures put both writes (`0x3ff`, then `9`) in the
  shared-memory readback block. `patches/0012` writes `9` at core init. The
  patch needs revisiting.
- **`HOSTF4`/`HOSTF5`**: `patches/0012` writes the final values (`0x0060`,
  `0x8088`), so the intermediate writes never match. This is intended.
- **`0x10f4`–`0x14b2`** is zeroed (480 words) by 7.14.89. On the DSL-3580L
  (6.30) the run is `0x10a4`–`0x1402`. What it depends on is not established.
- **b43.h map.** `BT_BASE0`/`BT_BASE1` (`0x0068`/`0x0468`) are the v4 firmware
  map; the captures load beacons at `0x0200`/`0x0480`. `PSM` (`0x05F4`) and
  `TKIPTSCTTAK` (`0x0318`) rest on the same layout assumption and have no
  evidence. Check each against the series before relying on it.
- **Cipher numbering.** The key index block sits at `0x05E0` on ucode42 with
  b43's encoding (`patches/0013`). The algorithm number 5 seen in the captures
  is past b43's enum, so the cipher numbering on this ucode is unmapped.

### Probe-response offload

This is deliberately off: b43 writes `PRMAXTIME=1`, and the item is declared in
`SOLO_VENDOR` of `test/unit/compare.py`. To implement it:

1. Keep only `b43_chip_init()`'s `PRMAXTIME=0`. The stock driver writes it
   once, b43 twice.
2. Add a `b43_write_probe_resp_template()` in the core, on the model of the
   beacon template, at template RAM `0x0700` (the TODO next to
   `B43_SHM_SH_BT_BASE0_AC` in `patches/0021`). At the bus the stock driver
   rewrites it after every beacon, once the beacon length and the MACCMD
   valid bits are written: one `RAM_CONTROL`, then 76 words on the agcombo.
   The probe response comes from mac80211 with
   `NL80211_PROBE_RESP_OFFLOAD_SUPPORT_*` and `ieee80211_proberesp_get()`.
3. Write the timings `0x0180`/`0x0182`/`0x0184`/`0x0186` = `0x0527`/`0x01f4`/
   `0`/`0x0032`. The stock driver writes them twice; the first time falls
   inside `op_init`, between `bcma_chipco_gpio_control()` and `mode_init`.
   `0x0184` is read back later, a handshake with the firmware.
4. Remove the `SOLO_VENDOR` entry.

### Power management queue

Not needed for the first bring-up. b43 keeps `B43_MACCTL_DISCPMQ` set and lets
mac80211 learn the stations' power-save state from the frames it receives; the
AC cores' stock driver clears it in AP mode (`0x44060402` -> `0x04060402` at
the bus on the agcombo) and uses the queue the MAC fills with each station's
PM-bit change. To implement it, next to the TODO in `b43_adjust_opmode()`
(`patches/0003`): clear the bit in AP mode, and in the `B43_IRQ_PMQ` handler
drain the queue and report each change to mac80211.

### SSID length

`AC_SSID_LEN` (default 8) drives the probe-response length, the PLCP of the
eight rates and `0x001e`. It can be read off the capture — `PRSSIDLEN` at
`0x0048`, the SSID at `0x0160` — but `gates.sh` does not do it yet. A segment
with another SSID fails silently.

## Core: hardware wiring of the event model

The check, the 1 s tick, the radar timer and the bss-up are wired (see
`driver-status.md`). What is left is the beacon template reload the stock
driver does from its watchdog turn, `b43_ac_beacon_reload()`.

The stock driver loads beacon templates from inside its own watchdog turn; b43
does it from `bss_info_changed` under the mutex. In the comparison each such
reload is a movable core block. It costs 32–57 operations of displacement each,
and `cmp_skip.py` has no category for it.

The check is pending at bring-up, and the calibrations it holds back run at
the bss-up, on the hot up as on the cold attach: the agcombo ch100/80 hot up
captured at the bus (`router-data/agcombo/ch100.bin`) keeps the MAC muted
for 61 s -- TX FIFOs suspended, station row cleared, no AP bit -- polls the
detector 452 times and opens the gate before the calibration block. The
harness declares the check pending on every radar channel, cold or hot;
`AC_DFS_CAC_DONE` exercises a channel whose check an earlier owner passed.

At rmmod the stock driver issues `SI.COREREG core=0 off=0x80 val=4`, which is
`pcie_watchdog_reset()` from `si_detach()`. bcma has no equivalent.

## Core: MAC and DMA, from the bus capture

`wl-diag` hooks the accessors and never saw raw MMIO, so nothing about the
MAC or the DMA in the router captures is evidence of anything. The mmiotrace
in `router-data/hybrid-4360/` is the first capture that shows them; it is
the hybrid `wl` 6.30.223 on x86, so the 7.14 reference may differ. What it
shows, against b43 v6.8 with the series applied:

- **Covered by the core:** core reset and `MACCTL 0x04000400`, ucode upload
  (10850 dwords, UCM auto-increment), `PSM_RUN` and the `MACSSPNDD` wait,
  four TX rings and one RX ring at 0x200/0x240/0x280/0x2c0/0x220 (b43 also
  sets ring 4, which `wl` never touches; data goes on FIFO 1, management on
  FIFO 3), `intrcvlazy 0x100 = 0x01000000`, `MACCMD DFQ_VALID`, `IFSSLOT`,
  the AMT clear (0011), interrupt mask and acks.
- **Covered by the firmware, to be diffed:** the ~90 IHR writes after the
  ucode (0x402-0x4e4, 0x500-0x510, 0x580-0x5a6, 0x600-0x69e, 0x800-0x8ee,
  0xa00, 0xa40) and the SHM bulks are the initvals; the five registers
  rewritten on every band switch (0x686, 0x680, 0x682, 0x700, 0x684) are the
  bsinitvals. b43 takes both from `b0g0initvals42.fw`/`b0g0bsinitvals42.fw`
  cut from 6.30.163; the list of the 6.30.223 values is in the decoded
  capture, ops #1140-#1480, and has not been diffed against the .fw yet.
- **Added by 0019:** the TX FIFO geometry (flush, seven allocations, the
  42-block chain) and the MAC clock fraction 0x6614b.
- **Still missing:** the null-data template `wl` writes at template RAM
  0x2c at attach (power save, not needed for a beacon on air); the TX status
  of this core, which `wl` reads as four words 0x170-0x17c where b43 reads
  two -- check `xmit.c` of 0008 before touching it; the `REG.WR 0x49e = 0`
  after each MACCTL write of the sample-collect loop (514 per channel set),
  where 7.14 does `PHY.MOD 0x040f`/`0x019e` and `MCTRL bit 0` instead, so it
  is a 6.30 form and not a target.
- **Not classified:** the read-modify-writes on 0x6b4/0x6b8 (BT coex),
  0x6c6 (23 per hop, `0xf0f`), 0x6f0/0x6f2, the `clk_ctl_st` 0x1e0 pass on
  every hop and the `gptimer` 0x18 writes. Small, per hop, and without a
  known consumer.

The steady-state mask `wl` runs with is `0xb0e7a860` and its `MACCTL` is
`0x44020402`; `ops_fold.py report` decodes which interrupt reasons actually
fire (TBTT|GP1|DMAINT at every beacon, TFS, TO).

## PHY: values

### Third `PHY.MOD 0x02e4` on radar-duty channels

The stock driver writes the field (`mask=0x3f00`):

- once on ch36–48 and ch144–165;
- three times on ch52–140.

The port writes it twice there. The third write sits in the channel-setup tail,
and no site emits it. The predicate is already
`b43_phy_ac_chan_has_radar_duty()`; what the field means is unknown.

### TX target `0x0646` on U-NII-3

On all eight U-NII-3 configurations (ch149–165) the stock driver writes `0x04`,
1 dBm, where the port writes `0x50`. It is not established that this is a
target at all.

A ceiling in `AC_MAX_POWER_MAP` chosen to produce it would be circular. What
would settle it, cheapest first:

1. a DSL-3580L or agcombo capture on ch149;
2. `0x0846` on the second core;
3. hot U-NII-3 segments.

### Per-core power word `0x0644` on a later bring-up

On a later channel setup the stock driver writes `PHY.MOD 0x0644+stride
val=0x0014 mask=0x007f` on each active core, after the `0x0070`/`0x1641`
bracket of the TX power-control enable. On a first bring-up it does not.
`0x14` is not derivable from the SROM; the port writes it as a constant under
`!FIRST_BRINGUP`.

### TX target: the second of three passes at 40/80 MHz

On cold ch36 and ch44 at 40 and 80 MHz the three `txpwrctrl_setup` passes write
`base`, `62`, `base`: 66/62/66 on the D6220, 68/62/68 on the agcombo. The
middle value is the same on two boards whose `maxp5ga` and `mcsbw*po` differ,
so it is not a different reading of the SROM table (the largest nibble of the
agcombo's word would give 52); it is the same `get_max()` with a different
regulatory cap in force, 68 before the 6-unit margin, 15.5 dBm written. The
per-rate `+0x0e` field of that pass is flat too, 24 on the D6220 and 72 on the
agcombo. Which locale state the middle pass runs under, and why only on U-NII-1
bonded channels (not above 5250 MHz, not at 20 MHz, not hot), is not
understood.

### Regulatory ceilings that cfg80211 cannot express

The stock driver's ceilings are per bandwidth, and cfg80211's `max_power` is per
20 MHz channel. The map in `gates.sh` reproduces the 20 MHz ceilings. Two
ceilings hold at 40 MHz only (ch60 at 22 dBm, ch100 at 24 dBm); on those the
target is the regulatory domain's, not the stock driver's.

On the TG789vac v2 the ceilings behave as conducted power, not EIRP less antenna
gain. The map therefore reproduces them only on a 5.5 dB board.

### Per-rate field `+0x0e`: a saturation

On 13 of the 15 still-divergent segments the stock driver writes
`max(distance, K)`, with K constant across the segment's rates:

| K | segments |
|---|---|
| 1 dB | ch104–144 at 20 MHz, ch60 at 40 |
| 2 dB | ch116 at 80 |
| 3 dB | ch36 at 80 |

It is not the regulatory ceiling, and not a function of width or band:

- ch100, ch104–144 and ch149–165 at 20 MHz share the `mcsbw205ghpo` word and
  the `maxp5ga` sub-band;
- yet K is 0, 1 dB and 0 on them.

ch100 at 40 and 80 MHz does not even follow that form: two tail steps of 1.5
and 2.5 dB where everything else is in half-dB multiples.

Two agcombo observations point at the locale rather than at the SROM:

- on the D6220, ch116/80 and ch132/80 have the same `maxp5ga`, the same
  `mcsbw805ghpo`, the same target (0x50) and different K, 2 dB against 0;
- the agcombo (three chains) writes twice the D6220's K where both have one:
  2 dB against 1 on ch60/40 and on ch100–140 at 20 MHz; on ch60/40 and
  ch100/40 the field is flat at 2 dB where the D6220 writes 1 dB and a ramp.

### Sub-band row offsets

The `sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*` and `mcslr5g*`
fields are extracted by `patches/0001` and are zero on every board. The loader
does not apply them, because which nibble goes to which rate is not in any open
source. The recalc warns once if a board carries them non-zero.

### RX IQ coefficient `b`, core 1

The driver solves each measurement tone separately and averages the per-tone
coefficients (`b43_phy_ac_iq_solve()`). `a` matches on 117 of 118 points and
`b` on 90 of 118. The misses are one LSB either way, almost all on core 1, and
follow the band: about 0.7 LSB high from ch100 up. `rxgainerr5ga*` is flat, so
it is not that. The agcombo is clean on all three chains, so it is not a
per-chain rule.

An input outside the six accumulators is missing. `VAL_TOLLERANZA` in
`compare.py` holds `0x?a1` at ±1 for the positional gate only; `cmp_skip.py`
still counts the misses.

`brcmsmac` forms the quotient by normalising `qq` to bit 30 and truncating the
divisor. On the 148-point set, two normalisations tie and neither dominates.
Choosing between them needs an oracle that shows the intermediate value.

### Loopback gain search at the floor

The criterion reproduces 128 of 148 search sequences. It halves the index while
the per-sample power `round(ii/1024) + round(qq/1024)` exceeds `0x169e`, and
stops at or below it or at index 0.

The 19 misses are all chain 1 at 20 MHz from ch100 up. There the search reaches
index 0 with the power 0.3–17% above the window. Whether the stock driver
accepts that, or its upper edge is slightly higher, is not in the data. The port
accepts the extreme index.

### Constants without a derivation

- **LUT base steps.** The TX IQ/LO LUT bases of tables `0x42`/`0x62`/`0x82`, and
  their steps at `0x21`/`0x1d`/`0x1b`, follow the pa5g sub-band and are
  tabulated. Why they exist, and their sizes, have no evidence.
- **The 80 MHz eleven-tap bank** at `0x?6a4`–`0x?6ae` comes in three series A,
  B, C of a `1/k` antisymmetric kernel. Which series goes to which chain is
  tabulated by (chains, core): the D6220 takes `{A, B}`, the agcombo
  `{C, B, A}`.
- **Partial chain mask** at `0x05d6`/`0x05d8`: a measured table by chain count,
  width and site.
- **Table `0x07`** `[0x3cd]`/`[0x3dd]` (`0x0c02 → 0x04c2/0x04e2`) read and write
  the same values on every capture, so field and constant cannot be told apart.
- **Block B2m** on table `0x07`: the high byte of the middle cell (`0x2f → 0x4f`
  on chain 0 only) has one point per chain.
- **RX-LPF coefficient**: 221 vs 222 and 215 vs 216 are ambiguous on two
  samples; see `txlpf-formula.md`.
- **PHY `0x0140`**: the meaning of bits [10:4].

### Discarded reads still worth a look

See [`read-census.md`](read-census.md) for the method. What remains uncertain:

- PHY `0x0393`/`0x0394` (per-index power);
- PHY `0x0550`;
- radio `0x0020/0x0022/0x003a` and strides;
- the read-modify-writes the harness cannot perturb (`0x0070`, `0x016c`,
  `0x03a9`, `0x040f`, `0x0678` family).

## PHY: operations missing or extra

### `PHY.RDW` on table `0x20`

The stock driver reads 19 cells of table `0x20` at full width: a `PHY.RD 0x0011`
followed by two `PHY.RDW`. The port stops after the `0x0011` read, so 38 reads
are missing. The values seen are `0x7f00`, `0xf3ff`, `0xf32f`, `0x1300` and
`0xf34f`.

Next step: find what table `0x20` is at that width and fix the read width at
the site.

### Counter sweep on late turns

When the stock driver's timer runs late it skips the statistics-window latch on
some turns, and on others the counter clear (`OBJ.WR 0x0308`–`0x0312`), on
different turns. The residual is 12–36 extra operations on the affected
segments.

`reverse-tools/watchdog_turns.py --check` measures the turns, but there is no
criterion to place them. Now that events have timestamps, the next attempt is a
rule on the time since the last latch.

### When the CRS block is written

The value is closed (see [`crs-min-power.md`](crs-min-power.md)); the moment is
not. On 12 cold segments, all radar-duty, the stock driver writes block E at
another latch than the port, or once more or once less.

- **`cold01`.** Its first watchdog turn collapses onto the window read. It is
  the only segment of 43 with that shape (and the only one reading the four
  head cells in address order). A rule fitted on it would be a transcription;
  if it is closed, it is closed by an input read from the capture.
- **Re-emission.** The rule by which the CRS block is re-emitted at steady state
  has not been found: a hysteresis on the ladder levels fails the negative test.

### Tempsense before the full calibration (closed)

The two readings that open a full calibration -- the channel calibration of
a bss-up on a channel without CAC, and the bss-up after the check -- are made
only with `phycal_tempdelta` non-zero, as brcmsmac does for the N-PHY on
`PHY_PERICAL_UP_BSS`. The watchdog's reading has its own cadence and is not
gated. The port does it in `b43_phy_ac_cal_reads_temp()`.

Counted with the `0x0394 = 0x0110` arm followed by `0x0393 = 0x8000` as the
signature, the d6220 reads on every segment and the tg789vac on 39, skipping
both readings on `cold01`, `cold32`, `cold33` and `cold41`. Those four are
exactly the segments of its second boot. The first boot was captured with
the script that still had `TUNE=1` by default (up to `b7fd828`), which gave
each loaded instance `phycal_tempdelta 40`; the second with the one without
it, where the instance starts from the NVRAM's 0. The weather segments count:
their check closes at turn 60 and their bss-up skips the reading like the
up's.

The harness takes `AC_TEMPDELTA` to run an instance the way it was captured;
`gates.sh` sets it from the `IOVAR.SET` the `wlc_ioctl` hook records when a
capture forces it. The tg789vac profile keeps its NVRAM 0, so its first-boot
segments want `AC_TEMPDELTA=40`. With the right value:

| segment | before | after |
| --- | --- | --- |
| `cold01` ch36/20 | 93.80% | 99.82% |
| `cold32` ch116/40, compressed | 80.45% | 99.52% |
| `cold33` ch124/40, compressed | 89.86% | 99.76% |
| `cold41` ch116/80, compressed | 68.30% | 99.59% |

Still open: the value the driver runs with at 255 in NVRAM, the unprogrammed
SROM. The DSL-3580L reports 40 through `wl phycal_tempdelta`, which its
userspace may set. Only zero against non-zero matters here.

The conversion of the raw samples is open; see
[`tempsense-wiring-evidence.md`](tempsense-wiring-evidence.md).

## Chip and board differences

- **Double analog programming.** The agcombo (4360, 7.14.43) enters the AFE arm
  unit twice in the preamble, with the PLL rewritten in between; the D6220 once.
  The gate in `op_switch_analog` is on `chip_id` because that is the only
  variable that separates the two. The TG789vac v2 (4360, 7.14.89) cold sweep
  can now tell chip from version and has not been checked.
- **TG789vac `watchdog=70000`.** Every turn-count rule (measure block every ten
  turns, region dump every thirty, entry turn, `AC_WD_PHASE`) was built on the
  D6220's 1.004 s beat. Measure the distance between two `PHY.MOD 0x0520
  mask=0xc` on that board before reusing them.

## DSL-3580L (6.30): version differences, not debt

These are recorded to tell version forks from hardware facts; none is a target.

- **prefregs.** The DSL skips radio `0x065e` and a second write.
- **AFE-LPF stage.** The DSL writes radio `0x0045 = 0x703f` and never
  `PHY.MOD 0x0728 = 0x0800`.
- **rccal.** About 6 of 38 operations differ, also on the agcombo; probably
  per-board analog measurement.
- **init_regs.** The DSL writes `0x0395/0x0315` on down→up and no `0x1645`.
- **`0x02e4`.** The DSL writes `0x0800` in the early phase, where 7.14 writes
  `0x0f00` on the cold attach.
- **PLLCTL3.** The DSL keeps the ROM value `0x00133333`: its `wl` has no
  fractional-vcofreq support.
- **`set_pdet_on_reset` and `pre_init_frontend`** are not isolated.
- **Radio `0x0033`.** The DSL writes `0x60b1`, high nibble `0x6000`, where 7.14
  writes `0x4060 → 0x4161 → 0x4181`. The port's maskset clears the `0x6000`.
- **Radio readbacks `0x040c`/`0x0416`** differ between the two 4352s.

Method for these: compare the symbols of the two blobs (`strings`,
`readelf -s`) before assuming a board difference.

## Working rules

- **An absence counts only if the class is traced** in that capture; see
  `router-data/CLASS-COVERAGE.md`.
- **Positional divergences.** Open `cmp_skip.py --verbose` first: it pairs
  `delete` and `insert` when a block has moved. A large displaced block looks
  like hundreds of content regions.
- **Conditional changes.** Measure a conditional change where the condition is
  true, not only where it is false.
- **Tolerances.** Size a tolerance on the residual of the model believed
  correct, not on the residual one can live with.
- **Partial blocks.** Emitting part of a contiguous block costs the positional
  comparison. Emit it at every site or none, and narrow `PERIMETER` in the same
  step.
- **Where to look.** Before saying a field or offset is missing, look in
  `patches/`; before declaring a core constant underivable, look in
  `brcmsmac`.
- **Co-varying predicates.** "Above 5250 MHz" and "radar duty" agree on
  ch52–140 and differ only on ch144–165. Any predicate that looks at either
  must be counted over all 43 segments, by partitioning the site's value, not
  by reading the code.
