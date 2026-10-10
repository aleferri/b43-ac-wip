# Comparison of the four files

## Stamping block

The block that publishes the version in SHM is at 0xF76 (D6220,
tg789vac_v2), 0xF77 (vd625) and 0xCED (DSL-3580_EU). Run by the
interpreter:

| file | [0x0] UCODEREV | [0x1] UCODEPATCH | [0x2] | [0x3] | [0x5] |
|---|---|---|---|---|---|
| D6220 | 0x03A0 | 0x2715 | 0xEC06 | 0x0600 | 0x1526 |
| tg789vac_v2 | 0x03A0 | 0x05DE | 0x7E15 | 0x0000 | 0x1526 |
| vd625 | 0x03A0 | 0x04B1 | 0x7DDD | 0x0000 | 0x1526 |
| DSL-3580_EU | 0x0310 | 0x0002 | 0xC80F | 0xAD76 | not written |

D6220's UCODEREV and UCODEPATCH match the host reads in the real captures
(`OBJ.RD addr=0x0000 val=0x03a0`, `addr=0x0002 val=0x2715`, 66 times each in
the cold sweep). The captures never read UCODEDATE/UCODETIME, so the meaning
of [0x2]/[0x3] is OPEN.

From reset the block is reached after the SHM clear: +9633 instructions
from PSM_RUN on the 0x3A0 family, +9248 on DSL (`11`).

## ucode42

| comparison | identical bytes at the same position | mnemonic similarity | what really changes |
|---|---|---|---|
| D6220 vs tg789vac_v2 | 47800/47808 | 100% | only the stamp: 8 bytes in 3 instructions (0xF77–0xF79) |
| D6220 vs vd625 | 29.6% | 99.7% | the same code: 5929 of the 5958 aligned instructions are identical apart from jump targets |
| D6220 vs DSL-3580_EU | 24.6% | 80.9% | a related program, an earlier generation |

**D6220 vs vd625.** The 30% of identical bytes comes only from the address
shift after the first insertion. The real differences are local: 13
instructions only in D6220, 15 only in vd625, 6 replaced and 29 operand
differences. Among them: one more PHY read in vd625's main loop (0x0018) and
the SHM pair [0xC61]/[0xC62] moved to [0xC60]/[0xC61]. There is no different
register allocation.

**D6220 vs DSL-3580_EU.** 1274 instructions only in D6220, 372 only in DSL,
329 replaced. D6220's last 97 instructions have no counterpart in DSL. The
main structures are the same at relocated SHM addresses (for example the
rotating accumulator of `06` is at 0x82D–0x833 instead of 0x873–0x879).

## initvals42 / bsinitvals42

Comparison on the `{addr, size, value}` records (`02`), with sequence
alignment:

| comparison | initvals42 | bsinitvals42 |
|---|---|---|
| D6220 vs tg789vac_v2 | identical | identical |
| D6220 vs vd625 | identical but for 3 consecutive records missing in vd625 | identical |
| D6220 vs DSL-3580_EU | 475 of 615 aligned equal | 31 of 73 different at the same position |

The three records vd625 lacks are a single SHM write: `objaddr =
0x030100B0` (SHM, auto-increment, offset 0xB0 in 32-bit units) followed by
two 32-bit `objdata`, 0x00000000 and 0x00000120. So vd625 does not
initialise SHM words 0x160–0x163 (bytes 0x2C0–0x2C7).

## Summary

| axis | D6220 vs tg789vac_v2 | D6220 vs vd625 | D6220 vs DSL-3580_EU |
|---|---|---|---|
| `wl` | the same (7.14.89.14) | 7.14.43.21 | 6.30.102.7 |
| ucode42 | identical but for the stamp | a few dozen local changes | an earlier generation |
| initvals42 | identical | 3 records fewer | 23% different |
| bsinitvals42 | identical | identical | largely different |
