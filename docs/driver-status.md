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
| `recalc_txpower` / `adjust_txpower` | `b43_phy_txpower_check()` | per-rate table from the SROM (`b43/ppr_ac.c`), target `0x0646` per core |
| `pwork_1sec` | the 1 s periodic work in `b43/main.c` | one watchdog turn while the PHY is in its run state; the bss-up once the availability check is over |
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
site with `b43_phy_ac_todo()` (`grep -n 'b43_phy_ac_todo(dev' b43/*.c`).

Some values are transcribed from captures rather than derived. On an RF chain
other than the one they were read from they can overdrive the PA, so they are
scaffolding: the FEM control table exists for `femctrl=6` with `femctrl_sub 0`,
every router here, and for `femctrl=2` with `femctrl_sub 1`, the MacBookAir6,1;
`b43_phy_ac_set_regtbl_on_femctrl()` stops with a warning on any other (the
archer-t5e has `femctrl=1`).

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
shared memory, template RAM, OTP), trapping its raw MMIO, reading the memory
the hardware reads by DMA (descriptors and the buffers they point at, as
`wl-mmio-trap` does at a TX index write), and reading the blob's
`.rodata`/`.data`. Not allowed: hooking its internal logic functions.
When a value is needed, the routes are, in order: an I/O accessor that is or
can be covered; DMA memory at the point the hardware is handed it; an iovar
from outside; a table in `.rodata`. If none is enough, the unknown stays
open.

## Source → patch map

`b43/` and `bcma/` are the source of truth: whole kernel files, changed or
new, over v6.8, the kernel `test/integration` builds against. The patches are
generated from them and are not edited by hand, except for their message.

| patch | content | files |
|---|---|---|
| 0001 | bcma/ssb: SROM revision 11, PMU init, the BCM4352 PCI ID, NVRAM `ledbh4..ledbh15`, `boardflags3` and `AvVmid_c0..2`, one section per change | everything under `bcma/drivers/` and `bcma/include/` |
| 0002 | b43: core support for the AC-PHY, one section per change | everything in `b43/` but the AC-PHY files |
| 0003 | b43: AC-PHY bring-up | `b43/Makefile` and the files it builds under `CONFIG_B43_PHY_AC`, with their headers |

The patches sit on other bases: 0001-0003 on v7.2, the OpenWrt `816-02` and
`816-03` on backports 7.2 with the b43 patches of OpenWrt's mac80211 package
that precede them (400, 810-815). Beyond the port they carry the adaptation to
that base, which the trees do not have, so a patch is not a diff of the tree
against the base. `scripts/regen-patches.sh` (0001-0003) and
`scripts/phase1-to-openwrt.sh` (816, given an OpenWrt tree, where it also
installs them) apply the current patches to their base and carry onto them,
by a three-way merge, what changed in the trees since they were generated;
the trees they were generated from are recorded in `patches/regen-base` and
`patches/regen-base-openwrt`, and the trees must be committed. Where the port
changed next to an adaptation the merge stops in a work directory, and after
resolving it the script resumes with `CARRY_RESUME` set to that directory.
Message, author and date come from the current patch: to change a message,
edit it in the patch and re-run. A new AC-PHY source file enters 0003 and
816-03 through its line in `b43/Makefile`; any other file in `b43/` goes into
0002 and 816-02. The 816 have `CPTCFG_B43` for `CONFIG_B43` and `b43info()`
for `b43dbg()` in the port's lines.

`880` is 0001 for OpenWrt's kernel, which carries bcma and ssb (backports does
not), and has no script.
