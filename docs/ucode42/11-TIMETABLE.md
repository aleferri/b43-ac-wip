# Host ↔ ucode timetable, in PSM instructions

Measured with `lockstep.py --settle`: after each host operation the PSM runs
until it is back at a point it already visited with no net state change.
The number is how many instructions the ucode needs to finish reacting; the
`+N` offsets count from the host operation. They hold within these limits:

- PHY transactions (SPR 0x018/0x019) complete at once, so the code that
  waits for them counts as a lower bound;
- the TX engine delays are model parameters (`--tx-engine IFS,START,DONE`,
  here 20,10,30): the rows that depend on them say so;
- the TSF advances only with `--psm-mhz` (`08`); without it, a wait on the
  TSF never ends.

## Boot: `MAC.MCTRL` 0x04000404 then 0x04020402

| event | 0x3A0 (D6220, tg789vac_v2, vd625) | DSL-3580_EU |
|---|---|---|
| jump to init | +2 | +2 |
| SHM clear, 3 instructions per word from the top | word *w* at +3·(0xC7F−*w*)+7, done at +9604 | +3·(0xBFF−*w*)+7, done at +9220 |
| UCODESTAT = 1 (INIT) | +9614 | +9229 |
| UCODEREV / UCODEPATCH | +9633 / +9634 | +9248 / +9249 |
| UCODESTAT = 3 (SUSP) | +9691 | +9272 |
| IRQ bit 0, MAC_SUSPENDED | **+9693** | **+9274** |
| stopped in the wait for COND_MACEN | +9704 | +9281 |
| PHY transactions in the boot | 7 | 1 |

The initvals must be written after MAC_SUSPENDED (`02`): a host SHM write
made before the clear has passed that word is lost.

## MAC enable and disable

| operation | D6220 / tg789vac_v2 | vd625 | DSL-3580_EU |
|---|---|---|---|
| ENABLED = 1: UCODESTAT = 2 | +4 | +4 | +4 |
| ENABLED = 1: stopped on the main-loop `nap` | +156/157 | +165 | +147 |
| ENABLED = 0: MAC_SUSPENDED | **+39** | **+47** | **+35** |
| ENABLED = 0: stopped in the wait for COND_MACEN | +50 | +58 | +42 |

On D6220 the same values appear at every enable and disable of the cold
sweep captures (20,664 disables in 44 captures with the TX model).

## Disable with a frame to send (0x3A0)

Host: `OBJ.WR` 0x00B8 = 0x7148, then ENABLED = 0. With `--tx-engine
20,10,30`:

| event | D6220 / tg789vac_v2 |
|---|---|
| UCODESTAT = 3 | +37 |
| SPR_TXE0_CTL = 0x4001 | +74 |
| COND_TX_NOW (model: IFS after TXE0_CTL) | +94 |
| SPR 0x320 = 0x8001 (frame ready) | +176 |
| COND_TX_POWER (model) | +186 |
| COND_TX_DONE (model) | +206 |
| TX engine stopped, then MAC_SUSPENDED | +334 |
| stopped in the wait for COND_MACEN | +345/346 |

vd625, same sequence: COND_TX_NOW +102, COND_TX_POWER +202, COND_TX_DONE
+222, MAC_SUSPENDED +374, stopped at +385.

With 5,5,10 on D6220 the result is the same (the latency is dominated by the
main-loop round), with 200,100,400 and 1000,500,2000 it grows, but the frame
and the suspend always complete and UCODESTAT comes back with the value the
host reads in the capture.

On DSL-3580_EU, same synthetic sequence, the ucode waits at 0x066C for 8 µs
to have passed since the TSF saved at 0x0654: without a TSF it does not
reach the suspend. With `--psm-mhz` it does:

| event | 80 MHz | 200 MHz |
|---|---|---|
| MAC_SUSPENDED | +917 | +1720 |
| stopped in the wait for COND_MACEN | +924 | +1727 |

The difference between the columns, 803 instructions, is of the same order
as the 960 that 8 µs are worth more at 200 MHz than at 80: the wait starts
from a TSF saved shortly before. Whether 0x00B8 plays the same role in the
0x310 family as in the 0x3A0 one is not checked.

## TSF and PSM clock

- **The TSF counts microseconds.** In agcombo's bus capture (ch36) the reads
  of MMIO 0x180 (`tsf_timerlow`) at #146108 and #146130 differ by 102,409,
  102 ms apart by the capture's clock; the stock driver programs
  `tsf_cfprep` = 0x00640000 and `tsf_cfpstart` = 0x00019000, that is 100 TU
  in µs.
- **The clock cannot be told well from the captures.** The only event with a
  known instruction count that the host sees is the boot: after `PSM_RUN`
  (#12179) the host reads `GEN_IRQ_REASON` three times and sees
  MAC_SUSPENDED on the third (#12182), after the model's roughly 9700
  instructions. With a 10 µs poll (the Broadcom drivers' `SPINWAIT`, a
  HYPOTHESIS for this binary) and the minimum cost of a traced access (about
  3.5 µs for a write and 5.5 µs for a read, from the busiest milliseconds of
  the capture) the bit would have risen between about 24 and 40 µs after
  `PSM_RUN`, that is a PSM between about 240 and 400 MHz. But between two
  TSF reads the same tracer takes up to about 45 µs per access: the real
  cost of the three accesses is not known and the bound is loose.
- The model therefore uses the 80–200 MHz estimate of research for a
  thesis, as a parameter (`--psm-mhz`), with one instruction per cycle.

## A beacon at every TBTT

Instructions from the moment the model raises COND_TX_TBTTEXPIRE, with
`--tx-engine 20,10,30`. They are the same at 80 and 200 MHz, because between
these events the ucode does not read the TSF. On vd625 the first TBTT, which
finds the main loop at another point, comes 3 instructions later in every
row (in brackets):

| event | DSL-3580_EU | D6220 | vd625 |
|---|---|---|---|
| SPR_BRC bit 12 = 1 | +51 | +56 | +45 (+48) |
| IRQ TBTT_INDI (0x4) | +80 | +89 | +78 (+81) |
| SPR_TXE0_CTL = 0x4D95 | +90 | +99 | +88 (+91) |
| SPR 0x320 = 0x8000 | +205 | +182 | +179 (+182) |
| SPR_BRC bit 12 = 0 | +207 | +184 | +181 (+184) |
| IRQ BEACON_TX_OK (0x8) | +215 | +192 | +189 (+192) |

The 0x8000 comes after COND_TX_NOW, which the model raises 20 instructions
after SPR_TXE0_CTL: those rows depend on the IFS parameter.
