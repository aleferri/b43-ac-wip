# PSM interpreter (`interpreter/` in b43-tools)

## Components

- `psm.py`: decoder (a port of b43-dasm, `04`) and executor. Every executed
  instruction is tagged with a confidence level. All state writes go through
  `store()`, with an optional observer; `jext`/`jnext` in EOI form notify an
  `on_eoi` hook.
- `lockstep.py`: co-simulation. Host and ucode share SHM and registers, the
  PSM starts at PC 0 on PSM_RUN and after each host operation runs for
  `--cycles` instructions or, with `--settle`, until it is idle again.
- `condfuzz.py`: brings the co-simulation to a state with a host op stream
  and from there raises one external condition at a time (on top of those
  given with `--with`), reporting which ones lead the ucode to an address or,
  with `--blocking`, which ones keep it away.
- `cosim.py`, `ucode_init.py`, `extcond_scan.py`, `cond.py`: earlier tools,
  updated where touched (`09`).

## Semantics, with their source

| instructions | semantics | source | confidence |
|---|---|---|---|
| add/sub/and/or/xor/sr/sl/rl/rr/nand/mul | op0, op1 → op2 | b43-dasm's decoder | HIGH |
| `orx 7,8` with two immediates | (A << 8) \| B, a case of the general formula | stamping against a real trace | CONFIRMED |
| `orx`/`srx` other parameters | rotated insert / extract from B:A | hypothesis | BEST-EFFORT |
| `jzx`/`jnzx M,S,A,B` | jump if the M+1 bits of A at S are (not) zero | b43-asm's `emulate_jand_insn()` | HIGH |
| `jand`/`jnand` | `jand` jumps if A & B == 0, `jnand` if != 0 | OpenFWWF (5 commented lines and a label), the vendor suspend check, the twin block at 0x03C4 | HIGH |
| je/jne, jl/jge/jg/jle, jls/jges/jgs/jles | equality, unsigned and signed comparisons | b43-asm README; OpenFWWF confirms je/jne, jl/jge/jg, jges/jgs | BEST-EFFORT |
| `jext`/`jnext` | external condition by selector & 0x7F (bit 7 = EOI) | `cond.inc` | model below |
| `calls`/`rets` | one return stack; on arch15 without a link register | b43-dasm | BEST-EFFORT |
| `[x, offN]` | SHM[x + SPR(0x060+N)] | `spr.inc`, use in the blobs | HIGH |

## Hardware model of `lockstep.py`

- **SHM, IHR, MAC.** MMIO 0x400+2n is SPR n; MACCMD is SPR 0x047; IRQ reason
  SPRs 0x042/0x043; the high half of MACCONTROL SPR 0x041. MACCONTROL with a
  mask (vendor captures) updates only the bits given; `MAC.MHF` updates the
  HOSTF1..5 words.
- **PHY.** Commands on SPR 0x018/0x019, completed at once on a register file
  holding what the host wrote and read.
- **CORECTL.** SPR 0x078: bit 13 follows bit 5 (a model of the handshake).
- **initvals.** With `--initvals`, written when the ucode first raises
  MAC_SUSPENDED after PSM_RUN, as in `brcms_b_coreinit`.
- **Conditions.** COND_TRUE (0x7F) true; COND_MACEN (0x24) from
  MACCONTROL.ENABLED; condition register 4 (0x40..0x4F) = SPR_BRC bit by bit,
  as OpenFWWF documents it; the rest false.
- **TX engine** (MODEL, `--tx-engine IFS,START,DONE`): bit 0 of SPR_TXE0_CTL
  rising → COND_TX_NOW after IFS instructions; a write of SPR 0x320
  (SPR_TX_Serial_Control) with bit 15 set → COND_TX_POWER after START and
  COND_TX_DONE after DONE; each event stays raised until the ucode
  acknowledges it with an EOI. Both families write 0x8001 for a queued frame
  and 0x8000 for the beacon; with 0x8001 alone the beacon never completed
  (`07`).
- **TSF** (MODEL, `--psm-mhz F`): SPR_TSF_WORD0..3 (0x119..0x11C) advance by
  1 µs every F instructions, as on a PSM running one instruction per cycle at
  F MHz; neither the clock nor the cycles per instruction are known (`11`).
  It is set by host writes to MMIO 0x632..0x638 or to the 32-bit TSF
  registers 0x180/0x184 (brcmsmac's `tsf_timerlow/high`) and by ucode writes
  to those SPRs. For `--settle` a cycle in which the ucode read the TSF is
  not idle. Without the option the TSF stays still.
- **TBTT** (with the TSF): MMIO 0x188 (`tsf_cfprep`, the interval in µs << 6)
  and 0x18C (`tsf_cfpstart`, the first TBTT), as brcmsmac and b43 program
  them; when the TSF reaches a TBTT, COND_TX_TBTTEXPIRE (0x2C) is raised and
  held until the EOI. A TBTT already in the past moves to the next multiple.
  The stock driver on agcombo writes 0x00640000 and 0x00019000 (100 TU).
- **Time between ops** (`cpuN WAIT us=0x...`): the ucode runs; while it is
  idle the clock jumps to the next TBTT or to the end of the wait.

With `--settle` the report adds the timetable (`11`).

## Checkpoints and validation

- **Stamping**, run from its address on the four blobs: the expected
  UCODEREV and UCODEPATCH (`03`), 4/4.
- **From PC 0** the boot reaches the self-suspend by itself and, with an
  enable, the main loop on all four blobs.
- **Against the captures** (D6220 cold sweep, 44 captures; tg789vac-v2 cold
  sweep, 44 captures), with `--settle`, the initvals and `--tx-engine
  20,10,30`, comparing every host SHM read after the initvals with the
  emulated value:

| | D6220 | tg789vac_v2 |
|---|---|---|
| MAC disables / MAC_SUSPENDED raised | 20,664 / 20,707 | 53,316 / 53,359 |
| reads of words the ucode wrote, matching | 88 / 88 | 172 / 172 |
| reads of words the host or the initvals wrote, matching | 41,854 / 49,929 | 377,677 / 512,533 |
| words the ucode never writes in the model that differ from the trace | 25 | 27 |

The words the ucode writes and the host reads are UCODEREV, UCODEPATCH and
UCODESTAT (ACTIVE after every enable): they all match. Without
`--tx-engine`, on D6220 32 reads of UCODESTAT did not match, because the
disable with a frame to send never completed. The words that still differ
are listed in `10`: counters, noise measurements and events the ucode only
writes on paths that need hardware the model does not have.

The vd625-agcombo and rxtx captures have no read values
(`val=UNDEFINED`), so they give no comparison; agcombo also has
`sel=0x0000` on the writes.

## Fixes with respect to the previous revision

- `jzx`/`jnzx`: in the git history the error of commit `3cb73ee` was M and S
  swapped; the second operand is always the immediate 0 in the blobs, so an
  op0:op1 combination would never have changed a result.
- `orx` with two immediates ignored M and S (18 instructions in D6220).
- `jand`/`jnand` were inverted: that is why the MAC did not suspend.
- The stall from PC 0 was the polling of bit 14 of SPR 0x018 (the PHY
  interface), not a `jext`/`jnext`; `lockstep.py` resolves it with the PHY
  model.
- `lockstep.py`'s parser skipped every line of the vendor captures
  (timestamp and op number first), so the PSM never started; a masked
  `MAC.MCTRL` was taken as the whole value and `gpio_init`'s first update
  cleared PSM_RUN; nothing compared the words the ucode wrote with the
  oracle's reads (now `handoffs` in the report).
- On 0x310 the disable with a frame to send stopped at 0x066C: it is a wait
  on the TSF (`sub spr119, [0x81A]`, `jl r37, 0x8`) that never ended with
  the TSF still, and `--settle` took it for idleness.

## The beacon in the model

With boot, initvals and bsinitvals, a 100 TU interval in 0x188/0x18C, the
MAC enabled, `MACCMD = 0x3` (both templates valid) and `WAIT us=0x60000`
(393 ms), with `--settle 300000 --tx-engine 20,10,30 --psm-mhz 80` (and
200):

| | TBTTs | beacon block run | TBTT_INDI / BEACON_TX_OK | SPR_BRC at the end |
|---|---|---|---|---|
| DSL-3580_EU | 3 | 3 | 3 / 3 | 0 |
| D6220 | 3 | 3 | 3 / 3 | 0 |
| vd625 | 3 | 3 | 3 / 3 | 0 |

The report on D6220's cold01, cold09, cold23 and cold40 without
`--psm-mhz` is identical to that of `c3812d9`; with `--psm-mhz` 80 or 200 it
differs only in no longer listing SPR_TSF_WORD0/1 among the SPRs read before
anyone wrote them.
