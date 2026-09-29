# Driver status

How the port is put together and how the sources map onto the patch series.
What is open is in [`retrace-todo.md`](retrace-todo.md); the scores are in the
top-level README.

## What the driver accepts

- `op_init()` accepts only the BCMA bus and chips `0x4352`/`0x4360`
  (`-EOPNOTSUPP` otherwise).
- `op_switch_channel()` returns `-EOPNOTSUPP` on 2.4 GHz, and `-ESRCH` when the
  radio is not 2069 rev 4 or the channel table has no row for the frequency.
- The 2069 channel table has 50 rows, 5170–5825 MHz, keyed on the frequency the
  synthesiser is tuned to: the primary channel at 20 MHz, `center_freq1` at 40
  and 80. Nothing filters by configuration.

## Entry points

| entry | caller in b43 | what it does |
|---|---|---|
| `op_init` | `b43_phy_init()` | PLL check, core probe, frontend pre-init, PMU regctl, GPIO, `mode_init`, table load, `init_regs`, host flags |
| `software_rfkill` | `b43_phy_init()`, `b43_phy_exit()` | unblocked: radio init, power-on, rccal, AFE-LPF stage. Blocked: `b43_phy_ac_down()` |
| `switch_analog` | four sites in `main.c` plus `b43_phy_init()` | the AFE arm unit; the cold preamble once, on the first entry with a channel (`b43_phy_ac_cold_preamble_due()`) |
| `switch_channel` | `b43_phy_init()`, `b43_op_config()` | channel setup; a no-op when channel and width are already programmed |
| `channel_calibrate` | `b43_op_config()`, in place of its final `mac_enable` | re-enables the MAC between the RX gain-control sweep and the post-switch calibrations |
| `recalc_txpower` / `adjust_txpower` | `b43_phy_txpower_check()` | per-rate table from the SROM (`src/ppr_ac.c`), target `0x0646` per core |
| `pwork_1sec` | the 1 s periodic work of `patches/0003` | one watchdog turn while the PHY is in its run state; the bss-up once the availability check is over |
| `pwork_60sec` | b43 periodic work | CRS minimum-power threshold, written only when it changes |
| `radar_poll` | `radar_work`, every 150 ms while `conf->radar_enabled` | the detector read (`PHY 0x0251`/`0x0252`) |
| `noise_sample_done` | `handle_irq_noise()` on `B43_IRQ_NOISESAMPLE_OK` | the background noise sample |

The availability check is `dev->cac_pending`, recomputed by `b43_op_config()`
and cleared by `BSS_CHANGED_BEACON_ENABLED`; the PHY keeps its own copy.
While it is pending `b43_ac_cac_match_gate()` keeps the MAC muted: TX FIFOs
suspended, station row of the address match table without flags,
`B43_MACCTL_AP` off.

## Warnings and scaffolding

Where the driver writes something it cannot derive, it logs one `b43warn` per
site with `b43_phy_ac_todo()` (`grep -n 'b43_phy_ac_todo(dev' src/*.c`).

Some values are transcribed from captures rather than derived. On an RF chain
other than the one they were read from they can overdrive the PA, so they are
scaffolding: the FEM control table is `femctrl=6`'s, the value of every router
here, and `b43_phy_ac_set_regtbl_on_femctrl()` stops with a warning on any
other (the archer-t5e has `femctrl=1`).

- A scaffold is removed by deriving the value, or by proving with captures that
  it is invariant over the whole domain — never because the gate passes.
- A derived value must not be replaced by the constant it reduces to on the
  reference boards (`tssifloor5g[grp] & 0x3ff` is `0x03ff` everywhere). The
  trace cannot see that regression, so check refactors with a source diff too.

## Reading a divergence: phase, version, chip, board

The stock driver has two paths where b43 has one, the first attach and a
bring-up on an interface that was already up, and the same function writes
different constants in the two. Rule out the phase before blaming the chip, and
the driver version before blaming the board.

| comparison | isolates |
|---|---|
| D6220 vs DSL-3580L (both 4352) | driver version (7.14.89 vs 6.30) |
| D6220 vs TG789vac v2 (same `wl`) | chip and ucode |
| agcombo vs TG789vac v2 (both 4360) | driver version within 7.14 |

The port reconstructs 7.14; the DSL-3580L can tell a version fork from a
hardware fact and nothing else. Within 7.14 the agcombo runs `switch_channel`'s
phases in the D6220's order: anchored on the first constant write of each, all
17 anchorable phases appear in order. The differences are in repetition (the
gain-cal writes on table `0x0c` recur about 55 times on 7.14.43, once per step
on the D6220), so an agcombo capture is compared as it is, with
`gates.sh --board agcombo`.

## What may be observed

Allowed: hooking the stock driver's hardware I/O accessors (PHY, radio, MMIO,
shared memory, template RAM, OTP), trapping its raw MMIO, and reading the
blob's `.rodata`/`.data`. Not allowed: hooking its internal logic functions.
When a value is needed, the routes are, in order: an I/O accessor that is or
can be covered; an iovar from outside; a table in `.rodata`. If none is enough,
the unknown stays open.

## Source → patch map

`src/` is the source of truth for 0006; every other patch is maintained by
editing it.

| patch | content | files |
|---|---|---|
| 0001 | ssb/bcma: SPROM revision 11 extraction | outside `b43/` |
| 0003 | b43: core support for the AC-PHY, one section per change | `main.c`, `dma.c`, `xmit.{c,h}`, `leds.{c,h}`, `phy_common.{c,h}`, `b43.h`, `Kconfig` |
| 0006 | b43: AC-PHY bring-up | everything in `src/`, generated |
| 0007 | bcma: PMU init (PLL and resources) for BCM4352/BCM4360 | outside `b43/` |
| 0009 | bcma: BCM4352 (`0x43b3`) in the PCI bridge table | outside `b43/` |
| 0016 | bcma/ssb: NVRAM `ledbh4..ledbh15` over the device's own SROM | outside `b43/` |
| 0018 | bcma/ssb: `boardflags3` and `AvVmid_c0..2` from NVRAM | outside `b43/` |
| 0019 | b43: TX FIFO geometry, MAC clock fraction and MAC setup before the PHY on core revision 42 | `main.c`, `b43.h` |
| 0020 | b43: read back register addresses as the stock driver does | `b43.h`, `bus.c`, `main.c`, `phy_common.{c,h}` |
| 0021 | b43: beacon templates where the AC microcode takes them | `main.c`, `b43.h` |

`scripts/regen-patches.sh` (`KVER=6.8.0-142` selects the headers) fetches
vanilla b43 at the headers' tag with `test/integration/fetch-upstream.sh`,
applies 0003, overwrites the tree with `src/` and re-emits 0006. Message and
author come from the current 0006. A new source file enters the patch through
its line in `src/Makefile`.
