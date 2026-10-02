# test/d11sim — d11 core simulator and init state-equivalence

A model of the corerev-42 d11 MAC core that runs a driver's init op stream and
the real firmware-42 microcode, so b43 and wl can be compared by **final core
state and RAM** instead of op by op.

Two execution substrates, each doing the part it is good for:

- **host side (C).** `d11_core` models what the bus sees: the MMIO register
  file, the object-memory spaces reached through OBJADDR/OBJDATA (ucode, shared
  memory, scratch, internal-hw, RCMTA) with the real `sel`/autoinc/word-size
  rules, the MACCONTROL latch and the PSM run/suspend handshake. `ops.c`
  replays a decoded op stream into it; `fakephy` absorbs PHY/radio accesses
  nexmon-style so they never reach core state.
- **ucode side (Python).** The shared-memory the microcode writes at init is
  produced by executing the actual blob through b43-tools' arch15 interpreter
  (`interpreter/psm.py`), driven by `ucode_init.py`. Nothing here fabricates
  ucode output: run the extracted D6220 blob and it writes UCODEREV 0x03A0 /
  PATCH 0x2715, the agcombo blob writes 0x03A0 / 0x04B1, each matching that
  board's `wl revinfo`.

`b43_upload_microcode` sets PSM_RUN and polls GEN_IRQ_REASON for
MAC_SUSPENDED; the core model raises that latch, and the interpreter supplies
the revinfo the host then reads back.

## Build

    make                      # builds ./d11sim
    git clone https://github.com/aleferri/b43-tools    # the interpreter tools
    export B43_TOOLS=$PWD/b43-tools                     # used below

The microcode is executed by the arch15 interpreter in **b43-tools**, and the
tools that wrap it — `ucode_init.py`, `cosim.py`, `cond.py`, `extcond_scan.py`
— live there, in `b43-tools/interpreter/`, next to `psm.py`. This directory
keeps only the C core model and the glue that consumes them. `run_init_equiv.sh`
finds b43-tools through `B43_TOOLS` (or `--b43-tools`).

## Pieces

| file | role |
|---|---|
| `d11_core.{c,h}` | corerev-42 core state: registers + object memory + handshake |
| `chip.{c,h}` | ChipCommon, PMU indirect words, PCIe2 (and its config space), agent |
| `fakephy.{c,h}` | fictional PHY/radio behind the PHY/radio ports |
| `psm42.{c,h}` | load the blob into ucode memory; static inventory |
| `ops.{c,h}` | replay a decoded op stream (mmio2ops / trace_out vocabulary) |
| `snapshot.{c,h}` | write the sparse canonical state dump |
| `d11sim.c` | CLI: `replay`, `inventory` |
| `compare_state.py` | diff two state dumps over the host-configuration surface |
| `run_init_equiv.sh` | wire wl and b43 sides into one verdict |
| *(b43-tools/interpreter)* | `ucode_init.py`, `cosim.py`, `cond.py`, `extcond_scan.py` |

## Extract the blob

    ../../reverse-tools/extract_ucode.py wlD6220.o_save --rev 42 -o d11ucode42.bin

A bus capture of the stock `up` carries the same blob, uploaded word by word:
`../../reverse-tools/fw_from_capture.py ch36.bin -o DIR` writes it as
`DIR/d11ucode42.bin`, next to the initvals the b43 side needs (see
`../integration/README.md`).

The blob is derived from a proprietary object; it stays out of the tree
(`.gitignore`), like the `wl*.o_save` in `PROVENANCE.md`.

## Run the wl side

    python3 ../../reverse-tools/mmio2ops.py \
        ../../router-data/agcombo/ch36.bin --no-bulk -o wl.ops
    ./d11sim replay --ops wl.ops --label wl --dump wl.state
    python3 "$B43_TOOLS"/interpreter/ucode_init.py d11ucode42.bin --out wl.ucode

`--no-bulk` matters: a folded `OBJ.BULKW` carries no values to replay, and
`d11sim` warns if it meets one.

## Run the b43 side

b43's op stream comes from `test/integration`, which needs kernel headers:

    cd ../integration && make fetch && make b43-trace
    ./b43-trace ... B43_TRACE_OUT=b43.trace            # see that README
    ./d11sim replay --ops b43.trace --label b43 --dump b43.state

## Compare

    python3 compare_state.py b43.state wl.state \
        --ucode-a b43.ucode --ucode-b wl.ucode --label-a b43 --label-b wl

The verdict is over the host-configuration surface (REG/SHM/SCR/HW/RCMTA).
Cells the ucode wrote (by provenance, from the interpreter dumps), the volatile
IRQ/command registers and the ucode image are reported in their own buckets and
left out of the verdict, since they are not what the driver's init configured.
Exit status is 0 when that surface is identical.

Or all at once:

    ./run_init_equiv.sh --wl-bin ../../router-data/agcombo/ch36.bin \
        --wl-blob d11ucode42_agcombo.bin \
        --b43-trace b43.trace --b43-blob d11ucode42.bin

## External conditions (jext/jnext)

The `jext`/`jnext` selector the interpreter leaves as a stub is, per OpenFWWF's
`cond.inc`, `(condreg << 4) | bit` with bit 7 = EOI; `COND_TRUE = 0x7f`.
`cond.py` parses that file (it is not vendored here) so conditions can be named,
and `ucode_init.py --seed` forces them before a run:

    python3 "$B43_TOOLS"/interpreter/extcond_scan.py d11ucode42.bin            # what it polls, named
    python3 "$B43_TOOLS"/interpreter/ucode_init.py d11ucode42.bin --seed TX.MACEN --seed COND_RX_COMPLETE

Seeding a condition steers the real dispatcher: from the main loop, with
`COND_RX_COMPLETE` forced the ucode enters its RX handler, with `TX.TX_DONE` its
TX-done handler, where unseeded it idles. The names are OpenFWWF's corerev-5
(arch5) names; the selector layout and `COND_TRUE` carry over to rev-42, but a
given FIXME bit is a lead for rev-42, not a certainty.

Run from reset the ucode does not stall in a pre-init wait: it runs its reset
path and settles in the main dispatch loop, idling because nothing asserts a
condition. The version-stamp block (`0xF76`) is reached from a command handler,
not from the free-running loop, which is why `ucode_init.py` starts there.

## Co-simulation against the wl trace

`cosim.py` shares one state between the host op stream and the real ucode: host
writes land in shared memory, scratch, RCMTA, template RAM (via
RAM_CONTROL/RAM_DATA) and the MMIO/IHR register file, and at each host->ucode
hand-off the microcode is run on that shared memory, so it reacts to what the
host set and writes back.

    python3 "$B43_TOOLS"/interpreter/cosim.py wl.ops d11ucode42.bin --out cosim.state

The snapshot covers every cell that interacts with d11, each tagged host or
ucode: SHM, SCR, RCMTA, TPL (template RAM), REG (d11 MMIO/IHR), GPR and SPR (the
PSM's own registers). On the agcombo ch36 capture this is ~546 SHM words, 216
template words, 108 registers, the PSM's version-stamp SPRs, and the real
revinfo the ucode wrote (UCODEREV 0x03A0 / PATCH 0x04B1 for that build).

The boot hand-off runs the validated version-stamp and is the ucode's shared-
memory contribution. The per-command hand-offs (MACCMD BEACONx_VALID, BGNOISE)
run the main loop, but a command the ucode picks up by reading the MAC command
register dispatches through IHR reads the interpreter does not model, so those
handlers do not run; the command is recorded and whatever the host routed
through shared memory for it is still applied. This is the honest boundary, not
a silent gap.

## Scope and limits

- The interpreter's decoder is validated instruction-for-instruction against
  `b43-dasm`; its executor is high-confidence for arithmetic/logic and the
  version-stamp prologue, best-effort for jump conditions, and a stub for the
  `jext`/`jnext` signal values (now seedable by name, above). Running from
  `--start` (default 0xF76) exercises the validated prologue; everything past it
  should be treated as unconfirmed.
- The replay covers the whole captured window; narrowing it to the init phase
  only is still open.
- The host-side b43 stream needs the kernel-header-dependent integration
  harness, which does not build in every environment.
