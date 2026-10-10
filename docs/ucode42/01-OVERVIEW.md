# Overview

## The four files

| file | chip | wl driver | driver build date | UCODEREV / UCODEPATCH |
|---|---|---|---|---|
| `wlD6220.o_save` | BCM4352 (2x2) | 7.14.89.14.cpe4.16L03.0-kdb | 17 May 2016 | 0x3A0 / 0x2715 |
| `wl_tg789vac_v2.ko` | BCM4360 (3x3) | 7.14.89.14.cpe4.16L03.0-kdb | 27 October 2017 | 0x3A0 / 0x05DE |
| `wl_vd625.ko` | BCM4360 (3x3) | 7.14.43.21.cpe4.16L02A.0-kdb | 7 February 2018 | 0x3A0 / 0x04B1 |
| `wlDSL-3580_EU.o_save` | BCM4352 (2x2, 5 GHz only) | 6.30.102.7.cpe4.12L07.0 | 5 May 2014 | 0x310 / 0x0002 |

UCODEREV and UCODEPATCH are what the ucode's stamping block writes to SHM
(checked by running it, `08`). Driver versions, dates and chips come from
the ELF files, which are not part of the archive of this revision: they
have not been checked again.

All four build for **D11 corerev 42**. UCODEREV tells two families apart:
0x3A0 (D6220, tg789vac_v2, vd625) and 0x310 (DSL-3580_EU, the `wl` 6.30.x
branch). D6220 and tg789vac_v2 share the `wl` although their chips differ,
and their ucode differs only in the stamp (`03`).

## Reference projects

- **b43-ac-wip**: the `b43` driver for the AC-PHY, reverse engineered from
  real MMIO captures of several boards. Source for the SHM/MMIO map
  (`b43/b43.h`), the TX header (`b43/xmit.h`) and the captures in
  `router-data/`.
- **b43-tools**: assembler/disassembler for the PSM ISA, plus
  `interpreter/` (decoder, executor, co-simulation). This revision's fixes
  are in `09`.
- **brcmsmac** (Linux): source for the initialisation order
  (`brcms_b_coreinit`), the d11init format and the `objaddr` registers.
- **OpenFWWF**: hand-written microcode for corerev 5, assembled with b43-asm
  and used on real hardware. The vendor corerev 42 ucode shares its
  structure (main loop, `mac_suspend_check`, `tx_frame_now`,
  `tx_infos_update`): it is the source for the semantics of
  `jand`/`jnand`, for condition register 4 = SPR_BRC, for the names of the
  external conditions (`cond.inc`) and for the transmit sequence.

## What each blob holds

- **`d11ucode42`**: the PSM program, 5976 64-bit instructions (5979 for
  vd625, 5002 for DSL-3580_EU).
- **`d11ac1initvals42`** / **`d11ac1bsinitvals42`**: lists of MMIO writes
  `{address, size, value}` that the host makes after the boot self-suspend
  and on a band change (`02`).
