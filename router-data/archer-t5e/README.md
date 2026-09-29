# router-data/archer-t5e — BCM4360 PCIe card, hybrid wl 6.30.223 on x86

An mmiotrace of the hybrid Linux `wl`, the only capture of it and the only one
taken on x86. It shows the MAC and the DMA, which the accessor hooks never
traced; it does not show config space (window moves) or host memory
(descriptors, frames).

```
chipid   0x15134352: chip 0x4352 rev 3, 5 cores    corerev 42 (chipcommon 43, pcie2 2, gci 1, armcr4 17)
PHY      AC rev 1 (0xcb01)     radio 2069 rev 4    ucode 10850 dwords
SROM     rev 11, boardtype 0x619, boardrev 0x1123, ccode US, subband5gver 4
chains   3x3 (txchain=rxchain=7), dual-band (aa2g=aa5g=7), MAC OUI c0:25:e9
driver   hybrid wl 6.30.223
```

## Files

| file | content |
|---|---|
| `mmiotrace.zip` | the raw mmiotrace: BAR0 (16 KiB, PCIe gen2 layout), 156 791 accesses over 8.2 s |
| `srom.txt` | the 234 SROM words read through the chipcommon alias, `wl1_srom.txt` layout |
| `erom.txt` | the cores enumerated from the EROM |

Decode with `reverse-tools/mmio2ops.py trace -o decoded.txt --mark-windows`,
then `reverse-tools/ops_fold.py {fold,report} decoded.txt`; the decoded
output goes through `split_trace.py --on chanspec`.

## What it contains

Attach and `up` (13k ops, first chanspec at +78 ms), a scan — 2.4 GHz ch 1–11,
5 GHz 36–48 and 149–165, all bw20, ~3.4k ops per hop with no calibration and
two probe requests on FIFO 3 per active hop — and an association on
**ch157/80** (chanspec `0xe29b`: centre 155, sideband UL), ~30k ops with the
full channel set and calibration, then 6 s associated: a beacon interrupt every
102.4 ms, 58 data frames on FIFO 1, 134 RX descriptors.

## What it establishes

- The 2069 channel table rows for 36, 40, 44, 48, 149, 153, 157, 161, 165 and
  the 80 MHz row 155 match this board's tuning writes (`0x11a/0x11b/0x719/
  0x630/0x65c/0x662`, PHY `0x371-0x376`) exactly: another board, another
  `wl`, another host.
- The MAC init the OEM driver does and b43 does not: see `docs/retrace-todo.md`,
  "Core: MAC and DMA, from the bus capture", and `patches/0019`.
- The table 0x20 is written three words per entry on port 0x11 here, one on
  the 7.14 boards: a version difference to keep in mind when comparing table
  bodies.
