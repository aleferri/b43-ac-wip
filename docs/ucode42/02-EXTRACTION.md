# Formats of the extracted blobs

The extraction from the ELF files (symbols `d11ucode42`, `d11ac1initvals42`,
`d11ac1bsinitvals42`, `d11ucode_ge40_bommajor/bomminor`, read from
`.symtab` with `pyelftools`) is described as it was done; the ELF files are
not in the archive of this revision, so only the resulting blobs can be
checked from here.

## Sizes

| file | `d11ucode42` | `initvals42` | `bsinitvals42` |
|---|---|---|---|
| D6220 | 47808 B = 5976 instructions | 4928 B = 615 records + terminator | 592 B = 73 + terminator |
| tg789vac_v2 | 47808 B = 5976 | 4928 B = 615 + t. | 592 B = 73 + t. |
| vd625 | 47832 B = 5979 | 4904 B = 612 + t. | 592 B = 73 + t. |
| DSL-3580_EU | 40016 B = 5002 | 4888 B = 610 + t. | 592 B = 73 + t. |

## `d11ucode42`

A bare array, with no header. Each instruction takes 8 bytes: two
big-endian 32-bit words, put together as `(word_B << 32) | word_A`, that is
with the two halves swapped with respect to a 64-bit read
(`b43-tools/disassembler/main.c`, fetch loop). The disassembler reads 8
bytes per instruction in arch5 too: the instruction size is not new in
corerev 42 (`04`).

## `d11ac1initvals42` / `d11ac1bsinitvals42`

brcm80211 `struct d11init` records of 8 bytes, here big-endian:
`{u16 addr, u16 size, u32 value}`, with `size` 2 or 4, closed by
`{0xFFFF, 0, 0}`. It is the format `disassembler/brcm80211-ivaldump` reads
and brcmsmac's `brcms_c_write_inits()` writes (`bcma_write16/32` on
`addr`). It is not `struct b43_iv`: read with that parser the blobs give
1205 and 147 "entries" and a false 2-byte trailer, which is the tail of the
terminator.

Addresses:

| | initvals42 (D6220) | bsinitvals42 (D6220) |
|---|---|---|
| records | 615 | 73 |
| with a `B43_MMIO_*` name | 520 (85%) | 69 (95%) |
| most frequent | SHM_DATA 341, SHM_CONTROL 75, RAM_DATA 70 | SHM_CONTROL 34, SHM_DATA_UNALIGNED 19, SHM_DATA 15 |

SHM writes go through `objaddr` (0x160) and `objdata` (0x164/0x166):
`objaddr = sel | (offset >> 2)` with `OBJADDR_SHM_SEL` 0x00010000 and
`OBJADDR_AUTO_INC` 0x03000000 (`brcmsmac/d11.h`), so the offset is in 32-bit
units. The initvals also write the scratch area 17 times
(`OBJADDR_SCR_SEL`) and 118 IHR registers (addresses ≥ 0x400, SPR `(addr −
0x400) / 2`), among them SPR_BRC (0x490), to 0.

## When they are written

`brcms_b_coreinit()`: `mctrl(IHR_EN | PSM_JMP_0 | WAKE)`, ucode download,
`mctrl(IHR_EN | INFRA | PSM_RUN | WAKE)`, wait for `MI_MACSSPNDD`, then the
initvals; the bsinitvals come with the band's bsinit. D6220's vendor
captures show exactly those two MACCONTROL writes (`0x04000404`,
`0x04020402`); the initvals do not appear because the vendor tracer does
not log direct MMIO writes. The ucode boot clears low SHM before the
self-suspend (`11`), so the order matters.
