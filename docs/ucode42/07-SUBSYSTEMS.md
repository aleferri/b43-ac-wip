# Subsystems examined

## CAC / radar threshold (DFS)

Host side. Symbols found in D6220 and DSL-3580_EU: `radarthrs` (IOVAR),
`wlc_dfs_iovars`, `wlc_dfs_doiovar`, `wlc_dfs_cacstate_*`. tg789vac_v2 and
vd625 have `wlc_set_dfs_cacstate` and `wlc_phy_radar_detect_on_off_cfg_acphy`
instead. They come from the ELF files, not in the archive of this revision:
not checked again.

## The block at 0x0374 (D6220)

The previous dossier read it as "AMPDU first/next". With the correct
semantics of `jand`/`jnand` (`08`) and condition register 4 = SPR_BRC, the
block says:

```
0374 jzx   0,14,[0x01,off0],0x0, 0381   ; bit 14 clear      -> jump
0375 jext  0x44, 03C1                   ; SPR_BRC bit 4     -> jump
0376 jnand [0x01,off0],0x200, 0381      ; bit 9 set         -> jump
0377 jzx   0,2,[0x2F],0x0, 037D         ; HOSTF1 bit 2
0378 jzx   0,0,[0x6AA],0x0, 037D
0379..037B  spr1F2..1F4 <- [0x6AD..0x6AF]
037D..037F  spr1F2..1F4 <- [0x6AA..0x6AC]
0380 orx   7,8,0x0,0x4,spr1F0           ; command: bit 2 of spr1F0
0381 ...                                ; both branches join
```

- spr1F2..1F4 are set up if bit 14 of the word is set, bit 4 of SPR_BRC
  clear and bit 9 of the word clear. The twin block at 0x03C4–0x03C6 makes
  the same check with `jnzx 0,9` instead of `jnand ...,0x200`: with b43-asm's
  semantics the two blocks would have contradicted each other.
- In OpenFWWF bit 4 of SPR_BRC is `COND_MORE_FRAGMENT`.
- If off0 points to the AC TX header (`struct b43_txhdr_ac` without `toe`),
  [0x01,off0] is `mac_ctl_lo`, and bit 14 is `B43_TXH_AC_MAC_STMSDU` ("first
  MSDU"). It is a HYPOTHESIS: where off0 points at that moment is not
  checked. The `B43_TXH_MAC_AMPDU_*` values (0x200/0x400/0x600) belong to the
  old cores' TX header and do not apply here.
- `calls 15F5` at 0x0397 is reached from both branches: there are not two
  separate setup paths.
- At 0x03C7 the ucode waits in a loop for bit 2 of `spr1F0` to clear:
  `spr1F0` behaves as the command register of a hardware engine, with
  `spr1F2..1F4` as parameters (HYPOTHESIS). The same pattern reappears at
  0x0A2E, 0x0C39, 0x0C42, 0x0F1F and 0x163D.

## MAC suspend

The vendor ucode has the same check as OpenFWWF (`mac_suspend_check`):

```
00BE jnext 0x24, 0DA2            ; COND_MACEN clear -> suspend check
0DA2 jnzx  0,8,spr148,0x0, 00C0  ; SPR_IFS_STAT bit 8 -> not yet
0DA3 jnand 0xE2,spr48, 00C0      ; work flags in SPR_BRC -> not yet
0DA4 jext  0x2D, 00C0            ; COND_TX_PHYERR
0DA5 jext  0x2C, 00C0            ; COND_TX_TBTTEXPIRE
0DA6 jext  0x22, 00C0            ; COND_TX_DONE
0DA7 calls 0E1A                  ; stop the TX engine (SPR_TXE0_CTL = 0x4000)
0DAE [0x20] = 3                  ; UCODESTAT = SUSP
0DAF je [0x5C],0x0, 0DB8         ; nothing to send -> suspend
0DB0..0DB7                       ; otherwise: transmit, then back to the loop
0DB8 spr042 |= 1                 ; MAC_SUSPENDED
0DBB..0DBF                       ; wait for COND_MACEN
0DC0 [0x20] = 2                  ; UCODESTAT = ACTIVE
```

When the host has just written a nonzero word to SHM 0x00B8 (0x7148, once
in almost every segment of D6220's cold sweep), the ucode copies it to SPR
0x0E7, sets SPR_TXE0_CTL = 0x4001, puts 2 in the low bits of SPR_BRC and
goes back to the main loop. The word is brcmsmac's `M_CTS_DURATION`
(`d11.h`), which `phy_n.c` sets to 10000 before suspending the MAC for the
RX calibration: the frame is a CTS-to-self, the value its duration in µs.
It goes through COND_TX_NOW (handler at 0x0426, like OpenFWWF's `tx_frame_now`),
COND_TX_POWER (0x0742, like `tx_infos_update`, which clears the SPR_BRC
flags) and COND_TX_DONE (0x079E). Only then does the suspend complete. Timing in `11`.

On 0x310 the same path is at 0x0B6A, after UCODESTAT = 3:

```
0B6A je   [0x5C],0x0, 0B78         ; no CTS duration -> MAC_SUSPENDED
0B6B calls 013E                    ; r18 = 0x31, frame control 0x00C4 (CTS)
0B6C sprE7 = [0x5C]                ; the duration
0B6E [0x5C] = 0                    ; one shot
0B70 spr323 = 14 << 5 | [0x49F] & 0x1F    ; L-SIG: 14 bytes, rate of [0x49F]
0B71 spr324 = [0x4A0]
0B72 spr86 = [0x4C] with bits 1:0 = 1     ; PHY control word 0, OFDM
0B73 spr8A = [0x4A5]                      ; PHY control word 1
0B76 SPR_TXE0_CTL = 0x4001
```

The initvals put 0x01CB at [0x49F] (6 Mb/s), 0 at [0x4A0], 0x01C4 at
[0x4C]. Byte offsets 0x093E/0x0940/0x094A are 7.14 cells of the MAC config
block, which 784 keeps 0x58 bytes lower: written at the 7.14 offsets they
give this CTS an L-SIG with no valid rate and a PHY TX error.

## Boot: the SHM clear

Right after the jump to init the ucode clears SHM from the top, three
instructions per word, unless bit 5 of MACCMD (SPR 0x047) is set:

```
0CD0 jnzx 0,5,spr47,0x0, 0CD5     ; DSL-3580_EU; 0x0F58 D6220, 0x0F59 vd625
0CD1 spr65 = 0xBFF                ; 0xC7F on the 0x3A0 family
0CD2 [0x00,off5] = 0              ; loop until spr65 < 0
```

In brcmsmac that bit is `MCMD_SKIP_SHMINIT`, commented "only used for
simulation". In agcombo's bus capture the host does not write MACCMD before
PSM_RUN, so on the hardware the clear does happen: it is what makes the
boot last about 9700 instructions (`11`).

## TBTT and beacon

The path is the same in both families, at different addresses:

| step | DSL-3580_EU (0x310) | D6220 (0x3A0) |
|---|---|---|
| test of COND_TX_TBTTEXPIRE (0x2C) in EOI form, `jnext 0xAC` | 0x00D1 | 0x00DE |
| SPR_BRC bit 12 = 1 (TBTT to serve) | 0x00D2 | 0x00DF |
| dispatch on bit 12, `jext 0x4C` | 0x00E8 | 0x00F8 |
| `jnand 0xE3, spr48` → main loop if SPR_BRC has work in progress | 0x0163 | 0x0178 |
| load the beacon phyctl from SHM bytes 0x00CC/0x00CE/0x00D0 | 0x016B | 0x0188 |
| IRQ TBTT_INDI (0x4) | 0x017F | 0x019B |
| SPR_TXE0_CTL = 0x4D95 | 0x018B | 0x01A7 |
| SPR_TX_Serial_Control = 0x8000 | 0x0462 | 0x0542 |
| IRQ BEACON_TX_OK (0x8) | 0x046F | 0x054F |

On 0x310, between the dispatch and the phyctl:

```
0163 jnand 0xE3,spr48, L0          ; work in progress -> retry from the main loop
0164 jnext 0x3D, 0168              ; 0x3D low -> beacon
0165 jext  0x3E, 0168              ; 0x3D high and 0x3E high -> beacon
0166 calls 0197                    ; otherwise a TBTT without beacon: TBTT_INDI,
                                   ; DTIM counter, SPR_BRC bit 12 = 0
0168 jnzx 0,2,[0x2F],0x0, 016A     ; HOSTF1 bit 2 -> beacon regardless
0169 jzx  1,0,spr47,0x0, 0166      ; no valid bit in MACCMD -> no beacon
016A calls ...
016B spr86 = [0x66]; spr8A = [0x67]; spr8B = [0x68]
016E r18 = 0x20                    ; the beacon's internal frame code
```

- The TBTT selector was found with `condfuzz.py` (`08`): from an idle main
  loop with the MAC enabled and `MACCMD = 0x3`, 0x2C is the only condition
  that leads to the beacon block on both families. 0x4C follows it because
  it is bit 12 of SPR_BRC, which the 0x2C handler sets. The name
  COND_TX_TBTTEXPIRE is OpenFWWF's, HIGH CONFIDENCE.
- The DTIM period comes from SHM word 0x09 (byte 0x0012,
  `B43_SHM_SH_DTIMPER`).
- 0x3D and 0x3E are bits 13 and 14 of the PHY condition register (3), with
  no name in `cond.inc`: their meaning is OPEN.
- At 0x0171 the block puts 1 in bits 2:0 of SPR_BRC: bit 0 is
  COND_NEED_BEACON in OpenFWWF, consistent with its use here. Bit 12, which
  OpenFWWF calls COND_PROBE_RESP_LOADED, marks the TBTT to serve here
  instead: for corerev 42 the names of register 4 have to be checked one by
  one.
- `r18 = 0x20` is assigned only at 0x016E on 0x310: it identifies the beacon
  in the TX fault copy too (below).
- With the model of `08` (TSF at 80 or 200 MHz, a 100 TU interval
  programmed, `--tx-engine 20,10,30`) both families send a beacon at every
  TBTT. Before the TX engine also started on the 0x8000 value of SPR 0x320,
  the first beacon never completed, SPR_BRC stayed at 0x21 and the test at
  0x0163 sent every later TBTT back to the main loop.

## PHY TX error and underflow

The main loop handles COND_TX_PHYERR (0x2D) and COND_TX_UNDERFLOW (0x2B, in
the EOI form 0xAB) before the TBTT. The names are those of OpenFWWF's
`cond.inc` (bits 13 and 11 of the TX condition register), HIGH CONFIDENCE.

| | DSL-3580_EU (0x310) | D6220 (0x3A0) |
|---|---|---|
| `jext 0xAB` → underflow handler | 0x00B2 → 0x0B21 | 0x00C2 → 0x0D53 |
| `jext 0x2D` → PHY error handler | 0x00B4 → 0x0B1C | 0x00C4 → 0x0D4E |
| copy to SHM | 0x0B3E–0x0B4B | 0x0D70–0x0D7D |

No instruction of the two blobs sets bit 11 of `GEN_IRQ_REASON`
(`B43_IRQ_PHY_TXERR`, 0x800): that interrupt comes from the hardware, not
from the ucode.

On 0x310:

- **PHY error** (0x0B1C): with SPR_BRC bit 5 it goes back to the main loop;
  otherwise it increments [0x7F];
- **underflow** (0x0B21): sets bit 8 of SPR_BRC (COND_TX_ERROR for OpenFWWF);
  with a frame in progress (SPR_BRC & 7) it increments [0x7E], otherwise it
  increments a counter indexed by [0x18] and, if the frame comes from the
  queue and has no reason yet, puts 6 (underflow, like brcmsmac's
  `TX_STATUS_SUPR_UF` and b43's `B43_TXST_SUPP_UNDER`) in bits 7:4 of
  [0x09,off0];
- **both** join at 0x0B39: they read PHY register 0x007 through SPR
  0x018/0x019, or `0x007 | [0x55]` if the frame is CCK (bits 1:0 of
  SPR_TXE0_PHY_CTL clear); if the latch is clear they copy the state to SHM
  and set the latch; they write 0xFFFF to both registers, which clears them:
  from the host, after the interrupt, the register no longer says anything.

So the copy alone does not tell a PHY error from an underflow: an earlier
underflow leaves the latch set and the later PHY error is not copied.

The copy, at offsets from the latch:

| byte offset | DSL-3580_EU word | D6220 word | contents |
|---|---|---|---|
| 0x00 | 0xB64 | 0xBFA | latch: 1 after the copy; the host clears it to catch the next fault |
| 0x02 | 0xB65 | 0xBFB | PHY TX error register |
| 0x04..0x08 | 0xB66..0xB68 | 0xBFC..0xBFE | SPR_TXE0_PHY_CTL, `_CTL1`, `0x16` |
| 0x0A..0x16 | 0xB69..0xB6F | 0xBFF..0xC05 | SPR 0x321..0x327, the PLCP |
| 0x1A | 0xB71 | 0xC07 | `r18`, the frame code (0x20 = beacon); in the high byte on 0x3A0 |

On 0x310 word 0xB70, in between, is rewritten with sprE at every end of
transmission (0x07FD) while the latch is clear.

The meaning of the bits of PHY register 0x007 on the AC is OPEN. In this
repository `b43_ac_report_phy_txerr()` (`b43/main.c`) prints the copy when
`B43_IRQ_PHY_TXERR` arrives and clears the latch.

In the model 0x2D, held until an EOI, blocks the beacon, because on 0x310
the EOI of 0x2D (`jext 0xAD`) is only at 0x0609, on a TX path, and not in
the main-loop handler. How the hardware clears it is OPEN.

## TX core selection

For the frames it builds itself (responses, BA, ...) the ucode takes the
core mask from a table in SHM, at 0x0E07 on 0x310 (L798):

| frame type (bits 1:0 of word 0) | cell |
|---|---|
| CCK (0) | 0x2EA (byte 0x05D4) |
| OFDM (1) | 0x2EB (0x05D6) |
| HT (2) | 0x2EC + bits 4:3 of word 2 (MCS / 8) + bit 6 (STBC) |
| VHT (3) | 0x2EC + bits 5:4 of word 2 (NSS − 1) + bit 6 |

The value goes into bits 13:6 of PHY control word 0. For types other than
HT/VHT word 2 is cleared. It is the table b43-ac-wip writes at
0x05D4..0x05DA.

The beacon at 0x016B does not go through this table: it uses the cores
written in its phyctl at 0x00CC.
