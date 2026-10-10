# SHM / SPR map

Full tables in `spr_shm_reference.md`, regenerated from the four blobs by
counting the direct `[addr]` operands (not the indexed ones).

## SHM addressing

The `[0xNNN]` addresses are word addresses: `host_byte = word * 2`. The
stamping writes [0x0]..[0x3] with the values the host reads at bytes
0x0000..0x0006 (`B43_SHM_SH_UCODEREV` .. `UCODETIME`), and D6220's real
trace confirms UCODEREV and UCODEPATCH.

## How many addresses

| | D6220 | tg789vac_v2 | vd625 | DSL-3580_EU | union |
|---|---|---|---|---|---|
| distinct direct SHM addresses | 507 | 507 | 512 | 403 | 704 |
| of which with a `B43_SHM_SH_*` name | 51 | 51 | 52 | 52 | 52 |
| distinct SPRs | 273 | 273 | 273 | 265 | 286 |
| of which named in `debug/include/spr.inc` | 269 | 269 | 269 | 263 | 282 |

`B43_SHM_SH_*` comes from b43/brcmsmac for the pre-AC chips: it is a working
base, checked on the fields the stamping and the captures touch, not line
by line.

## SPRs

`b43-tools/debug/include/spr.inc` names almost every SPR used, but for older
cores: for corerev 42 the names are leads, to be confirmed case by case. The
ones confirmed by the ucode's behaviour or by the captures:

| SPR | name | evidence |
|---|---|---|
| 0x018/0x019 | SPR_Ext_IHR_Address/Data | the ucode writes 0x018 with bit 14 (and 13 for a read) and waits for bit 14 to clear: the interface to the PHY registers |
| 0x042/0x043 | SPR_MAC_IRQLO/HI | 0x0DB8 writes bit 0 (MAC_SUSPENDED) when the ucode suspends |
| 0x047 | SPR_MAC_CMD | MACCMD (MMIO 0x124) |
| 0x048 | SPR_BRC | ucode state and flags; condition register 4 mirrors it (`08`) |
| 0x060..0x066 | SPR_BASE0..6 | offset registers `off0..off6` (below) |
| 0x080 | SPR_TXE0_CTL | 0x4001 starts the TX engine and 0x4000 stops it, as in OpenFWWF; the beacon block writes 0x4D95 |
| 0x086, 0x08A, 0x08B | SPR_TXE0_PHY_CTL, `_CTL1`, `0x16` | the frame's three PHY control words: the beacon block loads them from the SHM words of the beacon phyctl (bytes 0x00CC/0x00CE/0x00D0, `B43_SHM_SH_BEACPHYCTL_AC` for the first) and the PHY TX error handler copies them to SHM (`07`) |
| 0x119..0x11C | SPR_TSF_WORD0..3 | the 64-bit TSF: 0x07F3–0x07F6 of DSL-3580_EU read all of it, 0x0654 saves the low word and 0x066C measures an 8 µs wait on it; from the host it is MMIO 0x632..0x638 (0x400 + 2n) |
| 0x320 | SPR_TX_Serial_Control | written with bit 15 set once a frame is ready: 0x8001 for a queued frame, 0x8000 for the beacon (MODEL in `08`) |
| 0x321..0x327 | SPR_TX_PLCP_Sig0..1, `HT_Sig0..2`, `VHT_SigB0..1` | the frame's PLCP, copied to SHM by the PHY TX error handler (`07`) |

## Offset registers

`off0`..`off6` of `[offset, offN]` are SPRs 0x060..0x066. The evidence:
`spr.inc` calls them SPR_BASE0..6, and the corerev 42 blobs write
spr060..066 (for example `spr60 = 0x594` and `spr61 = 0x7D5` right after the
stamp) and never touch spr0C0..0C5. The arch15 assembler encoded them as
spr0C0+N, a bug fixed in `09`. Its warning about `off6` ("broken on certain
devices") does not apply to this core: D6220 uses it 66 times.

## Correlation with the MMIO traces

The vendor captures (b43-ac-wip's `router-data/`) log `OBJ.RD`/`OBJ.WR`
with the same byte addressing. On D6220 (cold sweep, 44 captures) there are
331,478 host SHM operations. In the DSL-3580L captures `OBJ.RD`/`OBJ.WR`
have no `sel` field, so SHM cannot be told apart from the other objects.

The window the host reads back on every watchdog tick (words 0x3B4–0x3C5,
bytes 0x0768–0x078A) is made of **32-bit accumulators** the ucode writes,
not of samples: for example `add. [0x3C0], spr147, [0x3C0]` followed by
`addc [0x3C1], 0x0, [0x3C1]` (at 0x0428, 0x0962, 0x0DA8), `[0x3B5:0x3B4] +=
spr14A`, `[0x3C3:0x3C2]` and `[0x3C5:0x3C4]` from `spr14A`, `[0x3BF:0x3BE]`
from r33/r34. The addends are SPRs of the IFS block (`spr147`, `spr14A`,
SPR_IFS_0x0e and neighbours in `spr.inc`). That is why in the trace the low
word changes on almost every read (549 distinct values in 590 reads of
0x3C0 on D6220) and the high one rarely (54).

## SHM words identified in this revision

Word addresses, with the host byte in brackets. Where a word has two
addresses, the first is DSL-3580_EU's (0x310), the second D6220's (0x3A0).
The sources are the listing and, where stated, b43.

| word | byte | contents | source |
|---|---|---|---|
| 0x09 | 0x0012 | DTIM period: the TBTT handler reloads the DTIM counter `r8` from it (0x00D5 on 0x310) | b43's `B43_SHM_SH_DTIMPER` |
| 0x2F | 0x005E | HOSTF1: bit 2 sends the beacon at every TBTT even without the MACCMD valid bits (0x0168 on 0x310) | `B43_SHM_SH_HOSTF1` |
| 0x66..0x68 | 0x00CC..0x00D0 | beacon phyctl, words 0..2 | listing at 0x016B (0x310), 0x0188 (0x3A0); b43 writes the first |
| 0x7E, 0x7F | 0x00FC, 0x00FE | 0x7F: PHY TX errors (0x0B1E, 0x0B2D); 0x7E: underflows with a frame in progress (0x0B24), on 0x310 | listing |
| 0x2EA.. | 0x05D4.. | TX core table per frame class: CCK, OFDM, then one cell per number of streams (`07`); b43 writes four of them, 0x05D4..0x05DA | listing at 0x0E07 (0x310) |
| 0x81A | 0x1034 | low TSF saved at 0x0654 for the 8 µs wait at 0x066C (0x310) | listing |
| 0xB64..0xB71 / 0xBFA..0xC07 | 0x16C8..0x16E2 / 0x17F4..0x180E | copy of the first TX fault (`07`) | listing at 0x0B3E (0x310), 0x0D70 (0x3A0) |
