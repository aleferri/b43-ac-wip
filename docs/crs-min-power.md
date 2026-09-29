# CRS minimum-power thresholds and bank `0x0910`

The AC-PHY carrier-sense threshold: one common threshold on the eight CRS
registers, and for every chain beyond 0 an offset in bank `0x0910`. In the port
it runs from `b43_phy_ac_crs_ladder[]` to `b43_phy_ac_prog_bank_0910()` in
`b43/phy_ac.c`.

**[BLOB]** is read from the D6220's 7.14.89.14 binary; **[MEASURED]** is
checked on the D6220 cold (43) and hot (44) sweeps and the agcombo cold (26)
and hot (26).

## What is written   [MEASURED]

**The common threshold.** It is the low byte of `0x0321`, `0x0324`, `0x0327`,
`0x032a`, `0x032d`, `0x0330`, `0x0333` and `0x0336`: the clip-detector
thresholds, stride `0xc`, interleaved across chains. All eight always carry the
same sequence of values.

**The per-chain bank.**

- Four registers per chain at stride `0x200`: `0x0910`–`0x0913` for chain 1,
  `0x0b10`–`0x0b13` for chain 2.
- `0x0710`, which would be chain 0, appears in no capture. There is one bank per
  wired chain beyond 0: one on the D6220 (three cores, two chains), two on the
  agcombo.
- Eight read-modify-writes per bank, always in this order:

      reg+0 hi, reg+0 lo,  reg+2 hi, reg+2 lo,
      reg+1 lo, reg+1 hi,  reg+3 lo, reg+3 hi

- The value is a signed byte (`0xfb` is −5), the same in both bytes of all four
  registers: 1356 high/low pairs over the four sweeps, no exception.
- The bank is write-only.

## The ladder   [BLOB]

The ladder is in the D6220's `.rodata`: three rows of 15 `u8` entries,
zero-terminated, stride 16.

| offset | width | entries |
|---|---|---|
| `+0x40e84` | 20 MHz | 45 48 51 53 54 57 60 63 66 68 70 72 75 78 80 |
| `+0x40e94` | 40 MHz | 44 46 48 50 52 54 56 58 60 63 66 69 71 74 76 |
| `+0x40ea4` | 80 MHz | 41 44 46 48 50 52 55 57 60 63 65 68 70 72 74 |

A single function references the three rows, `wlc_phy_crs_min_pwr_cal_acphy`.
The agcombo (7.14.43) uses the same rows: every value it writes, threshold or
bank, is an entry of them [MEASURED].

## The sample   [MEASURED]

**The noise window.** The watchdog latches the SHM window `0x0308`–`0x0314`,
one 32-bit cell per chain: `0x0308` chain 0, `0x030c` chain 1, `0x0310`
chain 2.

- On the 4352s the third cell is zero; on the agcombo it is not.
- The high words are always zero, and `0x0314` is always `0x45`.

**The sample** is an in-channel integrated power and scales with the width:
median 1384 at 20 MHz, 2525 at 40, 3832 at 80.

## The rule   [MEASURED]

For each chain and each latch:

    n     = sample >> width step                (0 at 20 MHz, 1 at 40, 2 at 80)
    v     = how many of {1024, 1536, 2048, 3072, 4096} n reaches
    index = {0, 1, 3, 4, 6, 7}[v] + anchor      (0 at 20 MHz, +2 at 40 and 80)

The index goes into a ring of four per chain. The index in force is the ring's
mean **rounded up**. Then:

    common threshold = row[index of chain 0] + 4 when cold
    bank of chain c  = row[index of chain c] - row[index of chain 0]

**The +4 bump** applies to the first two calibrations of the session, and
cancels in the bank. The sum `threshold + bank` is chain c's threshold: the
stock driver writes the common one for all chains and the correction for the
others, and the hardware adds them.

**Holes in the index.** A single sample's index has holes at 2 and 5, and the
mean fills them. 55 at 20 MHz and 56 at 40 appear only behind a window where
the noise straddles 1536.

### Check

On every CRS block with at least one sample before it:

| capture | common thresholds | bank values |
|---|---|---|
| D6220 cold | 63/63 | 63/63 |
| D6220 hot | 39/39 | 39/39 |
| agcombo cold | 13/13 | 26/26 |
| agcombo hot | 10/10 | 20/20 |

How tightly each threshold is pinned, from the blocks whose ring holds a single
sample, that is where one sample decides the index alone:

| threshold | highest below | lowest above |
|---|---|---|
| 1024 | 1001 | 1025 |
| 1536 | 1535 | 1538 |
| 2048 | 2047 | 2049 |
| 3072 | 2914 | 3147 |
| 4096 | 3907 | 4279 |

- **1536 and 2048** are pinned to within three units; **1024** only from above.
- **4096 → 7** rests on a single sample above, 4279 on agcombo hot.
- **Above 4096** no capture shows how the ladder continues. **SALAME**: the
  thresholds fall on half-octaves and the index rises by three per octave, about
  one per dB, and the ladder would continue that way.

A level no capture reached is not an extrapolation as long as the sample stays
below 4096: the thresholds under it are pinned, and a quieter or noisier
environment only picks another entry of the ladder. The driver does not warn on
the noise level.

## Where it is written   [MEASURED]

- **`chanspec_tail()`**, inside the channel setup. On the first bring-up the
  threshold is a literal per width, 58, 60 and 61, that is `LUT20[4]`,
  `LUT40[6]` and `LUT80[7]` plus 4. The bank is zero because the rings are
  empty.
- **Block E**, right after a sample latch: the common threshold and the bank,
  from the rule.
- **`pwork_60sec()`**: the common threshold only, and only when it changes.

## Other boards

The DSL-3580L (6.30) writes the bank as zero everywhere and has its own ladder
(`0x1fa19c`, stride 12, eleven entries; the 40 MHz row equal, the 20 MHz row
two entries off, the 80 MHz row nine). The port uses the D6220's rows on every
board. Ring reset on sub-band change and when the block is emitted are open,
see `retrace-todo.md`.

## Finding the ladder in the blob

It is the initializer of a local array, so it sits in anonymous `.rodata` with
no symbol. It is found by shape (monotonic, steps of 2 to 8) and attributed
through the `R_MIPS_HI16`/`R_MIPS_LO16` pairs in `.rel.text`: the function
containing the relocation is the one that loads it. Before declaring a small
table absent, look for immediate stores too.
