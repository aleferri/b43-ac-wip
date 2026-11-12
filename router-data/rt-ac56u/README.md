# ASUS RT-AC56U: `wl-mmio-trap` captures of the BCM43217 under `wl` 7.14

The RT-AC56U has a BCM4708 with two radios on PCIe:

- 2.4 GHz: a **BCM43217**, core rev 30, N-PHY rev 17, radio 2057 rev 14. It is DD-WRT `eth1`, PCIe 0001:01:00.0,
  BAR0 0x08000000; the BAR is 32 KiB and `wl` maps 16 KiB of it. Its sromrev 8 data comes from the CFE NVRAM
  (`0:` prefix, `nvram-0.txt` in `mmio-43217-cold.zip`, MAC address removed). It has an external PA
  (`extpagain2g=3`) and an external LNA (`elna2g=2`), with `boardflags=0x80001200` and `boardflags2=0x00001800`.
- 5 GHz: a BCM4352 (AC-PHY), DD-WRT `eth2`, outside the traced window and radio-disabled in NVRAM for these runs.

These captures have the same layout as egc112's BCM4331 set from a Netgear R6400 v1 (aleferri/b43-ac-wip#41), so
the two can be compared: an HT-PHY rev 1 against an N-PHY rev 17, both 2.4 GHz only, both with an external PA.

## Tool and firmware

- DD-WRT r66477, kernel `4.4.302-rt232-st79`, `wl.ko` 7.14.164.18 (r692288), WAN disabled (`wan_proto=disabled`).
  NVRAM country `US`, regrev 0: 20 MHz on ch 1-11, 40 MHz on 1l-7l and 5u-11u.
- `wl-mmio-trap/4-4-arm` with the sources of main c6ecb0c (`arm_syms.c` differs only in a comment), built against
  the DD-WRT 4.4 tree with OpenWrt GCC 14.4.0. The self-test passed:
  `insmod wl_mmio_trap.ko autoarm=0 win_phys=0x08000000 win_phys_len=0x4000 fsr_info_fn7=0x805702cc
  init_mm_addr=0x80576c68 fifo_recs=2097152 poll_secs=10`.
- The reader is `dd if=/proc/wl_mmio_trap bs=28` over ssh. The records were decoded with
  `reverse-tools/mmio2ops.py --format wl-mmio-trap` (main 7bc6476).
- Every run has `drops 0`, and the file size divided by 28 equals the emulated accesses plus the marks.

## Procedure

`cold.sh` and `hot.sh` (in the zips) have the exact command lines; `run-*.log` has what each step returned. Before
the runs, `/tmp/.startmon` was renamed so that DD-WRT's `check_ps` does not re-run `wlconf` and `nas` in the middle
of a capture.

- **COLD** (one file per chanspec): `rmmod wl; insmod wl`, so the attach is **not** in the trace. Then the trap is
  loaded and the following runs under it:

  `mark <label>`, `on`, `wl ap 1`, `[mark bwcap: wl bw_cap 2g 3]`, `mark chanspec: wl chanspec X`, `mark up:
  wl up` and 10 s, `mark ssid: wl ssid test-ap`, `mark bss-up` and 10 s of beaconing, `mark beaconing`, `off`,
  `mark done`.

  The chanspecs are 6, 1 and 11 at 20 MHz, and 6l, 1l and 11u at 40 MHz. The 40 MHz runs set `bw_cap 2g 3` under
  the trap: a fresh `insmod` caps 2.4 GHz at 20 MHz.
- **HOT** (one file per width): one `rmmod`/`insmod`, then an **untraced** warm-up (`ap 1`, `[bw_cap 2g 3]`,
  `chanspec 6`, `up` and 10 s, `down`). Then, under the trap, for each chanspec:

  `mark ch-X: wl chanspec X`, `mark up-X: wl up` and 10 s, `mark ssid-X: wl ssid test-ap` and 5 s,
  `mark down-X: wl down`.

  `hot-2g-20` covers ch 1-11, `hot-2g-40` 1l-7l and 5u-11u.

Two differences from egc112's runs:

- This `wl` binary does not parse `bss up` or `bss down`: it treats the argument as an integer. On an AP that is
  already up, `wl ssid` starts the BSS, so the BSS start is under the `ssid` mark. The `bss-up` mark only reads the
  state back (`wl bss`), and it read 1 in every cycle. Before the runs, untraced, the AC66U B1 next to it saw
  `test-ap` beaconing on ch 6 at -32 dBm after exactly these commands.
- "Cold" means a module reload, not a power-on. The BCM43217 keeps radio state across `rmmod` and a warm reboot.
  This board ran OpenWrt's b43 before DD-WRT was flashed for these captures, with no power cycle in between.

## Results

| file | records | emulated | `up` | `ssid` | `bss-up` (10 s beaconing) |
|---|---|---|---|---|---|
| `cold-2g6-20` | 108971 | 108964 | 31086 | 42378 | 5246 |
| `cold-2g1-20` | 110126 | 110119 | 31624 | 42342 | 6106 |
| `cold-2g11-20` | 110827 | 110820 | 31051 | 42552 | 6963 |
| `cold-2g6l-40` | 119799 | 119791 | 34843 | 50885 | 5138 |
| `cold-2g1l-40` | 122033 | 122025 | 34857 | 51909 | 6508 |
| `cold-2g11u-40` | 120573 | 120565 | 34837 | 51050 | 5868 |
| `hot-2g-20` | 644843 | 644797 | `up-X` 21 (126 on the first) | `ssid-X` 44.3k-47.0k | `down-X` ~1054 |
| `hot-2g-40` | 925338 | 925280 | `up-X` 21 (126 on the first) | `ssid-X` 52.9k-56.2k | `down-X` 1054-1446 |

The `up`, `ssid` and `bss-up` columns count decoded ops (`*-ops.txt`), not raw accesses.

- **Where the init runs.** On the first `up` after `insmod`, this N-PHY does about 31k ops (35k at 40 MHz), against
  about 8.6k on egc112's BCM4331. Once warm, it is like the 4331: `wl up` does 21 ops, and the whole per-channel
  init comes with the BSS start.
- **N-PHY table writes** (`*-tables.txt`, from `ntab.py`). PHY 0x072 holds the table id << 10 | offset; 0x074 is the
  high half, written first, and 0x073 the low half; the offset advances on each 0x073 access. The counts are 0x073
  writes.
  - `up` (cold): tables 0x00-0x03, 0x04, 0x07, 0x08, 0x09, 0x0f, 0x10, 0x11, 0x1a, 0x1b and 0x1e. Tables
    0x1a and 0x1b (26/27) take 4788 writes each over 0x000-0x23f.
  - `ssid`: the same set except 0x04 in the cold runs (0x04 is written there in the hot cycles); 0x1a and 0x1b take
    5228 writes each.
  - `bss-up`: table 0x08, 0x00a-0x00e.
  - `down-X` (hot): 0x1a and 0x1b, 0x000-0x093, 232 writes each.
  - The table set is the same on every channel. Between 20 and 40 MHz, two tables differ:
    - table 0x11 (the sample-play/tone table) is written over 160 entries at 20 MHz and 320 at 40 MHz: 2400/4800
      writes at `up` and 4960/9920 at `ssid`. That is the same 160/320 split as table 0x11 on the 4331;
    - table 0x07 gets two writes fewer at 40 MHz.

## Not covered

- The attach (`insmod wl`): the chip-control and external PA setup at probe is not in these traces.
  `autoarm=1` would catch it.
- Traffic: no client was associated during any run.
