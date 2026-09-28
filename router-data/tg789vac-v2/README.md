# router-data/tg789vac-v2 — BCM4360 on driver 7.14.89.14

Technicolor TG789vac v2, a VDSL2 gateway on a BCM63168 SoC (BMIPS4350, two
hardware threads):

- `wl0` is the N-PHY core on the internal bus (`BCM435f`, `sb/0/`);
- `wl1` is the BCM4360 on PCI (`pci/2/0/`), the one of interest.

The map file the driver loads for `wl0` is called `bcm6362_map.bin`; that is
the file's name, not the router's SoC.

```
deviceid   0x43a2      chipnum 0x4360   chiprev 0x3    chippackage 0x1
corerev    0x2a        phytype 0xb      phyrev  0x1    radiorev 0x42069
boardid    0x6d8       boardrev P120    sromrev 11     3x3 dual band
driverrev  0x70e590e = 7.14.89.14 (cpe4.16L03.0-kdb)
ucoderev   0x3a005de ~ 3.160.5.222
```

## Why this board

It runs the same `wl` as the D6220 on the 4360, which completes the table:

| | 7.14.43 | 7.14.89 |
| --- | --- | --- |
| 4352 | — | D6220 (DSL-3580L runs 6.30) |
| 4360 | agcombo | **TG789vac v2** |

The ucode differs from the D6220's (the image is per chip family), so this
board also separates **driver from ucode**. The shared-memory cells and the
statistics window are contracts with the ucode, not with the driver.

**What does not move** across chip, board type and driver branch:

- the rxgain triplets (5gl `(3,6,1)`, 5gm/5gh `(7,15,1)`, all three chains);
- `femctrl=6`, `subband5gver=0x4`, `boardflags=0x10000000`, `boardflags2=0x2`;
- `epagain5g=0`, `tssiposslope5g=1`, `gainctrlsph=0`, `paparambwver=0`,
  `xtalfreq=65535`.

**What moves:**

| field | D6220 | agcombo | here |
| --- | --- | --- | --- |
| `pdgain5g` | 10 | 10 | **19** |
| `maxp5ga0` | 72,70,86,0 | 74,74,82,82 | **90,88,92,88** |
| `aga0/1/2` | 133 | 133 | **68/68/67** |
| `tempthresh` | 255 | 255 | **120** |
| `phycal_tempdelta` | 255 | 255 | **0** |
| `temps_period` / `hysteresis` | 15 | 15 | **5** |
| `watchdog` | 3000 | 3000 | **70000** |

- **`pdgain5g`** selects the AvVmid set in the port (set 19 is this board's).
- **The four temperature fields** have thermal recalibration configured here,
  where the reference boards have placeholders.
- **`watchdog=70000`**: measure the turn cadence (two `PHY.MOD 0x0520 mask=0xc`)
  before reusing any rule counted in watchdog turns.

## Files

These dumps went through a terminal copy-paste, not a redirect. They are
provisional until redone with `ssh tg789 '<command>' > file`. The transcription
was cross-checked between the NVRAM and the raw SROM:

- `boardrev`, `boardtype`, `devid` and `sromrev` against their SROM words;
- the 36 `pa5ga` words;
- the six `maxp5ga`.

| file | content |
| --- | --- |
| `wl1_revinfo.txt` | `wl -i wl1 revinfo` |
| `wl1_nvram.txt` | `wl -i wl1 dump nvram` |
| `wl1_srom_raw.txt` | `wl -i wl1 srdump`, 234 words, sromrev 11 |
| `wl0_srom_raw.txt` | `wl -i wl0 srdump`, 220 words, **sromrev 8**, deviceid `0x4354`, boardid `0x566`: the SoC's N-PHY core, out of scope, kept to document who is who |
| `wl1_otp_dump.txt` | `wl -i wl1 otpdump`: all zero except `0x000c: 0x1fa5` and `0x0020: 0x0500` |
| `dmesg-wl.txt` | `dmesg \| grep wl` at boot |
| `cold-sweep.zip` | 43 cold segments plus the preamble, same numbering and configurations as the D6220's |

## The SROM is not hardware

```
wl:srom/otp not programmed, using main memory mapped srom info(wombo board)
wl: loading /etc/wlan/bcm4360_map.bin
wl: reading /etc/wlan/bcmcmn_nvramvars.bin, file size=32
```

No SROM or OTP is programmed. What `srdump` prints is a flash file mapped into
memory, so **the reference data is the binary**, as for the DSL-3580L's
`bcm43b3_3580l_map.bin`. Still missing:

```
/etc/wlan/bcm4360_map.bin        wl1's map
/etc/wlan/bcmcmn_nvramvars.bin   32 bytes, read after the map, so it may override fields
/etc/wlan/bcm6362_map.bin        wl0's map, for completeness
```

A `wl -i wl1 phytable` dump in the agcombo's form is also missing.

`macaddr=12:13:31:f6:da:77` has the locally-administered bit set and comes from
the map file: a placeholder. `srom[4]`–`[6]` are **not** the MAC.

## Platform

Technicolor firmware on OpenWrt: `DISTRIB_RELEASE` Chaos Calmer 15.05.1
(r46610), target `brcm63xx-tch/VANTF`, kernel `3.4.11-rt19` SMP PREEMPT, gcc
4.6.4 (OpenWrt/Linaro 4.6-2013.05). `/proc/config.gz` is absent
(`CONFIG_IKCONFIG_PROC` off).

For out-of-tree modules:

- The `kernel` package version (`opkg info kernel`) has the form
  `3.4.11~<md5>-r<rel>`. The md5 is of the sorted `=[ym]` lines of the vendor
  `.config` (`include/kernel-defaults.mk`), so it validates a candidate config
  exactly.
- The vermagic is read from the vendor `.ko` files (`strings wl.ko | grep
  vermagic`). With no `__versions` section, `MODVERSIONS` is off and that string
  is the whole `insmod` gate.
- Build with `CONFIG_DEBUG_PREEMPT` off. In 3.4 that option turns
  `preempt_disable()` into a call to `add_preempt_count()`, which this kernel
  does not export.

SoC, `uname -r` and toolchain come from serial boot logs of TG789vac v2 units;
confirm on this unit with `cat /proc/cpuinfo` and `uname -a`.

## Reloading `wl` without crashing the router

On this firmware `rmmod wl` + `insmod wl` alone crash the router. There are
three independent causes; `reverse-tools/capture_cold_tg789vac.sh` handles all
three.

**1. The wireless modules' memory is reserved and never freed.**

- The kernel places `wl`, `wlemf` and `wfd` by name in a contiguous physical
  region outside vmalloc: 5 MiB of cores from `0x80b8e000` to `0x8108e000`, then
  ~1.45 MiB of init.
- The core region is a page-aligned bump allocator that never goes back.
- The cursor is the word at `0x80575464` (found with `memfind`).
  `wl_diag bump_ptr=0x80575464 restore_alloc=1` rewinds it to `module_core` at
  `wl`'s `GOING` when the block is at the top, and checks at the next `COMING`
  that the new `module_core` matches.
- The address holds for this kernel build only. Find it again with
  `memfind 0x2000 0xb8c000 0x8108e000 0x80f84000`.

**2. hostapd keeps `/dev/wl_event` open.** Its `file_operations` live inside
`wl`'s block, so rewriting the block on reload crashes the next `read()` or the
first event. Stop it with `/etc/init.d/hostapd stop` before the first `rmmod`;
this also takes 2.4 GHz down.

**3. `/lib/wireless/init_broadcom.sh` configures the instance once per boot.**

- It sets `nar 0`, `phycal_tempdelta 40` (the NVRAM has 0) and the radar
  thresholds for `wl1`.
- It does so with an `up`/`down` on the default chanspec, ch44. So **the stock
  driver never does a cold init on the operating channel**, and hostapd's `up`
  is a hot one.
- The captured instances stay at Broadcom defaults, by choice. At the end the
  script removes the once-per-boot marker and restarts hostapd.
- That was not so for the first boot of the sweep: the script of the time
  (`TUNE=1`, up to `b7fd828`) gave every loaded instance `nar 0` and
  `phycal_tempdelta 40`. The delta decides whether a full calibration opens
  with a temperature reading, so the first-boot segments compare with
  `AC_TEMPDELTA=40`, and those of the second boot -- `cold01`, `cold32`,
  `cold33`, `cold41` -- with the NVRAM's 0, the profile's value. See
  `docs/retrace-todo.md`, "Tempsense before the full calibration".

**Watch:** every `rmmod` prints `dqmHandlerRegisterHost: Exceeded maximum number
of DQM IRQ Handlers! (8)` twice per interface. That is a handler leak on
`wfd` unbind with a ceiling of 8, so reloads per boot are limited; the script
counts them.

## cold-sweep.zip

The segments are `cold01-ch36-bw20` … `cold25-ch165-bw20`, `cold26-ch36-bw40` …
`cold37-ch157-bw40`, `cold38-ch36-bw80` … `cold43-ch149-bw80`.

**How it was taken.** `capture_cold_tg789vac.sh` ran over two boots:

- the first boot did seven consecutive runs (`20`, `40`, `80`, `20dfs`,
  `40dfs`, `80dfs`, `20meteo`), with hostapd stopped and restarted between runs;
- the second boot did `20 36` again, `40meteo` and `80meteo`.

Each segment has one `CS.SHM` write, on the requested channel (the block centre
at 40 and 80), and the op count varies by 1–2% within a run.

The split, per trace:

```
python3 reverse-tools/split_trace.py --on mod --drop-between-runs --prefix cold cold.txt split/
python3 reverse-tools/trace_filter.py --retvals split/<seg>.txt <seg>.txt
```

`--drop-between-runs` removes what lies between a run end and the next
`mod COMING`. That is the hostapd restart, half a million operations that belong
to no cycle.

**Things to know:**

- **`cold20-ch144-bw20` has a halved init**: 12.3k operations in the up phase
  against 33.6k on its neighbours, with 232 `MAC.MCTRL` toggles against 75. It
  is a different init branch (ch144 is not allocated under ETSI) and the only
  segment that exercises it.
- **The first boot hung on the first `40meteo` cycle**, after seven runs and
  forty clean reloads. The second boot passed `40meteo` and `80meteo` at once,
  so it accumulates. The measurable candidates are the DQM handler leak and the
  reload count.

**Before using the segments as evidence:**

1. Check class coverage (`../CLASS-COVERAGE.md`).
2. Attribute wl0's attach by CPU and named registers (method in
   `../dsl3580l/README.md`).
3. Remember the captures were armed with `skipphyrd="0x253,0x254"`: the radar detector's `0x0253`/`0x0254` reads are filtered out.

`gates.sh --board tg789` runs the harness with this board's profile.
