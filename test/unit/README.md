# test/unit — trace harness for the PHY

Compiles the AC-PHY driver in `../../src/` in userspace and produces a trace in
`wl-diag`'s format, to compare against the stock captures in `../../router-data/`.
It does not modify any file in `src/`.

## Goal

b43 should emit, one by one and in the same order, every operation the stock
driver emits on a cold attach: no missing operations, no extra ones, no wrong
values. The score says how much is left; what is left, with the reason for each
piece, is in `../../docs/retrace-todo.md`.

## The procedure

Every command below is the one the repository uses for the numbers it quotes.
Ad hoc variants give numbers that cannot be compared.

### 1. Prepare the captures

```sh
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
```

**The segments.** The 43 segments are `/tmp/cold/coldNN-chC-bwB.txt`: 25 at
20 MHz, 12 at 40 and 6 at 80. `cold00-preambolo.txt` is the capture's head, not
a segment.

- On the 21 radar-duty segments the availability check completes inside the
  capture.
- The six weather-radar segments (ch120, ch124, ch128 at 20 MHz, ch116 and ch124
  at 40, ch116 at 80) end during the check.

**Two attaches per segment.** `wl` attaches every core at load: first wl0, the
2.4 GHz N-PHY, then wl1, the AC. `strip_other_core.py` removes wl0's attach. The
comparison skips it anyway, because it starts at the AC attach's first
`PHY.RD 0x0739`. The **oracle** needs the strip, because it starts at the insmod
to cover OTP and the core probe. Without it the per-address queues are shifted
and the port reads the other core's shared memory. `gates.sh` does it by itself.

**Folding.** The captures are raw: reads carry `val=UNDEFINED` and the value is
in the following `RETVAL` line. Fold them before grepping for values:

```sh
python3 ../../reverse-tools/trace_filter.py --retvals \
    /tmp/cold/cold01-ch36-bw20.txt /tmp/m01
```

A `grep 'val=0x...'` on an unfolded file finds no reads, and the operation
looks absent.

### 2. Build

```sh
make
```

One build serves every channel and width. If the port emits a few thousand
operations instead of tens of thousands, the flow bailed out early; `gates.sh`
says so.

### 3. The gates

```sh
./gates.sh                                      # cold01 ch36 bw20
./gates.sh /tmp/cold/cold05-ch52-bw20.txt       # another segment
./gates.sh /tmp/cold/cold[0-9][0-9]-ch*.txt     # all 43

unzip -d /tmp/hot ../../router-data/d6220/hot-sweep.zip
./gates.sh --hot                                # three default up segments
./gates.sh --hot --flow switch_channel DIR      # one row per channel
./gates.sh --board tg789 SEGMENT                # another board's profile

AC_READ_ORACLE=../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > /tmp/p.out
python3 compare.py \
    ../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt /tmp/p.out
# must print MATCH
```

For each segment `gates.sh`:

1. strips the other core and folds the capture;
2. takes the comparison window from the attach's first PHY operation;
3. extracts the environment's events after the bring-up with
   `reverse-tools/timeline.py` — watchdog turns, radar polls, template reloads,
   bss-up — plus the watchdog phase and entry-turn shape;
4. takes the reloads on the `conf_tx` passes with `beacon_reloads.py`, and the
   captured `0x00cc` value (`AC_BSS_CC`);
5. exports the regulatory map `AC_MAX_POWER_MAP`;
6. runs the flow with the read oracle;
7. calls `cmp_skip.py` and `compare.py`.

Repeating the steps by hand gets the window wrong.

**Cold and hot are not interchangeable.**

- The cold sweep is all first bring-ups, one module reload per channel. Any
  predicate that tells a first bring-up from a later one is invisible there: a
  missing term reads the same on every segment.
- `--hot` runs the `up` flow with `AC_FIRST_INIT=0` on three segments chosen to
  catch that: ch36 below 5250 MHz, ch52 and ch104 above. If a predicate
  confuses the two conditions, the first stays put and the other two collapse.

Run the periodic gate after **every** change. It is the only position-by-position
comparison at `MATCH`, so it is the most sensitive regression detector there
is.

### 4. Reading the score

```
grezzo          : 29578/30059 = 98.40%   19 regioni
                  2 col valore sbagliato, 269 op di wl mancanti, 208 op del port di troppo
nel perimetro   : ...
CON  eccezioni  : ...
```

The tool prints Italian labels. The line to quote is **`grezzo`** ("raw"). Its
denominator is the union of the two streams, so it reaches 100% only when they
coincide.

The three counts are three different jobs and are not to be added by eye:

| count (label) | meaning | the job |
|---|---|---|
| wrong value (`col valore sbagliato`) | right register, wrong number | a formula to find |
| missing stock ops (`op di wl mancanti`) | the stock driver emits it, the port does not | code to write |
| extra port ops (`op del port di troppo`) | the port emits it, the stock driver does not | a gate to add, or a phase that does not run on the stock driver |

- `nel perimetro` ("within the perimeter") removes operations of code outside
  `src/`.
- `CON eccezioni` ("with exceptions") also applies the `KNOWN` rules of
  `cmp_skip.py`.

Both serve navigation, not scoring.

### 5. Finding the next divergence

```sh
GATE_TMP=/tmp/gate ./gates.sh                   # keeps seg, merged, full, cmp
python3 cmp_skip.py /tmp/gate/merged /tmp/gate/full 167:LAST --verbose
```

`compare.py` is positional. It stops at the first divergence with context, and
`gates.sh` prints its first `@N`. One missing operation shifts everything after
it, so `@N` says where to look, not how much is broken.

`cmp_skip.py --verbose` lists the diverging regions after the exceptions, with
`delete` and `insert` paired when a block has moved. Open it first on a
positional divergence.

To see **who** emits an operation in the port:

```sh
AC_FN_MARKERS=1 AC_CHANNEL=36 AC_BW=20 AC_FIRST_INIT=1 ./ac_trace full d6220
```

It annotates the output with nested `----FN:name----` / `----/FN:name----`
pairs. Attribute with the stack, not with the entry marker alone.

`GATE_TMP` is rewritten on every run. Analysing it after running the gate on
**another** segment reads the wrong segment's files.

### 6. Before saying a phase is absent on the stock driver

You need a **witness**: a register or table that, in the port, only that
function touches. Count it on every segment, not on one:

```sh
for s in /tmp/cold/cold[0-9][0-9]-ch*.txt; do
    printf '%-24s %s\n' "$(basename $s)" "$(grep -c 'addr=0x0380' $s)"
done
```

- **A witness at zero** proves the register absent, not the phase.
  `reverse-tools/phase_absent.py` checks every address of the phase, with the
  table as identity for work that goes through the data port.
- **Phases without a witness.** The method finds only phases that have an
  exclusive witness. A phase that shares all its registers cannot be seen this
  way; say so instead of concluding it is not there.
- **Traced classes only.** An absence counts only if the capture traces that
  class (`../../router-data/CLASS-COVERAGE.md`).

## The exception lists

They are in `compare.py`, each entry with its reason. They are the only place
where an operation is declared not to count, and every entry suspends a piece of
the goal, so keep them short and argued.

### `SOLO_PORT` — port operations the oracle cannot contain

There are two legitimate cases, kept in two lists:

- **`SOLO_PORT`** holds operations b43 must do by its structure where `wl` does
  something else. They are permanent:
  - the SHM cells `0x0004`/`0x0006`;
  - the self-test patterns on `0x0000`/`0x0002`;
  - `REG.WR 0x49c` (`GPIO_CONTROL`, how `leds.c` drives the LEDs from the MAC).
- **`SOLO_PORT_SENZA_CLASSE`** holds correct operations whose counterpart the
  capture does not trace. They apply only against a capture without that class,
  and switch off by themselves when the vendor side carries it. Today:
  `AMT.*`, active on the agcombo sweeps.

Anything else is the port doing too much, and is fixed in the driver.

### `VAL_NONDET` — values nobody can predict

**BSLOTS** and **REGGAP** of the four EDCFQ blocks:

- BSLOTS is the random backoff drawn at the start of the contention window,
  uniform in `[0, CWMIN]` over 43 segments and four queues;
- REGGAP is `AIFS + BSLOTS` on all 172 points.

Address, class and position are compared, the value is not.

The bar is strict: nondeterministic **by construction**, not merely unknown. A
value that is not understood goes into the driver with `b43_phy_ac_todo()`, not
here.

### `VAL_TOLLERANZA` — values compared with a tolerance

PHY `0x?a1`, the RX IQ `b` coefficient, at ±1 LSB on 10-bit two's complement.
The tolerance equals the maximum residual of the model believed correct.

The tolerance applies to the positional gate, **not** to the score:
`cmp_skip.py` still counts those operations as wrong values, so the residual
stays visible.

### `SOLO_VENDOR` — operations no b43 code can emit

- **`MAC.BW`**: `wlc_bmac_bw_set` only sets state. In b43 the width lives in
  `phy.chandef`, and there is no register to write.
- **`MARK`**: labels written by the capture script and the tracer.
- **`IOCTL`/`IOVAR.SET`**: commands from userspace, inputs to the driver rather
  than its operations.
- **The probe-response offload**: `0x0048`, `0x004a`, `0x0180`–`0x0186`, the
  SSID at `0x0160`–`0x017e`, and `TPL.RAMW 0x0700`. This is a WIP choice, not a
  limit; see `../../docs/retrace-todo.md`.

Each entry declares a piece of the goal unreachable, so it needs proof that
there is nothing to emit.

### `PERIMETER` — operations of code outside `src/`

- MAC shared memory, as listed in `CORE_SHM`;
- template RAM `0x0048`;
- `MAC.MHF.RD`, OTP and SROM control, `CAL.INIT`;
- the LEDs on the chipcommon — `GPIO.*` with the board masks and
  `gpiotimeroutmask` at `0x8c`, which are `wlc_bmac_hw_up`/`wlc_bmac_led`/
  `wlc_bmac_led_hw_deinit` in the blob and `leds.c` in b43.

The criterion is ownership shown by `b43.h` or the blob, **not** reachability
from `src/`. Reachability would be degenerate: removing code would raise the
score.

**Narrow it every time the port learns to write a cell.** The perimeter drops
operations from the vendor side only, so a cell the port emits and the perimeter
drops becomes an insertion with no counterpart. `PHY_ANCHE` lists the core cells
the port does emit.

## How the harness works

- **No `#ifdef` in `src/`.** Every hardware accessor (`b43_phy_*`,
  `b43_radio_*`, `b43_read16`/`write16`, `b43_actab_*`, `b43_mac_*`) is
  intercepted at link time with `-Wl,--wrap=<sym>`. The list is `WRAP_SYMS` in
  the `Makefile`. A new driver helper that touches hardware needs a line there,
  or its operations disappear from the trace.
- **`wrap.c`** provides `__wrap_<sym>`. It emits a wl-diag line, updates an
  in-process mirror on writes and returns a value on reads.
  - Reads are served from the oracle (`AC_READ_ORACLE`), then any registered
    read plan, then the mirror.
  - Table cells have their own oracle and mirror keyed by `(id, offset)`,
    because every cell goes through the same data port.
- **`main.c`** builds a fake `struct b43_wldev` from a board profile — `d6220`
  (default), `dsl`, `agcombo`, `tg789` — then registers read plans and runs one
  of the flows.
- **`stubs/`** holds the minimal kernel and b43 headers needed to compile `src/`
  without a kernel tree.
- **`test_harness.h`** is the framework API, included only by `main.c` and
  `wrap.c`. Code in `src/` does not see the framework.

### Flows

| flow | what it does |
|---|---|
| `full` | the complete cold attach and bring-up, then the post-bring-up events from `AC_TIMELINE`; use it against a cold segment |
| `up` | a bring-up on an interface that was up (use with `AC_FIRST_INIT=0` against a hot `up` segment) |
| `periodic` | one watchdog turn, against the steady-tick oracle |
| `switch_channel` | one warm cycle, one row per channel with `gates.sh --hot --flow switch_channel` |
| `down`, `op_init`, `rfkill`, `crsmin` | pieces, for targeted work |
| `rxiq_est_debug`, `rxiq_comp` | the RX IQ estimator/solver alone |
| `led_pins` | prints the LED pins of the board profile (used by `gates.sh`) |

A cold segment is a whole attach: use `full`, not `switch_channel`.

### Core mirrors in `main.c`

Some operations the stock driver emits live in b43's core, which the harness
does not compile. Where the comparison needs them in place they are mirrored as
`emit_core_*()` functions:

- chip-init cells, host flags and the station MAC;
- the AMT, the BSS configuration and SSID, and the `conf_tx` passes;
- the core-init tail (`b43_upload_card_macaddress()` and `b43_security_init()`).

Their values come from the corresponding patch, so if the patch changes, the
mirror becomes wrong and the comparison says so.

**Order matters more than value**: a single inversion drops the operation from
the comparison.

### Events after the bring-up

After the channel switch the stock driver reacts to events: the 1 s watchdog,
the 150 ms radar timer, template reloads by the core, the bss-up that closes
the availability check. `timeline.py` lists them with their instants (`WD`,
`POLL`, `TPL`, `BSS_UP`), and `run_timeline()` in `main.c` delivers them to
`b43_phy_ac_watchdog()`, `b43_phy_ac_radar_poll()`, `b43_ac_beacon_reload()`
and `b43_phy_ac_bss_up()`.

What the driver does on each event is the driver's. No list of ticks crosses
into `src/`.

The only state carried in from before the segment is:

- `AC_WD_PHASE`, the phase of the watchdog's ten-turn counter;
- `AC_WD_ENTRY_TURN`, whether the first turn is a full one or only the counter
  sweep, which depends on when the timer expires.

The consequence for reading the score: the gate measures that these entry
points **emit** the right operations, not that b43 calls them. On hardware they
are not wired yet; `test/integration` is the tool that shows what b43 actually
calls.

## Read plans and the oracle

For reads the driver consumes, the harness serves scripted values instead of
the mirror:

- **`AC_READ_ORACLE=<capture>`** serves values from the capture itself, in the
  order they appear. This is the canonical mode and the one `gates.sh` uses.
  `AC_READ_ORACLE_FROM=<episode>` moves the starting point.
- **`b43_test_plan_*_reads()` in `main.c`** is for targeted cases without an
  oracle. Hand-written plans have a structural flaw: a wrong plan is not caught,
  because the output is compared, not the input. New flows should use the
  oracle.

At the end of a run the oracle prints a summary line on stderr. Read it this
way:

- **exhausted queues** must be **zero**. Otherwise the port read an address more
  times than the capture did, and from that point on the oracle serves values of
  another read;
- **reads with no entry** is not zero, and need not be. Table words are served
  from the `(id, offset)` oracle, so every data-port read under a `TBL.RD`
  marker counts here even when the table is served. If the count is not
  explained by the data port (`grep -c 'PHY\.RD   addr=0x000f'` plus
  `0x0011`), the port read an address the capture does not have;
- **not fully consumed** is not zero and not a defect: the oracle loads every
  address the capture reads, including the core's.

Explicit read plans must show `iter=N/N`: an underrun means the flow ended
early, an overrun that it ran longer than expected.

## What the harness does NOT simulate

- **Dynamic hardware.** A read returns the last write or the oracle's value. No
  read-only bits respond to stimuli.
- **Timing.** `udelay`/`msleep` are no-ops. The order is kept, the real windows
  are not.
- **MAC races.** The `b43_mac_*` calls are traced, not executed.
- **Scheduling.** The harness is single-threaded, so the capture's `cpuN` column
  is normalised away.

## Coverage per function

```sh
AC_FN_MARKERS=1 ./ac_trace full d6220 > /tmp/annotated.txt
python3 ../../reverse-tools/fn_map.py coverage /tmp/annotated.txt \
    /tmp/cold/cold01-ch36-bw20.txt
```

Coverage is measured against the **raw** capture, not the folded one: the
markers align on episodes.
