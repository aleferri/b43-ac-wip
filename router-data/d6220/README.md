# router-data/d6220 — Netgear D6220 wl1

The reference board of the repository.

```
PCI ID   0x14e4:0x43b3 (BCM4352 family)   boardid 0x668   boardrev P355   sromrev 11
driver   wl 0x70e590e ~ 7.14.89.14        ucode 0x3a02715   kernel: OEM Linux 3.4
```

It is the same reference design as the DSL-3580L (P353), a consecutive board
revision from a different OEM.

## Files

| file | content |
|---|---|
| `cold-sweep.zip` | the source of the cold gate: `cold00-preambolo.txt` plus 43 cold segments `coldNN-chC-bwB.txt`, one module reload per configuration (25 at 20 MHz, 12 at 40, 6 at 80) |
| `hot-sweep.zip` | `segmenti/`: 44 hot `NN-up-chC-bwB.txt` segments, the source of `gates.sh --hot`; `hot-gaps/`: what the capture holds between consecutive `up` segments |
| `wl-diag-wl1-steady-tick-ch36-bw20.txt` | one steady-state watchdog turn on ch36 bw20: the oracle of the periodic gate |
| `wl1-ch36-phytables.txt`, `wl1_ch44_phytables.txt` | PHY table dumps (`wl phytable`) on ch36 and ch44 |
| `wl1_phyreg_rxgain.txt` | `wl -i wl1 phyreg 0x{6,8,a}f9` on 5g36/20 and 5g100/20, taken after `wl up` and association to a 5 GHz AP |
| `wl1_revinfo.txt` | `wl -i wl1 revinfo` |
| `wl1_srom_raw.txt` | `wl -i wl1 srdump` |
| `wl1_nvram.txt` | `wl -i wl1 dump nvram` |
| `wl1-log.txt` | `wl` lines of the boot log |
| `rxtx-1s-ht20-40-vht20-40-80-txbf0.zip` | decoded `wl-diag` trace of `wl-capture-scripts/capture_txrx.sh` with `TXBF=0`, a one-stream VHT phone passing traffic: `VHTMODE=0` on 5g36/20 and 5g40/40 (the phone joins as HT), then `VHTMODE=1` on 5g36/20, 5g40/40 and 5g44/80; 512 TX frames with their d11 headers, 512 RX frames and 512 TX statuses per chanspec, cut by `MARK` records; with the `pianifica()` hook plan of the session |

The sweep segments contain wl0's attach at their head, like the other two-core
boards; `reverse-tools/strip_other_core.py` removes it.

## Throughput of the stock driver

Ookla Speedtest from the phone (one stream) to a server in Milan, through
the router's WAN, during `rxtx-1s-ht20-40-vht20-40-80-txbf0`: HT20 67 Mbit/s,
HT40 112, VHT20 67, VHT40 135, VHT80 267. End to end, so the line and the
router's forwarding bound them as much as the radio. The TX frames the
capture's budget took are small (100-350 bytes) and go out in aggregates of
up to 32: by their size, TCP ACKs or the test's latency phase rather than
its download.

## What the D6220 settles

**Same topology as the DSL-3580L.** PCI ID, chipnum, chiprev, corerev,
radiorev, phytype, phyrev, sromrev, `subband5gver`, `txchain`/`rxchain`,
`femctrl` and boardid are all identical. The patched b43/bcma/ssb path must
handle both without distinction.

**The `+2` in the rxgains populator.** PHY `0x?f9` bits 14:8 read `0x16` instead
of the `0x14` that 6.30 gives. The 7.14 branch applies a `+2` that 6.30 does not
(also seen on the agcombo, 7.14.43).

**A second PA dataset**, different from the DSL-3580L's.

- Every `pa5ga0/1/2[12]` word differs.
- `maxp5ga0/1` is asymmetric per sub-band (`72,70,86,0` against the DSL's
  `76,76,76,76`).
- `maxp5ga0[3] = 0` is a sub-band capped to zero, to be treated as "unavailable
  on that chain", not as 0 dBm.

## Open

**The rxgains SROM encoding** of the other halves (elnagain, trelnabyp). The
triplets are identical to the DSL-3580L's (5gl `(3,6,1)`, 5gm/5gh `(7,15,1)`,
2g `(0,0,0)`), and so are the SROM bytes (`0xffff 0xb300` at `srom[112-113]`).
Closing it needs a board of the family with different triplets.

**The `+2` as an algebraic identity** with 6.30's `(triso+4)<<1`. **SALAME**: it
holds on the single data point (`0x16 − 2 = 0x14 = (6+4)<<1` for `triso=6` on
5gl). Other `triso` values (5gm/5gh give 15) would test it, by forcing an
attach-time repopulate per sub-band and re-reading `0x{6,8,a}f9`.

## Raw SROM against the DSL-3580L

**Identical word for word:**

- `srom[8..15]` (chip identity) and `srom[48]` (`0x43b3`);
- `srom[64].lo` (boardnum `0x0634`);
- `srom[80..103]` (PA/ag) and `srom[104..111]` (`pa2gccka0` and noiselvl);
- `srom[112-113]`, `srom[132-133]`, `srom[152-153]` (rxgains, chains 0/1/2);
- `srom[168..175]`.

**Different:**

- `srom[64].hi` (boardrev);
- `srom[114-127]` (maxp5ga0 + pa5ga0), `srom[134-147]` (chain 1),
  `srom[154-167]` (chain 2);
- `srom[176..199]` (the `mcsbw*po` and `sb*po` power offsets);
- the final CRC.

**Partial dumps.** The DSL shows `srom[72-74]` non-zero, where Netgear zeroes
them; the D6220 shows `srom[18-39]` non-zero, where the DSL zeroes them.
Different OEM drivers log different SROM blocks, so each dump is partial.
