# The b43-tools patches of this revision

The first nine are on `master` of
[aleferri/b43-tools](https://github.com/aleferri/b43-tools) up to
`c3812d9`; the last two (*series 1–2*) are a series to apply on top of
`c3812d9` with `git am`. Each commit message gives the reason and the
checks made.

## Assembler

| commit | what |
|---|---|
| `8273598` | offset registers `off0..off6` encoded as SPRs 0x060..0x066 (spr0C0+N before); 10-bit SPR field on arch15, up to 0x342. Disassembling and reassembling gives back the four blobs, except the 12 `nap` (`04`) |
| `58cd2bb` | `jand` jumps if A & B is zero, `jnand` if it is not, as in OpenFWWF and the vendor suspend check; README corrected, a wide `jand` emitted as `jzx` |

## Interpreter

| commit | what |
|---|---|
| `1929969` | dead decoder paths removed; listing of the four blobs unchanged |
| `81eebb9` | `orx` with two immediates honours M and S (18 instructions in D6220) |
| `27afed7` | every state write goes through `store()`, with an observer |
| `48837a6` | `lockstep.py` reads the vendor captures (timestamp and op number first: every line used to be skipped and the PSM never started), applies `MAC.MCTRL` with its mask (`gpio_init`'s first update used to clear PSM_RUN), writes the initvals at the first MAC_SUSPENDED, `--settle` and the timetable |
| `d2e7919` | `cosim.py`: MACCONTROL with its mask and `MAC.MHF` |
| `96f7866` | `jand`/`jnand` semantics as the hardware runs them (with the previous ones an idle MAC never suspended); EOI selectors by `selector & 0x7F` |
| `c3812d9` | condition register 4 = SPR_BRC bit by bit; the `--tx-engine` TX engine model |
| *series 1* | a running TSF (`--psm-mhz F`), TBTTs from the programmed beacon interval, `WAIT` ops, TX engine started on bit 15 of SPR 0x320 (`08`, `07`) |
| *series 2* | `condfuzz.py`: which conditions lead the ucode to an address (`08`) |
