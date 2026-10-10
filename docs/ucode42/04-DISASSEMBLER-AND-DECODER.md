# Disassembler and decoder

## b43-dasm on corerev 42

`b43-dasm -a 15 -f raw-be32` decodes the four blobs without errors:

| file | instructions | warnings |
|---|---|---|
| D6220 | 5976 | 24 (12 `nap` with zero operands, two warnings each) |
| tg789vac_v2 | 5976 | 24 |
| vd625 | 5979 | 24 |
| DSL-3580_EU | 5002 | 0 |

With the assembler fixes in `09`, disassembling and reassembling gives back
the original binary for DSL-3580_EU, and for the other three apart from the
12 `nap` (the vendor encodes them with zero operands, b43-asm with r0).

## Instruction format (arch15)

Each instruction takes 8 bytes (this holds for arch5 too; `02`). Fields,
from `disassembler/main.c`:

- opcode = bits[50:39] (12 bits)
- operand0 = bits[38:26], operand1 = bits[25:13], operand2 = bits[12:0]
  (13 bits each)

A 13-bit operand:

| pattern | meaning |
|---|---|
| bit12 = 0 | direct SHM, 12-bit word address |
| bits[12:11] = `11` | 11-bit signed immediate |
| bits[12:7] = `101111` (mask 0x1F80 = 0x1780) | GPR, 7-bit number |
| bits[12:10] = `100` | SPR, 10-bit number (the blobs use up to 0x342) |
| bits[12:10] = `101` | indexed SHM `[offset, offN]`: 7-bit offset, 3-bit N |

The GPR test comes before the indexed one: `[x, off7]` does not exist, that
pattern is a GPR.

## The interpreter's decoder

A Python port of the same logic, bit by bit. Compared with `b43-dasm` on all
four blobs: no difference in mnemonic or target, apart from four opcodes
(0x000, 0x002, 0x070, 0x071; 0x002 appears twice, five instructions in all)
that b43-dasm does not know either.

## What the decoder does not say

The disassembler only needs to know where operands and targets are, not
what they do. The execution semantics come from elsewhere (`08`): OpenFWWF
for `jand`/`jnand` and the comparisons, b43-asm's emulation for
`jzx`/`jnzx`, the real captures for the stamping.
