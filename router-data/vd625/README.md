# router-data/vd625 — the board of `wl_vd625.ko`, wl1

A TIM-operator router (SSID `TIM-95961183`) whose `wl` module is
`wl_vd625.ko`:

```
driver   wl 7.14.43.21.cpe4.16L02A.0-kdb      kernel: OEM Linux 3.4 (BCM CPE)
```

The module keeps no local function symbols: `wlc_bmac_read/write_objmem16`
and hnddma's `dma64_txfast`/`dma64_txunframed` do not resolve, and wl-diag
takes the TX frames at `wlc_txfifo` and the RX frames at `wlc_recv`. The
microcode revision is not recorded here.

## Files

| file | content |
|---|---|
| `rxtx-ch36.zip` | `rxtx-ch36.txt`, decoded wl-diag trace of `wl-capture-scripts/capture_txrx.sh` on 5g36/20, 5g40/40 and 5g44/80: template RAM writes with content through each bring-up (`tpl <chanspec>`), then 512 TX and up to 512 RX frames with their d11 headers while a VHT station passes traffic (`txrx <chanspec>`), split by `MARK` records |
| `rxtx-ch36-mimo2.zip` | the same run with a two-stream VHT station: 1089 TX and 1242 RX frames, VHT on two streams in both directions, BCC received at MCS 0 |

## What it settles

What b43 needed and no earlier capture had (see `docs/retrace-todo.md`,
"HT and VHT"): the PHY TX control words, PLCP and rate of VHT data frames at
20, 40 and 80 MHz on one and two streams, the subband with the primary off
the bottom of its block, the stock RX header around VHT and legacy frames,
and the beacon template as the stock driver writes it.
