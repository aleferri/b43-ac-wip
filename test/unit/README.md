# test/unit — trace harness for the PHY

Compiles the AC-PHY files of `../../b43/` in userspace and produces a trace in
`wl-diag`'s format, to compare against the stock captures in
`../../router-data/`. The goal is that the port emits, in the same order, every
operation the stock driver emits on a cold attach: no missing, no extra, no
wrong values. What is left is in `../../docs/retrace-todo.md`.

## Running the gates

```sh
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
unzip -d /tmp/hot  ../../router-data/d6220/hot-sweep.zip
make

./gates.sh                                      # cold01 ch36 bw20
./gates.sh /tmp/cold/cold05-ch52-bw20.txt       # another segment
./gates.sh /tmp/cold/cold[0-9][0-9]-ch*.txt     # all 43
./gates.sh --hot                                # three default up segments
./gates.sh --hot --flow switch_channel DIR      # one row per channel
./gates.sh --board tg789 SEGMENT                # another board's profile

AC_READ_ORACLE=../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > /tmp/p.out
python3 compare.py \
    ../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt /tmp/p.out
# must print MATCH
```

These are the commands behind the quoted numbers; ad hoc variants give numbers
that cannot be compared. `gates.sh` strips the other core's attach, folds the
capture, takes the window from the attach's first PHY operation, extracts the
post-bring-up events and the environment (`timeline.py`, `beacon_reloads.py`,
`AC_BSS_CC`, `AC_MAX_POWER_MAP`), runs the flow with the read oracle and calls
`cmp_skip.py` and `compare.py`. The head of the script says why each step is
where it is.

`./band_gate.sh` (`BOARD=archer` for the SROM-derived values) compares the
port's 2.4 GHz delta with the one the MacBookAir6,1 and the archer-t5e share;
see `../../docs/retrace-todo.md`, "2.4 GHz".

Run the periodic gate after **every** change: it is the only
position-by-position comparison at `MATCH`, the most sensitive regression
detector there is.

**Cold and hot are not interchangeable.** A predicate that tells a first
bring-up from a later one reads the same on every cold segment. `--hot` runs
the `up` flow with `AC_FIRST_INIT=0` on ch36 (below 5250 MHz), ch52 and ch104
(above): if a predicate confuses the two, the first stays put and the others
collapse.

**The segments.** 43 cold segments, 25 at 20 MHz, 12 at 40, 6 at 80;
`cold00-preambolo.txt` is the capture's head. Every segment starts with wl0's
attach, which `strip_other_core.py` removes; the oracle needs it gone because
it starts at the insmod. The captures are raw, so fold them before grepping for
values (`reverse-tools/trace_filter.py --retvals`), or every read looks absent.

## Reading the score

```
grezzo          : 29776/29861 = 99.72%   18 regioni
                  2 col valore sbagliato, 71 op di wl mancanti, 10 op del port di troppo
```

The line to quote is `grezzo` (raw). Its denominator is the union of the two
streams, so it reaches 100% only when they coincide.

Ops are matched in order, in blocks: `tracelib.align_opcodes()` anchors the
two streams on runs of eight ops found in both, extends each anchor while the
ops agree and searches the gaps between anchors the same way. A block of one
op is not a match -- one op equal to one op says nothing about where the two
streams are -- so it counts as missing on one side and extra on the other. The three counts are three
different jobs:

| count | meaning | the job |
|---|---|---|
| `col valore sbagliato` | right register, wrong number | a formula to find |
| `op di wl mancanti` | the stock driver emits it, the port does not | code to write |
| `op del port di troppo` | the port emits it, the stock driver does not | a gate to add |

`nel perimetro` and `CON eccezioni` also drop the code outside the PHY and the
`KNOWN` rules; they serve navigation, not scoring.

## Finding the next divergence

```sh
GATE_TMP=/tmp/gate ./gates.sh                   # keeps seg, merged, full, cmp
python3 cmp_skip.py /tmp/gate/merged /tmp/gate/full 167:LAST --verbose
AC_FN_MARKERS=1 AC_CHANNEL=36 AC_BW=20 AC_FIRST_INIT=1 ./ac_trace full d6220
```

`compare.py` is positional and stops at the first divergence (`@N`): one
missing operation shifts everything after it, so `@N` says where to look, not
how much is broken. `cmp_skip.py --verbose` lists the regions, pairing `delete`
and `insert` when a block has moved. `AC_FN_MARKERS=1` annotates the port's
output with `----FN:name----` pairs; attribute with the stack, not the entry
marker. `GATE_TMP` is rewritten on every run.

**Before saying a phase is absent** on the stock driver you need a witness, a
register only that function touches, counted on every segment, and the class
must be traced in the capture (`../../router-data/CLASS-COVERAGE.md`). A witness
at zero proves the register absent, not the phase;
`reverse-tools/phase_absent.py` checks every address of the phase.

## Exception lists

`compare.py` holds `SOLO_VENDOR`, `SOLO_PORT`, `SOLO_PORT_SENZA_CLASSE`,
`PERIMETER`, `VAL_NONDET` and `VAL_TOLLERANZA`, and `cmp_skip.py` holds `MOVED`
(empty: see the `UNIT_TEST` sites below) and `KNOWN`, each entry with its
reason. They are the only place where an
operation is declared not to count, and every entry suspends a piece of the
goal:

- a value goes in `VAL_NONDET` only if it is nondeterministic by construction
  (the EDCF backoff); a value not understood goes in the driver with
  `b43_phy_ac_todo()`;
- `PERIMETER` is decided by ownership shown by `b43.h` or the blob, not by
  reachability from the PHY, and is narrowed every time the port learns to write
  a cell (`PHY_ANCHE` lists the core cells the port emits);
- a `VAL_TOLLERANZA` applies to the positional gate only; `cmp_skip.py` still
  counts the value as wrong.

## How the harness works

- **Accessors by link, not by `#ifdef`.** Every hardware accessor is
  intercepted at link time with `-Wl,--wrap=<sym>` (`WRAP_SYMS` in the
  `Makefile`). A new driver helper that touches hardware needs a line there, or
  its operations vanish.
- **Two defines.** `-DALLOW_24=true`, temporary, lifts `switch_channel`'s
  refusal of 2.4 GHz; `AC_CHANNEL` 1 to 14 is that band. `-DUNIT_TEST=1`
  compiles the `b43_phy_ac_core_site()` calls in the PHY: the points where the
  stock driver runs core work inside a PHY sequence -- the address match
  rows, the beacon reloads, the gate of the availability check -- which b43
  runs from the core at another time. `main.c` emits the core's operations
  there, so the positional gate keeps the stock order. A site is only for work
  that is known and belongs to the core; what is not understood stays out.
- **`wrap.c`** emits a wl-diag line per access, keeps a mirror of the writes and
  serves reads from the oracle, then any read plan, then the mirror. Table
  cells have their own oracle and mirror keyed by `(id, offset)`.
- **`main.c`** builds a fake `struct b43_wldev` from a board profile
  (`../board_profile.h`: `d6220`, `dsl`, `agcombo`, `tg789`, and `archer`, the
  one dual-band SROM) and runs a flow:
  `full` (cold attach, bring-up and the timeline, against a cold segment),
  `up` (with `AC_FIRST_INIT=0`, against a hot segment), `periodic`,
  `switch_channel`, and pieces (`down`, `op_init`, `rfkill`, `crsmin`,
  `rxiq_est_debug`, `rxiq_comp`, `led_pins`).
- **Core mirrors.** What the core emits and the comparison needs in place is
  mirrored as `emit_core_*()` in `main.c`, with values from the corresponding
  patch: if the patch changes, the mirror is wrong and the comparison says so.
- **Events.** `run_timeline()` delivers the capture's `WD`, `POLL`, `TPL`,
  `BSS_UP` and `NOISE` events to `pwork_1sec`, `b43_phy_ac_radar_poll()`, the
  core's beacon reload mirror, the core's gate and
  `b43_phy_ac_noise_sample_done()`. The only state carried in from before the
  segment is `AC_WD_PHASE` and `AC_WD_ENTRY_TURN`. The gate measures what
  these entry points emit; which ones b43 calls is `../integration`'s job.

## The read oracle

`AC_READ_ORACLE=<capture>` serves reads from the capture in order, per address;
it is what `gates.sh` uses. Hand-written read plans (`b43_test_plan_*_reads()`)
are for targeted flows only, since a wrong plan is not caught. The summary on
stderr: **exhausted queues must be zero** (otherwise the port read an address
more often than the capture and every later value is shifted); reads with no
entry and queues not fully consumed are expected, from the table data port and
the core's reads.

The harness does not simulate dynamic hardware, timing (`udelay` is a no-op),
MAC races or scheduling.
