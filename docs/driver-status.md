# Driver status

How the port is put together, who calls what, and how the sources map onto
the patch series. What is still open is in [`retrace-todo.md`](retrace-todo.md);
the current scores are in the top-level README.

## Scope of what the driver accepts

- **Bus and chip.** `op_init()` accepts only the BCMA bus and chips `0x4352`
  and `0x4360`, and returns `-EOPNOTSUPP` otherwise.
- **Band.** `op_switch_channel()` returns `-EOPNOTSUPP` on 2.4 GHz.
- **Radio and frequency.** It returns `-ESRCH` when the radio is not 2069 rev 4
  or the 2069 channel table has no row for the tuned frequency.
- **The channel table.** It has 50 rows, 5170–5825 MHz, every 10 MHz where a
  bonded block needs a centre. It is keyed on the frequency the synthesiser is
  tuned to: the primary channel at 20 MHz, `center_freq1` at 40 and 80.

Nothing filters by configuration. Every 5 GHz channel and width in the table is
programmed, and the per-segment scores of the sweeps measure how far each one
is from the stock driver.

## Entry points

| entry | caller in b43 | what it does |
|---|---|---|
| `op_init` | `b43_phy_init()` | PLL check, core probe, frontend pre-init, PMU regctl, GPIO, `mode_init`, table load, `init_regs`, host flags |
| `software_rfkill` | `b43_phy_init()`, `b43_phy_exit()` | unblocked: radio init, power-on, rccal, AFE-LPF stage. Blocked: `b43_phy_ac_down()` |
| `switch_analog` | four sites in `main.c` plus `b43_phy_init()` | the AFE arm unit; the cold preamble is emitted once, on the first entry that has a channel (`b43_phy_ac_cold_preamble_due()`) |
| `switch_channel` | `b43_phy_init()`, `b43_op_config()` | channel setup; returns without emitting when channel and width are already programmed |
| `channel_calibrate` | `b43_op_config()`, in place of its final `mac_enable` (`patches/0015`) | re-enables the MAC between the RX gain-control sweep and the post-switch calibrations |
| `recalc_txpower` / `adjust_txpower` | `b43_phy_txpower_check()` | per-rate table from the SROM (`src/ppr_ac.c`), target `0x0646` per core |
| `pwork_15sec` | b43 periodic work | one watchdog turn, only while the PHY is in its run state |
| `pwork_60sec` | b43 periodic work | CRS minimum-power threshold, written only when it changes |

After the bring-up the stock driver no longer drives the flow: it reacts to
events. Three entry points model that. On hardware nothing calls them yet; the
unit harness drives them from the capture's timestamps (`reverse-tools/timeline.py`):

| entry | event in the stock driver | missing wiring |
|---|---|---|
| `b43_phy_ac_watchdog()` | 1 s watchdog callback | b43 has only the 15 s and 60 s works |
| `b43_phy_ac_radar_poll()` | 150 ms radar-detector timer (`PHY 0x0251`/`0x0252`) | no timer; b43 does not advertise radar detection |
| `b43_phy_ac_bss_up()` | AP start after the availability check: AMT row restored, tempsense, calibration block | `bss_info_changed(BEACON_ENABLED)` must call it |

The CAC state is `ac->cac_pending`:

- `b43_phy_ac_may_calibrate_tx()` is its complement on channels with radar
  duty (`IEEE80211_CHAN_RADAR`);
- `bss_up()` clears it;
- nothing in the kernel sets it. On hardware the arm belongs in
  `b43_op_config()` when `hw->conf.radar_enabled` rises.

Two helpers are declared in `phy_ac.h` for the core to define:

- `b43_ac_cac_match_gate()` suspends and restores AMT row `0x3f` around the
  check;
- `b43_ac_beacon_reload()` reloads the beacon template.

## Driver warnings

Where the driver writes something it cannot derive, it logs one `b43warn` per
site with `b43_phy_ac_todo()`. `grep -n 'b43_phy_ac_todo(dev' src/*.c` lists the
sites. There are three today:

- a partial chain mask for a chain count outside the measured table;
- the 80 MHz tap bank for a chain count outside the measured table;
- the residual on the RX IQ `b` coefficient.

## Transcribed values are scaffolding

Some values are still transcribed from captures rather than derived. On an RF
chain different from the one they were read from they can overdrive the PA, so
they are handled as scaffolding, not data:

- **The FEM control table** is `femctrl=6`'s, the only value observed (all four
  boards). `b43_phy_ac_set_regtbl_on_femctrl()` stops with a warning on any
  other value. `grep -in scaffold src/*.c` finds the site.
- **Removing scaffolding.** A scaffold is removed only by deriving the value, or
  by proving with captures that it is invariant over the whole domain — never
  because the gate passes.
- **The reverse is also true.** A derived value must not be replaced by the
  constant it reduces to on the reference boards. Example: `tssifloor5g[grp] &
  0x3ff` is `0x03ff` on every board in the repo. The trace cannot see that
  regression, so a refactor is checked with a source diff as well.

## Reading a divergence: phase, version, chip, board

The stock driver has two paths where b43 has one: the first attach, and a
bring-up on an interface that was already up. The same function writes
different constants in the two. Before attributing a divergence to the chip,
rule out the phase; before attributing it to the board, rule out the driver
version.

The boards isolate the axes:

| comparison | isolates |
|---|---|
| D6220 vs DSL-3580L (both 4352) | driver version (7.14.89 vs 6.30) |
| D6220 vs TG789vac v2 (same `wl`) | chip and ucode |
| agcombo vs TG789vac v2 (both 4360) | driver version within 7.14 |

The port reconstructs 7.14. The DSL-3580L can tell a version fork from a
hardware fact and nothing else, so it is never an oracle.

On `switch_channel`, the agcombo (7.14.43) runs the same phases in the same
order as the D6220. Anchored on the first constant write of each phase, all 17
anchorable phases appear in order, with no permutation. The differences are in
repetition, not in order: the gain-cal writes on table `0x0c` recur about 55
times through the calibration block on 7.14.43, once per step on the D6220.

## What may be observed

**Allowed.**

- Hooking the stock driver's **hardware I/O accessors** — PHY, radio, MMIO,
  shared memory, template RAM, OTP. That traces the device, not the driver's
  expression.
- Reading `.rodata` and `.data` of the blob, which is data: tables, ladders,
  constants.

**Not allowed.** Hooking internal logic functions of the driver. That turns
observing the hardware into observing the implementation.

When a value is needed, the permitted routes are, in order:

1. check whether it passes through an I/O accessor that is covered or can be;
2. query the stock driver from outside through an iovar;
3. read tables from `.rodata`.

If none of the three is enough, the unknown stays open.

## Implementation notes

- **Analog LPF caps** come from rccal. The formulas are in
  [`txlpf-formula.md`](txlpf-formula.md).
- **Width laws.** Several registers follow a law in the width step
  `b43_phy_ac_bw_step()` (0/1/2 for 20/40/80 MHz):

  | register | 20 MHz | 40 | 80 | law |
  | --- | --- | --- | --- | --- |
  | radio `0x0122` (`AFECAL_CFG`) | from the readback | | | read, armed with `\| 0x000f`, restored |
  | PHY `0x0381` | `0x7976` | `0x7987` | `0x7998` | `+0x11` per step |
  | PHY `0x0463` | `0x27` | `0x4f` | `0x9f` | `(x+1)` doubles |
  | radio `0x004e`/`0x024e` | `0x8000` | `0x8009` | `0x8012` | `+9` per step |
  | PHY `0x0738`/`0x0938` `[2:0]` | 3 | 4 | 5 | `+1` per step |

  Each law is fitted on three points; 160 MHz would be a fourth, and there is
  no capture of it.
- **PHY `0x0140`** is `(read & 0x0800) | 0x05f4`. Only bit 11 moves (set at
  20 MHz); across the 139 segments of the four sweeps the register takes only
  `0x05f4/0x05f6/0x0df4/0x0df6`. What bits [10:4] mean is unknown.
- **Bounded polls.** Every poll in the driver has a finite budget and a
  non-fatal `b43err`: for example `rxcal_afe_iter` on `0x0380`, 1000 ×
  `udelay(1)`, and the PLL lock poll, 100 × 10 µs. The stock driver peeks once,
  so the budgets are an engineering choice, not a measurement.
- **Minor forms.** The RX freeze uses `phy_mask`/`phy_set` where a single
  `phy_maskset` would do; six operations, identical semantics.

## Source → patch map

`src/` is the source of truth; `patches/` is the series applied to a kernel.

| patch | content | files |
|---|---|---|
| 0001 | ssb/bcma: SPROM revision 11 extraction | outside `b43/` |
| 0003 | b43: dedicated 5 GHz channel set for the AC-PHY | `main.c` |
| 0004 | b43: 64 KB descriptor-ring alignment for the AC-PHY DMA64 | `dma.c` |
| 0006 | b43: AC-PHY bring-up | everything in `src/`, generated |
| 0007 | bcma: PMU init (PLL and resources) for BCM4352/BCM4360 | outside `b43/` |
| 0008 | b43: wire the AC-PHY into the core TX/RX control path | `main.c`, `xmit.c` |
| 0009 | bcma: BCM4352 (`0x43b3`) in the PCI bridge table | outside `b43/` |
| 0010 | b43: station MAC in shared memory on the AC cores | `main.c`, `b43.h` |
| 0011 | b43: address match table from core revision 42 | `main.c` |
| 0012 | b43: shared-memory cells the AC cores expect | `main.c` |
| 0013 | b43: key index block relocated on ucode42 | `main.c`, `xmit.h` |
| 0014 | b43: use-after-free of `wldev` in `b43_bcma_remove` | `main.c` |
| 0015 | b43: `channel_calibrate` phyop, called from `b43_op_config` | `phy_common.h`, `main.c` |
| 0016 | bcma/ssb: NVRAM `ledbh4..ledbh15` over the device's own SROM | outside `b43/` |
| 0017 | b43: LEDs on GPIO pins above 3, several LEDs per role | `leds.{c,h}`, `main.c` |
| 0018 | bcma/ssb: `boardflags3` and `AvVmid_c0..2` from NVRAM | outside `b43/` |
| 0019 | b43: TX FIFO geometry and MAC clock fraction on core revision 42 | `main.c`, `b43.h` |

Patches other than 0006 touch files that have no counterpart in `src/` and are
maintained by editing the patch.

### Regenerating 0006

```sh
scripts/regen-patches.sh          # KVER=6.8.0-142 selects the headers
```

The script fetches vanilla b43 at the tag of the installed kernel headers with
`test/integration/fetch-upstream.sh` — the same base as the integration suite.
It then:

1. applies 0003 and 0004;
2. overwrites the tree with all of `src/` (`.c`, `.h` and the kernel `Makefile`
   with the AC lines);
3. re-emits 0006 with `git format-patch`.

Message and author come from the current 0006 via `git am`; to change them,
edit the patch and rerun. A new source file enters the patch through its line
in `src/Makefile`.

### Planned split of 0006

0006 is a monolith and must be split into commits of roughly 300–500 lines
before submission to `linux-wireless`:

| # | content | depends on |
|---|---|---|
| a | ops scaffold: allocate/free/prepare_structs, mode_init, init_regs, register access, ops struct with stub `switch_channel`/`op_init`/`software_rfkill`; the init tables | — |
| b | TX power: pa5g group, `txpwrctrl_setup`, TX gain table, idle-TSSI, `ppr_ac.c` | a |
| c | RF sequencer and reset-time setup: rfseq tables, `set_reg_on_reset`, `force_rf_sequence`, `reset_cca` | a |
| d | analog on reset: femctrl, TX/RX LPF, DACBUF, pdet | c |
| e | channel setup: classifier, clip detect, rxcore state, RX gate, chanspec tail, coefficient bank, per-channel tables, EVM shaping, ADC reset — fills `switch_channel` | b, c, d |
| f | `op_init` and `software_rfkill`: wire the PHY into b43 | e |
| g | RX gain init and gain control | e |
| h | Farrow resampler setup and tables | a |
| i | tempsense (`b43_phy_ac_tempsense()` and its leaves) | e, g |
| j | post-switch calibrations and `channel_calibrate`; watchdog, radar poll and bss-up entry points | e, g, i |
| k | `rxiqcal_phy_ac.{c,h}`: the RX IQ estimator/validator the harness flows `rxiq_est_debug`/`rxiq_comp` call | a |
