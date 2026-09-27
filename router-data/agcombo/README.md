# router-data/agcombo — BCM4360, radio 2069

The first non-BCM43b3 board of the collection.

```
PCI ID   0x14e4:0x43a2     chipnum 0x4360   chiprev 0x3   corerev 0x2a
PHY      AC rev 1          sromrev 11       boardrev P353 (0x1353)
chains   3x3 (txchain=rxchain=7), dual-band (aa2g=7, aa5g=7), full PA chain on both bands
driver   0x70e2b15 ~ 7.14.43.21   ucode 0x3a004b1 ~ 3.160.4.177
```

## Files

| file | content |
|---|---|
| `wl1_revinfo.txt` | `wl -i wl1 revinfo` |
| `wl1_srom.txt` | `wl -i wl1 srdump` — all zero on this blob; the nominal SROM is in the NVRAM dump |
| `wl1_nvram.txt` | `wl -i wl1 nvram_dump` |
| `wl1_phytable_5gl.txt` | `wl -i wl1 phytable 0x{44,45} {0..41} 8`, plus a width-disambiguation probe at `0x44` offset 0 |
| `wl1_pmu-trace.txt` | PMU resource masks |
| `stats.txt` | `wl` status reads (chanspec, `curpower`, `phy_tempsense`, …) from a session on ch100/80 |
| `cold-sweep.zip` | 26 cold segments, `coldNN-chC-bwB.txt` |
| `hot-sweep.zip` | 26 hot `up` segments, `NN-up-chC-bwB.txt` |
| `bss-up.zip` | one bss-up on ch100/80 with the extended hook set (AMT, ADDRM, OBJ.BULK, PHY.FGC) |
| `cold-sweep-partial.tar.gz` | an older partial cold split, `split-agcombo/`; does not trace `OBJ` |

The sweeps lack the classes listed in `../CLASS-COVERAGE.md`.

## What it establishes

**Chip-side dispatch.** The `b43_phy_ac_*` path is exercised on BCM4360 as
well as on BCM4352. Both are sromrev 11, radio 2069, `subband5gver 0x4`.

**The default rxgain triplets are the radio's.** All three chains read the same
triplet on every 5 GHz sub-band: 5gl `(3,6,1)`, 5gm `(7,15,1)`, 5gh `(7,15,1)`.
PHY `0x{6,8,a}f9` bits 14:8 read `0x16` on all three chains. The D6220 and the
TG789vac v2 show the same values.

**7.14 populates rxgains once, at attach.** PHY `0x6f9` reads `0x1602` on both
5g36/20 and 5g100/20, and `phytable 0x44 32 8` reads `0x07` on both. The port
calls `b43_phy_ac_rxgain_init` only at `op_init`.

**The rxgains encoding.** `0x16` is `((triso+4)<<1)+2`, with the `+2` of the
7.14 branch, which implies `triso=6` at runtime. The only natural bit layout
that maps the SROM byte `0xb3` to `triso=6` is `(b<<7)|(t<<3)|e`.

**The `wl phytable` format** on this blob:

- width 8 uses a byte stride;
- the low byte of the printed 32 bits is the table-id echo and is discarded;
- the high-order byte is the data;
- widths 16 and 32 are little-endian reinterpretations of the same buffer.

**Chanspec 5g100/20 is accepted** (`Chanspec set to 0xd064`) with `ccode=""` and
`regrev=0`.

**The analog arm unit is entered twice in the preamble**, with the PLL
rewritten in between; the D6220 enters it once. See `docs/retrace-todo.md`.
