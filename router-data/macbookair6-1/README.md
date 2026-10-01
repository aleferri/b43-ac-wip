# router-data/macbookair6-1 — BCM4360 in a MacBookAir6,1, hybrid wl on x86

Taken by gonsolo for his own AC-PHY work
([`bcm4360-acphy`](https://github.com/gonsolo/bcm4360-acphy)): the first
capture from his `traces/` at commit `2a24fb2`, the others sent to this
project on 2026-10-01.

```
PCI ID   0x14e4:0x43a0     chip 0x4360   corerev 42   radio 2069 rev 4
driver   broadcom-sta (hybrid wl), on x86
```

## Files

| file | content |
|---|---|
| `wl-init-20260926-132021.trace.xz` | bpftrace output of kprobes on `osl_read{b,w,l}`, `osl_write{b,w,l}` and `osl_delay` (`wl_full_trace.bt`): `<ns> R32\|W16\|… <kernel VA> <value>` and `<ns> DELAY <us>` |
| `wl-init-20260929-093137.trace.xz` | the same script, driven by `trace_wl.sh`: `ifdown`, `ifup`, 20 s of reassociation |
| `wl-firstload-20260926-190438.trace.xz` | ftrace kprobe events of the same accessors plus `osl_pci_write_config`: `modprobe wl` from unloaded to a 2.4 GHz association |
| `wl-firstload-5g-20260926-225244.trace.xz` | the same, to a 5 GHz association |
| `wl-tx-20260926-194512.trace.xz` | the same, then traffic |
| `macdump-wl-{1,2}.txt`, `macdump-b43-{1,2}.txt` | `<offset> <value>` of the D11 registers 0x000-0xddf under `wl` and under this port |
| `srom-raw.txt` | the SROM words through the chipcommon alias, `word[NNN]:` rows; the image is words 0x000-0x0ef, the alias repeats from 0x100 |
| `chipcommon-pmu-state-b43-loaded.txt` | chipcommon and PMU registers with this port loaded |
| `trace_wl.sh`, `wl_full_trace.bt` | the capture scripts of the bpftrace ones |

Driver `broadcom-sta` 6.30.223.271 (nixpkgs), kernel 6.18.53, chip rev 3.
The SROM and every capture carry the card's MAC address (SROM words
0x48-0x4a, the address match table). The callers the ftrace lines name are
the nearest global symbol of a stripped `wl.ko`, not the function: do not
attribute ops by them.

Decode the bpftrace ones with `--format bpftrace`; the ftrace ones are
recognised:

```sh
xz -dc wl-tx-20260926-194512.trace.xz > /tmp/tx.trace
python3 ../../reverse-tools/mmio2ops.py /tmp/tx.trace --keep-flush -o /tmp/tx.txt
python3 ../../reverse-tools/split_trace.py --on chanspec /tmp/tx.txt /tmp/tx/
python3 ../../reverse-tools/timeline.py /tmp/tx.txt /tmp/tx.tl
```

## What they contain

- `wl-init-20260926`: `ifdown`, `ifup`, a scan over ch1-13 and ch36-161 at
  20 MHz, about 3.5k ops per hop; the association on **ch64/80** (chanspec
  `0xe33a`), 33k ops over 9.4 s with the calibrations; background scans
  returning to `0xe33a`; at the end **ch6/20** (`0x1006`). 96 segments.
- `wl-init-20260929`: the same cycle, ending on **ch116/80**, 39k ops over
  16 s. 40 segments, 580 interrupts. One read lost its address in the script
  and is dropped.
- `wl-firstload`: the attach (EROM, SROM, PMU), a scan at 20 MHz on both
  bands and the association on **ch1/20** (`0x1001`), 41 s, 1048 interrupts.
- `wl-firstload-5g`: the attach and the association on **ch100/80**
  (`0xe36a`), with scans returning to it; 303 segments, 1495 interrupts.
- `wl-tx`: the attach, the association on ch1/20 and traffic: 242 TX
  descriptor posts, each a write of the TX ring 1 index (`0x0244`), with
  only reads around it (TSF `0x0180`/`0x0184`, `0x0160`, `MACCONTROL`);
  MACCONTROL is `0xc0020403`, without `AWAKE`, at 142 of them.

PHY writes touch cores 0 and 1 only.

## What they establish

- The 2069 channel table of the 6.30.102.7 blob holds on both bands with one
  register map: `reverse-tools/check_channeltab.py` on the split of
  `wl-init-20260926` gives every BW20 row, ch1-13 and ch36-161, at 6/6 PHY,
  6/6 `chan_raw6`, 39/39 radio and 45/45 in position. The archer-t5e agrees on
  ch1-11.
- On 2.4 GHz radio `0x066d` is `0x18c0` on every channel, and the
  `0x08c9`-`0x08c8` block of the 5 GHz channel setup is absent.
- The Farrow resampler at 2.4 GHz: the 5 GHz 20 MHz mode with `D/M = 80/3`,
  exact on ch1-13 (and ch1-11 on the archer-t5e).
- The full table `0x20` loads, 384 words each: on 2.4 GHz exactly
  `acphy_txgain_epa_2g_2069rev4` of the 6.30.102.7 blob, on 5 GHz a table
  that differs from 7.14's in 38 words.
- The band delta of `reverse-tools/band_delta.py` against the archer-t5e;
  see `docs/retrace-todo.md`, "2.4 GHz".
- The SROM is rev 11, board type `0x0117`, board rev `0x1204`. The rev 11
  extractor of `bcma/` reads `aa2g = aa5g = 3`, `txchain = rxchain = 3`,
  `antswitch = 0`, `subband5gver = 4`: a 2x2 board. Read with the rev 8
  offsets the same image gives `txchain 6`, `rxchain 0`, `antswitch 6` (word
  0x51), which is what a kernel without the rev 11 parser reports.
- The PMU: `wl` writes `0x0c31` and `0x100e` to PLL control 2 and 3, as on the
  D6220 and the agcombo, and `max_res_mask = 0x1ff` twice, where `bcma/` sets
  `0x7ff` for every 4352/4360 (see `docs/retrace-todo.md`).
- The EROM: chipcommon rev 43, D11 rev 42, ARM CR4 rev 2, PCIe Gen2 rev 1,
  USB 2.0 device rev 17, as on the archer-t5e.

## Provenance

Only the capture is taken from that repository. It also holds material derived
from decompiling `wl.ko`, which this project does not use. The repository has
no licence file: permission to redistribute the capture is pending.
