# Dataflow chains (SHM → operations → SHM)

Table in `dataflow_chains.md`.

## Method

Local forward slicing: from each instruction that reads `[addr]`, the
destination is followed for 12 instructions (also reg→reg→SHM), stopping at
the first write to an `[addr2]` other than the source. The convention
"operands 0 and 1 are sources, 2 the destination" is structural in
b43-dasm's decoder. The result depends on the heuristic: redone with an
independent implementation of the same criterion, it finds 24 of D6220's 26
pairs.

## What agreement between builds means

D6220's pairs are found 26/26 in tg789vac_v2 and 24/26 in vd625, but that is
not an independent validation: tg789vac_v2 is the same binary and vd625 the
same code (`03`). The two pairs missing from vd625 are [0xC61]→[0x3E9] and
[0xC62]→[0x3EA], because vd625 moved those cells to [0xC60]/[0xC61]. With
DSL-3580_EU the exact overlap is 0/26 because the addresses are relocated,
not because the structures are missing: the same chains reappear at other
addresses (EXTNPHYCTL→0x449 instead of 0x479, the rotating accumulator at
0x82D–0x833 instead of 0x873–0x879).

## Results

- **CHAN → 0xC55.** `srx 2,8,[0x50],0x0,r33` followed by `sl 0x1,r33,[0xC55]`:
  [0xC55] = 1 << ((CHAN >> 8) & 7), a three-bit one-hot of the chanspec, not
  the channel. That those bits are the chanspec sideband is a HYPOTHESIS.
- **SPUWKUP and PRETBTT → 0x7C3.** Added together (with [0xB05]) and
  subtracted from [0x7C1] (0x102F–0x1035). That it is a combined wake-up
  time is a HYPOTHESIS.
- **EXTNPHYCTL → 0x479.**
- **0x873–0x879.** Seven cells chained with `xor`, `sr` and `orx` on data
  read by index, closing back on 0x873. The shape suggests a CRC or a serial
  hash; it remains a HYPOTHESIS.

## Host side

The table of the b43-ac-wip functions that read and write SHM (from `ctags`
and parsing `b43_shm_read/write`) is in `spr_shm_reference.md`, Addendum 3.
It is the host driver's view, not the PSM's.
