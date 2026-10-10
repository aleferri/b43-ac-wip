# d11ucode42: dataflow chains inside the PSM (SHM -> operations -> SHM)

Input->output relations inside the microcode. For the host side see
`spr_shm_reference.md`, Addendum 3.

## Method

- Local forward slicing: from each instruction that reads `[addr]` as a
  source, the destination is followed for 12 instructions (also reg -> reg
  -> SHM), stopping at the first write to an `[addr2]` other than the source.
- "operand0, operand1 = sources, operand2 = destination" is structural in
  `b43-dasm`'s decoder for `add/sub/and/or/xor/sr/sl/rl/rr/nand`.
- The result depends on the heuristic (window, jumps ignored): an
  independent implementation of the same criterion finds 24 of D6220's 26
  pairs.
- The meaning of each chain is a HYPOTHESIS.

## Agreement between builds

| comparison | D6220 (src,dst) pairs found |
|---|---|
| tg789vac_v2 | 26 / 26: the same binary apart from the stamp |
| vd625 | 24 / 26: the same code; the two missing ones use [0xC61]/[0xC62], which vd625 moved to [0xC60]/[0xC61] |
| DSL-3580_EU | 0 / 26 at the same address pair, because of the SHM relocation |

The agreement confirms that the code is the same, not that the heuristic is
right: it is not an independent validation.

## D6220 chains (representative; identical on tg789vac_v2/vd625 except where stated)

| src | dst | length | known src | known dst | also in |
|---|---|---|---|---|---|
| 0x3D4 | 0x6B3 | 2 |  |  | tg789vac_v2, vd625 |
| 0x4A | 0x7C3 | 3 | SPUWKUP |  | tg789vac_v2, vd625 |
| 0x4B | 0x7C3 | 4 | PRETBTT |  | tg789vac_v2, vd625 |
| 0x50 | 0xC55 | 2 | CHAN |  | tg789vac_v2, vd625 |
| 0x58 | 0x479 | 2 | EXTNPHYCTL |  | tg789vac_v2, vd625 |
| 0x84E | 0x839 | 2 |  |  | tg789vac_v2, vd625 |
| 0x865 | 0x6 | 2 |  |  | tg789vac_v2, vd625 |
| 0x874 | 0x875 | 4 |  |  | tg789vac_v2, vd625 |
| 0x875 | 0x876 | 2 |  |  | tg789vac_v2, vd625 |
| 0x876 | 0x877 | 2 |  |  | tg789vac_v2, vd625 |
| 0x877 | 0x878 | 2 |  |  | tg789vac_v2, vd625 |
| 0x878 | 0x879 | 2 |  |  | tg789vac_v2, vd625 |
| 0x879 | 0x873 | 4 |  |  | tg789vac_v2, vd625 |
| 0xACC | 0xADD | 2 |  |  | tg789vac_v2, vd625 |
| 0xACD | 0xADD | 2 |  |  | tg789vac_v2, vd625 |
| 0xAD4 | 0xAEC | 3 |  |  | tg789vac_v2, vd625 |
| 0xAD5 | 0xADD | 2 |  |  | tg789vac_v2, vd625 |
| 0xAD6 | 0xAE4 | 2 |  |  | tg789vac_v2, vd625 |
| 0xADB | 0xAD8 | 2 |  |  | tg789vac_v2, vd625 |
| 0xAEB | 0xAEC | 2 |  |  | tg789vac_v2, vd625 |
| 0xAEE | 0xAE7 | 2 |  |  | tg789vac_v2, vd625 |
| 0xAEF | 0xAE7 | 3 |  |  | tg789vac_v2, vd625 |
| 0xB01 | 0xADB | 2 |  |  | tg789vac_v2, vd625 |
| 0xB05 | 0x7C3 | 2 |  |  | tg789vac_v2, vd625 |
| 0xC61 | 0x3E9 | 2 (x3) |  |  | tg789vac_v2 |
| 0xC62 | 0x3EA | 2 (x3) |  |  | tg789vac_v2 |

## The most readable chains

### CHAN -> 0xC55
```
/* 0DCD */ srx	2, 8, [0x50], 0x0, r33
/* 0DCE */ sl	0x1, r33, [0xC55]
```
[0xC55] = 1 << ((CHAN >> 8) & 7): a three-bit one-hot of the value in
`B43_SHM_SH_CHAN`, not the channel. That bits 10:8 are the chanspec
sideband is a HYPOTHESIS.

### SPUWKUP and PRETBTT -> 0x7C3
```
/* 102F */ or	[0x4B], 0x0, r35
/* 1030 */ add	r35, [0x4A], r35
/* 1033 */ add	r35, [0xB05], r35
/* 1035 */ sub	[0x7C1], r35, [0x7C3]
```
`SPUWKUP` and `PRETBTT` are timing parameters known from b43.h; they end up
added together in the same `0x7C3`. That it is a combined wake-up time is a
HYPOTHESIS.

### The 7-cell accumulator (0x873-0x879)
```
/* 1127 */ or	[0x879], 0x0, r34
/* 1128 */ xor	r34, [0x00,off4], r34
/* 1129 */ sr	r34, 0x1, r34
/* 112A */ orx	7, 8, r34, [0x873], [0x873]
```
With the chains `0x874->0x875` ... `0x878->0x879`, seven cells linked in a
ring with data read by index. The shape suggests a CRC or a serial hash:
HYPOTHESIS.

## DSL-3580_EU chains (same search, its own addresses)

| src | dst | length | known src | known dst |
|---|---|---|---|---|
| 0x4A | 0x815 | 3 | SPUWKUP |  |
| 0x58 | 0x449 | 2 | EXTNPHYCTL |  |
| 0x806 | 0x7F1 | 2 |  |  |
| 0x820 | 0x6 | 2 |  |  |
| 0x82E | 0x82F | 4 |  |  |
| 0x82F | 0x830 | 2 |  |  |
| 0x830 | 0x831 | 2 |  |  |
| 0x831 | 0x832 | 2 |  |  |
| 0x832 | 0x833 | 2 |  |  |
| 0x833 | 0x82D | 4 |  |  |
| 0xA88 | 0xA99 | 2 |  |  |
| 0xA89 | 0xA99 | 2 |  |  |
| 0xA90 | 0xAA8 | 3 |  |  |
| 0xA91 | 0xA99 | 2 |  |  |
| 0xA92 | 0xAA0 | 2 |  |  |
| 0xA97 | 0xA94 | 2 |  |  |
| 0xAA7 | 0xAA8 | 2 |  |  |
| 0xAAA | 0xAA3 | 2 |  |  |
| 0xAAB | 0xAA3 | 3 |  |  |
| 0xABD | 0xA97 | 2 |  |  |
| 0xAC1 | 0x815 | 2 |  |  |

Some are D6220's structures at relocated addresses: EXTNPHYCTL -> 0x449
(D6220 0x479) and the ring 0x82D-0x833 (D6220 0x873-0x879).
