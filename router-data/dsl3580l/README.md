# router-data/dsl3580l — D-Link DSL-3580L

A 4352 board, driver **6.30.102.7** (`cpe4.12L07.0`), kernel 2.6.30. It is not
an oracle: its `wl` is older than the reference, and its captures serve to tell
version forks from hardware facts.

There is one `wl` per router, so that version serves both cores the module
drives, wl0 and wl1. `router_info.txt` prints it once for that reason, not
because it describes wl0 only.

## Files

| file | content |
|---|---|
| `cold01-ch36-bw20.txt` | 41580 ops, decoded with `RETVAL` folded; the reference DSL capture |
| `out-cold-dsl-decodificata.txt` | the same run, decoded, not folded |
| `out-cold-dsl-merged.txt` | the same run, folded |
| `full-sweep.zip` | hot sweep: segments in `20a/`, `20b/`, `20b-bis/`, `40/`, `80/`, `80-bis/` plus the whole decoded trace per width; does not trace `CAL` |
| `wl1_otp_dump.txt` | `wl -i wl1 otpdump`. Almost all zero: the only non-zero words in the first `0x50` are `0x000c: 0x1b08`, `0x0020: 0x0500`, `0x003c: 0x4352 0x4001` |
| `wl1_srom_raw.txt`, `wl1_nvram.txt` | raw SROM and NVRAM. On the fields that matter the NVRAM equals the D6220's, `boardtype 0x668` included; only `boardnum` differs |
| `wl1_phytable_5gl.txt` | PHY tables, low 5 GHz band |
| `router_info.txt` | **describes wl0**, the N-PHY core integrated in the 6362 (`chipnum 0x6362`, `corerev 0x16`, `phytype 0x4`); it holds no wl1 revinfo |
| `dsl3580l_pmu-trace.txt` | ChipCommon PMU resource masks of the 4352, read with `pcicfg` + `mempeek` with `wl` down |
| `bcm43b3_3580l_map.bin` | the 480-byte SROM map file the driver loads |
| `wl1_curpower_ch52-bw80.txt` | `wl -i wl1 curpower` on ch52/80: regulatory, board and per-rate targets. The maximum, 15.00 dBm, is the `0x0646 = 0x3c` the stock driver writes in `full-sweep.zip!80/seg01-ch52.txt` |
| `wl1_curppr_ch52-bw80.txt` | `wl -i wl1 curppr`, same session: each rate's distance from the maximum. The header says 1/4 dB, but the values match `curpower` only when read as half-dB. There is no 80 MHz section |
| `wl1_phy_txpwrindex_ch52-bw80.txt` | `wl -i wl1 phy_txpwrindex`, same session: TX gain table index of the closed loop, 23 on both cores. The bring-up programs `0x14` and then `0x13` |

The shared-memory zeroing above `0x1000` covers different ranges on the two
4352 boards:

- here it is `0x10a4`–`0x1402` (432 words);
- the D6220 (7.14.89.14) zeroes `0x10f4`–`0x14b2` (480);
- the two boards share the chip, `boardtype 0x668` and the relevant NVRAM.

It is what the newer driver zeroes, not board data. The port follows 7.14.

The captures were armed with `skipphyrd="0x253,0x254"`. On DFS channels the
radar detector polls those two registers continuously, up to 85% of all PHY
reads, and they have nothing to do with the channel configuration. `0x251` and
`0x252` are kept.

## The captures contain wl0's attach

`wl` attaches both cores on every load, and the hooks are on `wl`'s functions,
not per core, so the head of every capture belongs to wl0's N-PHY. On
`cold01-ch36-bw20.txt` it is `#295`–`#404`, closed by a 2.27 s gap.

Attribute with two criteria, not with the gap alone:

- **the CPU**: wl0's operations are on `cpu0`, our core's bring-up on `cpu1`;
- **named registers**: `0x0078`, `0x008f`, `0x00a5`, `0x00a6` and `0x00a7` are
  `RFCTL_CMD`, `AFECTL_OVER1`, `AFECTL_OVER`, `AFECTL_C1` and `AFECTL_C2` in
  `b43/phy_n.h`, and are not in `phy_common.h`, so they are N-PHY and nobody
  else's. The `clr 0x0400` on `0x0078` is `RFCTL_CMD_CHIP0PU`: wl0 powering its
  radio down after attach. There are no RAD operations in that window.

`OTP.RDR`, `OTP.INIT`, `SROMCTL.RD/WR` and the two `PHY.WARR` fall inside it.
**They are not ours and are not to be ported.**

In `full-sweep.zip` the complete bring-up appears only on U-NII-1:

- the segments with ~5000 `TBL.WR` are channels 36–48 at 20 MHz and 36/44 at
  40 MHz;
- every DFS segment stops at ~143, because the driver does not reach the table
  loads before the mandatory CAC.

**Read plans depend on timing.** `0x0270` is a poll on bit 0 followed by a
peek, and the number of iterations depends on how long the hardware takes. The
DSL plan in `test/unit/readplan_0270.h` is used only by flows **without**
`AC_READ_ORACLE`; with the oracle it stays at `iter=0`.
