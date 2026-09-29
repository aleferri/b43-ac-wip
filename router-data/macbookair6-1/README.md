# router-data/macbookair6-1 — BCM4360 in a MacBookAir6,1, hybrid wl on x86

Taken by gonsolo for his own AC-PHY work
([`bcm4360-acphy`](https://github.com/gonsolo/bcm4360-acphy), `traces/`,
commit `2a24fb2`). Board facts are from his README; this hot capture does not
read the chip identity or the SROM.

```
PCI ID   0x14e4:0x43a0     chip 0x4360   corerev 42   radio 2069 rev 4
driver   broadcom-sta (hybrid wl), on x86
```

## Files

| file | content |
|---|---|
| `wl-init-20260926-132021.trace.xz` | bpftrace output of kprobes on `osl_read{b,w,l}`, `osl_write{b,w,l}` and `osl_delay` (his `wl_full_trace.bt`): `<ns> R32\|W16\|… <kernel VA> <value>` and `<ns> DELAY <us>` |

The accessors are the hybrid driver's hardware I/O, so the capture is the bus
traffic, like an mmiotrace, plus the delays. Decode it with

```sh
xz -dc wl-init-20260926-132021.trace.xz > /tmp/mb.trace
python3 ../../reverse-tools/mmio2ops.py /tmp/mb.trace --format bpftrace -o /tmp/mb.txt
python3 ../../reverse-tools/split_trace.py --on chanspec /tmp/mb.txt /tmp/mb/
```

## What it contains

With `wl` bound, `ifdown`, `ifup` and 20 s of NetworkManager reassociation:
96 chanspec segments.

- A scan over ch1–13 and ch36–161 at 20 MHz, about 3.5k ops per hop.
- The association on **ch64/80** (chanspec `0xe33a`, centre 58): 33k ops over
  9.4 s, with the calibrations.
- Background scans returning to `0xe33a` between hops.
- At the end **ch6/20** on 2.4 GHz (`0x1006`), again with calibrations, 16k ops.

PHY writes touch cores 0 and 1 only.

## What it establishes

- The 2069 channel table of the 6.30.102.7 blob holds on both bands with one
  register map: `reverse-tools/check_channeltab.py` on the split gives every
  BW20 row, ch1–13 and ch36–161, at 6/6 PHY, 6/6 `chan_raw6`, 39/39 radio and
  45/45 in position. The archer-t5e agrees on ch1–11.
- On 2.4 GHz radio `0x066d` is `0x18c0` on every channel, and the
  `0x08c9`–`0x08c8` block of the 5 GHz channel setup is absent.
- The Farrow resampler at 2.4 GHz: the 5 GHz 20 MHz mode with `D/M = 80/3`,
  exact on ch1–13 (and ch1–11 on the archer-t5e).
- The full table `0x20` loads, 384 words each: on 2.4 GHz exactly
  `acphy_txgain_epa_2g_2069rev4` of the 6.30.102.7 blob, on 5 GHz a table
  that differs from 7.14's in 38 words.
- The band delta of `reverse-tools/band_delta.py` against the archer-t5e;
  see `docs/retrace-todo.md`, "2.4 GHz".

## Provenance

Only the capture is taken from that repository. It also holds material derived
from decompiling `wl.ko`, which this project does not use. The repository has
no licence file: permission to redistribute the capture is pending.
