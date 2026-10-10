# SPR/SHM map observed in d11ucode42 (corerev 42, AC-PHY)

Tables regenerated from the four blobs with the interpreter's decoder,
counting the direct `[addr]` operands (indexed `[x,offN]` accesses are not
counted). SHM names from `B43_SHM_SH_*` of `b43-ac-wip/b43/b43.h`, SPR names
from `b43-tools/debug/include/spr.inc`.

**Files**: D6220 (BCM4352, wl 7.14.89.14), tg789vac_v2 (BCM4360, wl
7.14.89.14), vd625 (BCM4360, wl 7.14.43.21), DSL-3580_EU (BCM4352, wl
6.30.102.7).

## Method and limits

- The `[0xNNN]` addresses are word addresses: `byte = word * 2`, checked on
  the stamping (`03`) and on the host reads in the captures.
- The `B43_SHM_SH_*` names come from b43/brcmsmac for pre-AC chips: for
  corerev 42 they are a working base.
- The `spr.inc` names are from older cores: for corerev 42 they are leads.
  The ones confirmed by the ucode's behaviour are in `05`.

## Counts

| | D6220 | tg789vac_v2 | vd625 | DSL-3580_EU | union |
|---|---|---|---|---|---|
| distinct direct SHM addresses | 507 | 507 | 512 | 403 | 704 |
| of which with a `B43_SHM_SH_*` name | 51 | 51 | 52 | 52 | 52 |
| distinct SPRs | 273 | 273 | 273 | 265 | 286 |
| of which named in `spr.inc` | 269 | 269 | 269 | 263 | 282 |

## The annotated opening (D6220, as an example)

The start of the program (jump to init at 0x0F55, then the main loop from
0x0002) and the stamping block, with SHM names resolved where known:

```
%arch 15
%start entry

entry:
/* 0000 */	orx	7, 8, 0x0, 0x0, spr4E
/* 0001 */	jext	0x7F, L865
L0:
/* 0002 */	jext	0x50, L3
/* 0003 */	jext	0x4C, L3
/* 0004 */	jnzx	0, 4, r20, 0x0, L3
/* 0005 */	jnzx	0, 7, r43, 0x0, L3
/* 0006 */	jzx	0, 4, [0x2F=HOSTF1], 0x0, L1
/* 0007 */	jext	0x50, L3
/* 0008 */	jnzx	0, 8, r45, 0x0, L3
/* 0009 */	jnzx	0, 6, r44, 0x0, L3
/* 000A */	jnzx	0, 1, r45, 0x0, L3
/* 000B */	jnzx	0, 0, r63, 0x0, L3
L1:
/* 000C */	jzx	0, 9, spr47, 0x0, L2
/* 000D */	or	[0xC0A], 0x0, [0xC0B]
/* 000E */	orx	7, 8, 0x2, 0x0, spr47
L2:
/* 000F */	jnzx	0, 9, [0x30=HOSTF2], 0x0, L3
/* 0010 */	orx	7, 8, 0xFF, 0xFF, spr40
/* 0011 */	orx	7, 8, 0x73, 0x60, spr5C
/* 0012 */	orx	7, 8, 0x0, 0x1, spr5D
/* 0013 */	orx	7, 8, 0x73, 0xF, spr5E
/* 0014 */	orx	7, 8, 0xE, 0x57, spr5F
/* 0015 */	nap
L3:
/* 0016 */	orx	7, 8, 0x0, 0x0, spr40
/* 0017 */	jnext	0xB6, L4
L4:
/* 0018 */	jzx	0, 12, [0x2F=HOSTF1], 0x0, L5
/* 0019 */	jnext	0xB4, L5
...
/* 0F75 */	jne	[0x29=PHYTYPE], 0x4, L871
L871:
/* 0F76 */	orx	7, 8, 0x3, 0xA0, [0x0=UCODEREV]
/* 0F77 */	orx	7, 8, 0x27, 0x15, [0x1=UCODEPATCH]
/* 0F78 */	orx	7, 8, 0xEC, 0x6, [0x2=UCODEDATE]
/* 0F79 */	orx	7, 8, 0x6, 0x0, [0x3=UCODETIME]
/* 0F7A */	orx	7, 8, 0x0, 0x0, [0x4=PCTLWDPOS]
/* 0F7B */	orx	7, 8, 0x15, 0x26, [0x5]
/* 0F7C */	orx	7, 8, 0x7, 0xD5, spr61
/* 0F7D */	orx	7, 8, 0x5, 0x94, spr60
/* 0F7E */	orx	7, 8, 0x0, 0x0, [0x863]
/* 0F7F */	or	r3, 0x0, r5
/* 0F80 */	and	spr12D, r3, spr145
/* 0F81 */	orx	7, 8, 0x4, 0x8, r33
```


`/* 0F75 */ jne [0x29=PHYTYPE], 0x4, L871`: the branch right before the
stamping checks `PHYTYPE` (the PHY type), and the checks at the start of the
program on `[0x2F]`/`[0x30]` are `HOSTF1`/`HOSTF2`, the usual host flags
with which the driver tells the ucode which features and workarounds to
enable: a mechanism known and documented in b43 for pre-AC chips, and
apparently used in the same way on rev 42.


## SHM: addresses with a known name (from b43.h)

Occurrences as a direct operand in each blob.

| word | byte | name (b43.h) | D6220 | tg789vac_v2 | vd625 | DSL-3580 |
|---|---|---|---|---|---|---|
| 0x0000 | 0x0000 | UCODEREV | 6 | 6 | 6 | 6 |
| 0x0001 | 0x0002 | UCODEPATCH | 1 | 1 | 1 | 1 |
| 0x0002 | 0x0004 | UCODEDATE | 1 | 1 | 1 | 1 |
| 0x0003 | 0x0006 | UCODETIME | 1 | 1 | 1 | 1 |
| 0x0004 | 0x0008 | PCTLWDPOS | 2 | 2 | 2 | 2 |
| 0x0007 | 0x000E | EDCFSTAT | 2 | 2 | 3 | 5 |
| 0x0008 | 0x0010 | SLOTT | 2 | 2 | 2 | 7 |
| 0x0009 | 0x0012 | DTIMPER,DTIMP | 7 | 7 | 7 | 7 |
| 0x000C | 0x0018 | BTL0 | 1 | 1 | 1 | 1 |
| 0x000D | 0x001A | BTL1 | 1 | 1 | 1 | 1 |
| 0x000E | 0x001C | BTSFOFF | 1 | 1 | 1 | 1 |
| 0x000F | 0x001E | TIMBPOS | 1 | 1 | 1 | 1 |
| 0x0011 | 0x0022 | ACKCTSPHYCTL | 1 | 1 | 1 | 1 |
| 0x0018 | 0x0030 | TXFCUR | 38 | 38 | 38 | 34 |
| 0x001E | 0x003C | DEFAULTIV | 2 | 2 | 2 | 2 |
| 0x0020 | 0x0040 | UCODESTAT | 8 | 8 | 8 | 8 |
| 0x0021 | 0x0042 | FWCAPA | 2 | 2 | 2 | 2 |
| 0x0022 | 0x0044 | SFFBLIM | 2 | 2 | 2 | 2 |
| 0x0023 | 0x0046 | LFFBLIM | 2 | 2 | 2 | 2 |
| 0x0024 | 0x0048 | PRSSIDLEN | 3 | 3 | 3 | 3 |
| 0x0025 | 0x004A | PRTLEN | 4 | 4 | 4 | 4 |
| 0x0026 | 0x004C | NOSLPZNATDTIM | 2 | 2 | 2 | 2 |
| 0x0028 | 0x0050 | PHYVER | 3 | 3 | 3 | 3 |
| 0x0029 | 0x0052 | PHYTYPE | 7 | 7 | 7 | 8 |
| 0x002A | 0x0054 | BEACPHYCTL | 2 | 2 | 2 | 2 |
| 0x002B | 0x0056 | KTP | 2 | 2 | 2 | 2 |
| 0x002F | 0x005E | HOSTF1 | 66 | 66 | 66 | 52 |
| 0x0030 | 0x0060 | HOSTF2 | 17 | 17 | 17 | 19 |
| 0x0031 | 0x0062 | HOSTF3 | 23 | 23 | 23 | 24 |
| 0x0033 | 0x0066 | RADAR | 1 | 1 | 1 | 1 |
| 0x003A | 0x0074 | PRMAXTIME | 2 | 2 | 2 | 2 |
| 0x003C | 0x0078 | HOSTF4 | 11 | 11 | 11 | 10 |
| 0x0040 | 0x0080 | MAXBFRAMES | 2 | 2 | 2 | 2 |
| 0x0046 | 0x008C | JSSIAUX | 1 | 1 | 1 | 1 |
| 0x004A | 0x0094 | SPUWKUP | 2 | 2 | 2 | 6 |
| 0x004B | 0x0096 | PRETBTT | 3 | 3 | 3 | 1 |
| 0x004C | 0x0098 | SIZE01 | 4 | 4 | 4 | 4 |
| 0x0050 | 0x00A0 | CHAN | 14 | 14 | 14 | 9 |
| 0x0054 | 0x00A8 | MCASTCOOKIE | 2 | 2 | 2 | 2 |
| 0x0058 | 0x00B0 | EXTNPHYCTL | 2 | 2 | 2 | 3 |
| 0x005B | 0x00B6 | BCN_LI | 3 | 3 | 3 | 3 |
| 0x0061 | 0x00C2 | MACHW_H | 1 | 1 | 1 | 1 |
| 0x0066 | 0x00CC | BEACPHYCTL_AC | 1 | 1 | 1 | 1 |
| 0x006A | 0x00D4 | HOSTF5 | 13 | 13 | 13 | 13 |
| 0x006C | 0x00D8 | BT_BASE0_AC784 | 15 | 15 | 15 | 15 |
| 0x0080 | 0x0100 | CHAN_5GHZ | 2 | 2 | 2 | 2 |
| 0x0084 | 0x0108 | BCMCFIFOID | 2 | 2 | 2 | 2 |
| 0x0100 | 0x0200 | CHAN_40MHZ,BT_BASE0_AC832,CCKDIRECT | 1 | 1 | 1 | 1 |
| 0x016C | 0x02D8 | BT_BASE1_AC784 | 4 | 4 | 4 | 4 |
| 0x0380 | 0x0700 | NPHY_TXIQW0 | 1 | 1 | 1 | 1 |
| 0x0382 | 0x0704 | NPHY_TXIQW2 | 2 | 2 | 2 | 2 |
| 0x03C6 | 0x078C | AC_MACADDR | 0 | 0 | 1 | 7 |

## SHM: addresses without a known name, the most used (80 of 652)

| word | byte | D6220 | tg789vac_v2 | vd625 | DSL-3580 | tot |
|---|---|---|---|---|---|---|
| 0x0AE7 | 0x15CE | 43 | 43 | 43 | 1 | 130 |
| 0x0B2F | 0x165E | 38 | 38 | 38 | 0 | 114 |
| 0x0839 | 0x1072 | 24 | 24 | 24 | 0 | 72 |
| 0x0838 | 0x1070 | 22 | 22 | 22 | 0 | 66 |
| 0x0B32 | 0x1664 | 22 | 22 | 22 | 0 | 66 |
| 0x0AD0 | 0x15A0 | 18 | 18 | 18 | 4 | 58 |
| 0x005E | 0x00BC | 13 | 13 | 13 | 13 | 52 |
| 0x0AEB | 0x15D6 | 3 | 3 | 3 | 42 | 51 |
| 0x0AFD | 0x15FA | 17 | 17 | 17 | 0 | 51 |
| 0x0C0B | 0x1816 | 17 | 17 | 17 | 0 | 51 |
| 0x0AE4 | 0x15C8 | 16 | 16 | 16 | 2 | 50 |
| 0x0006 | 0x000C | 11 | 11 | 11 | 11 | 44 |
| 0x037A | 0x06F4 | 11 | 11 | 11 | 11 | 44 |
| 0x03E7 | 0x07CE | 14 | 14 | 14 | 0 | 42 |
| 0x03E8 | 0x07D0 | 14 | 14 | 14 | 0 | 42 |
| 0x085C | 0x10B8 | 14 | 14 | 14 | 0 | 42 |
| 0x0ADB | 0x15B6 | 13 | 13 | 13 | 2 | 41 |
| 0x0ADF | 0x15BE | 11 | 11 | 11 | 4 | 37 |
| 0x0AA3 | 0x1546 | 0 | 0 | 0 | 36 | 36 |
| 0x0042 | 0x0084 | 9 | 9 | 9 | 8 | 35 |
| 0x0AE5 | 0x15CA | 11 | 11 | 11 | 1 | 34 |
| 0x0379 | 0x06F2 | 8 | 8 | 8 | 8 | 32 |
| 0x0C61 | 0x18C2 | 11 | 11 | 10 | 0 | 32 |
| 0x07C1 | 0x0F82 | 10 | 10 | 10 | 0 | 30 |
| 0x0B48 | 0x1690 | 10 | 10 | 10 | 0 | 30 |
| 0x0C58 | 0x18B0 | 10 | 10 | 10 | 0 | 30 |
| 0x0162 | 0x02C4 | 7 | 7 | 7 | 8 | 29 |
| 0x005F | 0x00BE | 7 | 7 | 7 | 7 | 28 |
| 0x0161 | 0x02C2 | 7 | 7 | 7 | 7 | 28 |
| 0x0AE6 | 0x15CC | 9 | 9 | 9 | 1 | 28 |
| 0x0AEA | 0x15D4 | 9 | 9 | 9 | 1 | 28 |
| 0x0AEE | 0x15DC | 4 | 4 | 4 | 16 | 28 |
| 0x0AF5 | 0x15EA | 9 | 9 | 9 | 1 | 28 |
| 0x06AA | 0x0D54 | 9 | 9 | 9 | 0 | 27 |
| 0x07C3 | 0x0F86 | 9 | 9 | 9 | 0 | 27 |
| 0x07F1 | 0x0FE2 | 0 | 0 | 0 | 27 | 27 |
| 0x085B | 0x10B6 | 9 | 9 | 9 | 0 | 27 |
| 0x0B0F | 0x161E | 9 | 9 | 9 | 0 | 27 |
| 0x0B3B | 0x1676 | 9 | 9 | 9 | 0 | 27 |
| 0x0AD8 | 0x15B0 | 8 | 8 | 8 | 2 | 26 |
| 0x0AE3 | 0x15C6 | 8 | 8 | 8 | 1 | 25 |
| 0x006E | 0x00DC | 6 | 6 | 6 | 6 | 24 |
| 0x006F | 0x00DE | 6 | 6 | 6 | 6 | 24 |
| 0x0165 | 0x02CA | 6 | 6 | 6 | 6 | 24 |
| 0x0166 | 0x02CC | 6 | 6 | 6 | 6 | 24 |
| 0x03C0 | 0x0780 | 6 | 6 | 6 | 6 | 24 |
| 0x03C1 | 0x0782 | 6 | 6 | 6 | 6 | 24 |
| 0x06A8 | 0x0D50 | 8 | 8 | 8 | 0 | 24 |
| 0x07C2 | 0x0F84 | 8 | 8 | 8 | 0 | 24 |
| 0x0864 | 0x10C8 | 8 | 8 | 8 | 0 | 24 |
| 0x0AD1 | 0x15A2 | 8 | 8 | 8 | 0 | 24 |
| 0x0AF2 | 0x15E4 | 7 | 7 | 7 | 3 | 24 |
| 0x0B08 | 0x1610 | 8 | 8 | 8 | 0 | 24 |
| 0x0B1E | 0x163C | 8 | 8 | 8 | 0 | 24 |
| 0x0BE8 | 0x17D0 | 8 | 8 | 8 | 0 | 24 |
| 0x0C40 | 0x1880 | 8 | 8 | 8 | 0 | 24 |
| 0x03C9 | 0x0792 | 7 | 7 | 7 | 2 | 23 |
| 0x0B01 | 0x1602 | 7 | 7 | 7 | 0 | 21 |
| 0x0B04 | 0x1608 | 7 | 7 | 7 | 0 | 21 |
| 0x0B1B | 0x1636 | 7 | 7 | 7 | 0 | 21 |
| 0x0B94 | 0x1728 | 7 | 7 | 7 | 0 | 21 |
| 0x002D | 0x005A | 7 | 7 | 6 | 0 | 20 |
| 0x0164 | 0x02C8 | 5 | 5 | 5 | 5 | 20 |
| 0x016D | 0x02DA | 5 | 5 | 5 | 5 | 20 |
| 0x0AEF | 0x15DE | 6 | 6 | 6 | 2 | 20 |
| 0x0B51 | 0x16A2 | 0 | 0 | 0 | 20 | 20 |
| 0x0C62 | 0x18C4 | 10 | 10 | 0 | 0 | 20 |
| 0x07F0 | 0x0FE0 | 0 | 0 | 0 | 19 | 19 |
| 0x0AF0 | 0x15E0 | 5 | 5 | 5 | 4 | 19 |
| 0x0C60 | 0x18C0 | 4 | 4 | 11 | 0 | 19 |
| 0x03D3 | 0x07A6 | 6 | 6 | 6 | 0 | 18 |
| 0x03E9 | 0x07D2 | 6 | 6 | 6 | 0 | 18 |
| 0x03EA | 0x07D4 | 6 | 6 | 6 | 0 | 18 |
| 0x079E | 0x0F3C | 6 | 6 | 6 | 0 | 18 |
| 0x07A3 | 0x0F46 | 6 | 6 | 6 | 0 | 18 |
| 0x085D | 0x10BA | 6 | 6 | 6 | 0 | 18 |
| 0x0862 | 0x10C4 | 6 | 6 | 6 | 0 | 18 |
| 0x086C | 0x10D8 | 6 | 6 | 6 | 0 | 18 |
| 0x0B33 | 0x1666 | 6 | 6 | 6 | 0 | 18 |
| 0x0010 | 0x0020 | 5 | 5 | 5 | 2 | 17 |

## SPR: the most used (60 of 286), with the `spr.inc` name

| spr | name in spr.inc (older cores) | D6220 | tg789vac_v2 | vd625 | DSL-3580 | tot |
|---|---|---|---|---|---|---|
| spr065 | SPR_BASE5 | 174 | 174 | 174 | 147 | 669 |
| spr048 | SPR_BRC | 151 | 151 | 151 | 155 | 608 |
| spr119 | SPR_TSF_WORD0 | 118 | 118 | 118 | 104 | 458 |
| spr086 | SPR_TXE0_PHY_CTL | 58 | 58 | 58 | 44 | 218 |
| spr15C | SPR_BTCX_Transmit_Control | 53 | 53 | 53 | 39 | 198 |
| spr064 | SPR_BASE4 | 50 | 50 | 50 | 42 | 192 |
| spr019 | SPR_Ext_IHR_Data | 49 | 49 | 51 | 27 | 176 |
| spr1E0 | SPR_WEP_CTL | 38 | 38 | 38 | 33 | 147 |
| spr004 | SPR_RXE_FIFOCTL1 | 32 | 32 | 32 | 33 | 129 |
| spr0A4 | SPR_TXE0_FIFO_PRI_RDY | 35 | 35 | 35 | 24 | 129 |
| spr11A | SPR_TSF_WORD1 | 34 | 34 | 35 | 22 | 125 |
| spr049 | SPR_PHY_HDR_Parameter | 30 | 30 | 30 | 32 | 122 |
| spr06C | SPR_PSM_COND | 29 | 29 | 29 | 27 | 114 |
| spr0E6 | SPR_TME_VAL12 | 30 | 30 | 30 | 23 | 113 |
| spr1E5 | SPR_WEP_0x4a | 28 | 28 | 28 | 28 | 112 |
| spr0E7 | SPR_TME_VAL14 | 27 | 27 | 27 | 25 | 106 |
| spr178 | SPR_BTCX_ECI_Address | 26 | 26 | 26 | 26 | 104 |
| spr080 | SPR_TXE0_CTL | 24 | 24 | 24 | 23 | 95 |
| spr324 | SPR_TX_PLCP_HT_Sig1 | 24 | 24 | 24 | 21 | 93 |
| spr179 | SPR_BTCX_ECI_Data | 23 | 23 | 23 | 23 | 92 |
| spr084 | SPR_TXE0_WM0 | 25 | 25 | 23 | 16 | 89 |
| spr063 | SPR_BASE3 | 22 | 22 | 22 | 21 | 87 |
| spr323 | SPR_TX_PLCP_HT_Sig0 | 22 | 22 | 22 | 19 | 85 |
| spr1E2 | SPR_WEP_IV_Key | 22 | 22 | 22 | 18 | 84 |
| spr06D | SPR_PSM_0x5a | 21 | 21 | 21 | 16 | 79 |
| spr08A | SPR_TXE0_PHY_CTL1 | 21 | 21 | 21 | 14 | 77 |
| spr145 | SPR_IFS_BKOFFTIME | 19 | 19 | 19 | 19 | 76 |
| spr320 | SPR_TX_Serial_Control | 19 | 19 | 19 | 16 | 73 |
| spr047 | SPR_MAC_CMD | 18 | 18 | 18 | 17 | 71 |
| spr08D | SPR_TX_STATUS1 | 18 | 18 | 18 | 15 | 69 |
| spr095 | SPR_TXE0_FIFO_Write_Pointer | 18 | 18 | 18 | 15 | 69 |
| spr21C | SPR_AQM_FIFO_Ready | 17 | 17 | 17 | 15 | 66 |
| spr083 | SPR_TXE0_TIMEOUT | 16 | 16 | 16 | 16 | 64 |
| spr041 | SPR_MAC_CTLHI | 16 | 16 | 16 | 15 | 63 |
| spr148 | SPR_IFS_STAT | 16 | 16 | 16 | 15 | 63 |
| spr08B | SPR_TXE0_0x16 | 15 | 15 | 15 | 16 | 61 |
| spr078 | SPR_PSM_0x70 | 17 | 17 | 17 | 8 | 59 |
| spr133 | SPR_TSF_GPT2_STAT | 15 | 15 | 15 | 14 | 59 |
| spr0C6 | SPR_TME_MASK12 | 15 | 15 | 15 | 12 | 57 |
| spr093 | SPR_TXE0_FIFO_Head | 14 | 14 | 14 | 14 | 56 |
| spr00C | SPR_RXE_FRAMELEN | 13 | 13 | 13 | 13 | 52 |
| spr00D | SPR_RXE_0x1a | 13 | 13 | 13 | 13 | 52 |
| spr12D | SPR_TSF_RANDOM | 12 | 12 | 12 | 11 | 47 |
| spr018 | SPR_Ext_IHR_Address | 11 | 11 | 11 | 11 | 44 |
| spr042 | SPR_MAC_IRQLO | 11 | 11 | 11 | 10 | 43 |
| spr05C | SPR_BRWK0 | 11 | 11 | 11 | 9 | 42 |
| spr040 | SPR_MAC_MAX_NAP | 12 | 12 | 12 | 5 | 41 |
| spr147 | SPR_IFS_0x0e | 11 | 11 | 11 | 8 | 41 |
| spr081 | SPR_TXE0_AUX | 10 | 10 | 10 | 10 | 40 |
| spr0B8 | SPR_TXE0_0x70 | 10 | 10 | 10 | 9 | 39 |
| spr05F | SPR_BRWK3 | 10 | 10 | 10 | 8 | 38 |
| spr094 | SPR_TXE0_FIFO_Read_Pointer | 9 | 9 | 9 | 11 | 38 |
| spr124 | SPR_TSF_GPT1_STAT | 10 | 10 | 10 | 8 | 38 |
| spr062 | SPR_BASE2 | 9 | 9 | 9 | 9 | 36 |
| spr085 | SPR_TXE0_WM1 | 9 | 9 | 9 | 9 | 36 |
| spr0B2 | SPR_TXE0_0x64 | 9 | 9 | 9 | 9 | 36 |
| spr144 | SPR_IFS_CTL | 10 | 10 | 10 | 6 | 36 |
| spr1E7 | SPR_WEP_0x4e | 9 | 9 | 9 | 9 | 36 |
| spr075 | SPR_PSM_0x6a | 11 | 11 | 11 | 2 | 35 |
| spr02B | SPR_RXE_0x56 | 8 | 8 | 8 | 10 | 34 |

## Addendum 1: correlation with real MMIO traces (b43-ac-wip/router-data)

D6220's `cold-sweep` captures hold 331,478 host `OBJ.RD`/`OBJ.WR` operations
(byte-addressed SHM, the same scheme as `B43_SHM_SH_*`). Addresses without a
name in the ucode but read by the host, with what b43-ac-wip says (not
checked again; the `phy_ac.c` line numbers are from an earlier revision)
and the real use in the ucode (direct occurrences in D6220, tg789vac_v2,
vd625, DSL-3580_EU):

| word | byte | use in the ucode | what b43-ac-wip says |
|---|---|---|---|
| 0x0184-0x018A | 0x0308-0x0314 | 12, 12, 12, 5: written by the noise measurement routine 0x1208–0x1246 (sums of PHY registers) | the watchdog's latch window, one 32-bit cell per RF chain — `phy_ac.c:5493` |
| 0x0086/0x0087 | 0x010C/0x010E | 4, 4, 4, 4: counters incremented at 0x0DE8 and 0x09A3 | one of 4 scattered cells read at the start of the sweep — `phy_ac.c:375-408` |
| 0x00AC/0x00AF | 0x0158/0x015E | 4, 4, 4, 0 | the other two of the same 4 cells |
| 0x03B4-0x03C5 | 0x0768-0x078A | 28 in each blob: 32-bit accumulators (below) | the watchdog's "flat sweep", read whole every tick — `phy_ac.c:405-406` |
| 0x03BE | 0x077C | 2: `add. [0x3BE],r33` / `addc [0x3BF],r34` | a 32-bit counter read hi/lo/hi — `phy_ac.c:11017-11022` |
| 0x0066 | 0x00CC | 1: loaded into SPR_TXE0_PHY_CTL by the beacon block (`07`) | chain mask and BSS state — `phy_ac.c:1079-1090` |
| 0x00A7 | 0x014E | 2, 2, 2, 4: a counter incremented at 0x09D9 | last read of the statistics poll — `phy_ac.c:10862` |
| 0x00AD | 0x015A | 2, 2, 2, 0 | second to last read of the same poll — `phy_ac.c:10326` |

## Addendum 2: usage patterns in the traces

Words 0x03C0/0x03C1 (bytes 0x0780/0x0782) are the most variable of the
watchdog window:

| board | reads of 0x0780 | distinct values of 0x03C0 | distinct values of 0x03C1 |
|---|---|---|---|
| D6220 | 590 | 549 | 54 |
| tg789vac_v2 | 6169 | 5859 | 827 |
| DSL-3580_EU (46 full-sweep segments) | 477 | 435 | 17 |

They are not samples: the ucode updates them as a 32-bit accumulator,
`add. [0x3C0], spr147, [0x3C0]` / `addc [0x3C1], 0x0, [0x3C1]` (0x0428,
0x0962 and 0x0DA8, on the suspend path), with `spr147` cleared afterwards. In
the same way `[0x3B5:0x3B4]`, `[0x3C3:0x3C2]` and `[0x3C5:0x3C4]` accumulate
`spr14A`. That is why the low word changes on almost every read and the high
one rarely. `spr147` and `spr14A` are in the IFS block (`spr.inc`:
SPR_IFS_0x0e and neighbours); that they measure medium times is a
HYPOTHESIS.

Only in the DSL-3580L captures does the host write bytes 0x10B6-0x10F2
(words 0x85B-0x879), about 31 writes per segment, without ever reading them
back; in the D6220 and tg789vac-v2 captures those addresses do not appear.

## Addendum 3: SHM input -> function -> SHM output (host side, from real source)

The goal: tie each input address to the function that consumes it and to
what it produces. This is possible **only on the host side** (b43-ac-wip's
C code, whose functions have real names), not on the ucode side (the PSM has
no symbols, see above).

Extracted with `ctags` (function boundaries) and a simple parser of the
`b43_shm_read16/32`/`b43_shm_write16/32` calls in `phy_ac.c, main.c, xmit.c,
leds.c, helpers_phy_ac.c, radio_2069.c`: 660 calls found, 56 functions that
both read and write SHM. Of these, the 6 below have clean addresses
(literals or macros) on **both** the input and the output side; the other
50 use variables computed at run time (loop index, chain offset) that the
static parser cannot resolve without running the code.

| function | file | IN (SHM) | OUT (SHM) |
|---|---|---|---|
| `b43_phy_ac_shm_readback_block` | phy_ac.c | 0x0092 | B43_SHM_SC_MINCONT, B43_SHM_SC_MAXCONT, B43_SHM_SH_SLOTT, 0x0052, 0x0050 |
| `b43_phy_ac_bss_cc_update` | phy_ac.c | 0x00cc | 0x00cc, 0x00cc |
| `b43_phy_ac_shm_mac_config_block` | phy_ac.c | 0x0eec | 0x0020, 0x08ec, 0x0910, 0x08f4, 0x08f0, 0x0eec |
| `b43_phy_ac_op_switch_channel` | phy_ac.c | 0x0056 | 0x0094, B43_SHM_SH_RFATT, 0x018a, 0x018c, 0x0074, 0x0082, 0x00ba, 0x003c, 0x007c, 0x005a |
| `b43_write_beacon_template` | main.c | B43_SHM_SH_BEACPHYCTL | B43_SHM_SH_BEACPHYCTL |
| `b43_mgmtframe_txantenna` | main.c | B43_SHM_SH_ACKCTSPHYCTL, B43_SHM_SH_PRPHYCTL | B43_SHM_SH_ACKCTSPHYCTL, B43_SHM_SH_PRPHYCTL |

Relations **with explicit arithmetic** (an address computed from another,
not just a name):

| function | file | IN | OUT |
|---|---|---|---|
| `b43_amt_write` | main.c | (index * 2, (index * 2 | (index * 2, (index * 2 |
| `b43_rate_memory_write` | main.c | offset | offset + 0x20 |
| `b43_update_basic_rates` | main.c | - | basic + 2 * offset |

A readable example: `b43_rate_memory_write` reads at `offset` and writes at
`offset + 0x20`, a fixed 32-byte offset between two parallel SHM regions
(probably two rate tables, a read one and a "+0x20" write one).

This table is the host driver's view, not the microcode's; for the PSM see
`dataflow_chains.md`. It comes from b43-ac-wip's source and was not checked
again in this revision.
