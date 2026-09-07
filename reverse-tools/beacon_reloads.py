#!/usr/bin/env python3
"""Beacon-reload schedule of a capture, read off its timestamps.

Every so often the stack above the driver republishes the beacon, and the
driver reloads the template: 0x00cc read and echoed, TIMBPOS, the template
word in template RAM, the length in BTL0 or BTL1, then a pass over the
probe-response PLCP. The length is not a constant: it is 0x012a at 20 MHz and
something else at 40 and 80, so only the cell is matched here and not its
value. `wl` alternates beacon0 and beacon1, starting on beacon0.

How many times it happens is not the driver's to decide, and the captures show
it: on the 26 cold segments of one board the count runs from 7 to 21, on the
same tree, with no relation to segment length -- they all sit between 33 and 39
seconds. It is the number of times hostapd pushed a beacon while the tracer was
running. So it goes the same way AC_PROBE_TICKS and AC_WATCHDOG_TICKS go: out of
the capture and in through the caller, because the harness has no stack above
it.

Which reloads are these is a structural question, not a positional one. The two
anchored to b43_op_config() and the three of the conf_tx passes carry no
mac_suspend between the length and the template; the later ones do, and that is
what this looks for.

Placement reuses probe_schedule.py's tick grid, so a reload lands on the tick of
the probe group that follows it. Reloads before the phase starts are counted
separately: they run before the loop, not on a tick of it.

  ./beacon_reloads.py <capture> [--sh]

--sh emits `AC_BEACON_RELOADS=<pre>:<tick>,<tick>,..`, ready to prefix a
command. Exit status is 1 when the capture has no probe phase,
since without the grid there is nothing to place the reloads on.
"""

import argparse
import importlib.util
import os
import re
import statistics
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "probe_schedule", os.path.join(_HERE, "probe_schedule.py"))
probe_schedule = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe_schedule)

OP = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(.*?)\s*$")
BTL = re.compile(r"^OBJ\.WR\s+addr=0x00(18|1a)\s+val=0x[0-9a-f]+$")
SUSPEND = "MAC.MCTRL val=0x00000000 mask=0x00000001"


def collect(path):
    """Timestamps of the reloads that carry the internal mac_suspend."""
    ops = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if m:
            ops.append((float(m.group(1)), m.group(3)))
    out = []
    for i, (t, op) in enumerate(ops):
        m = BTL.match(op)
        if not m:
            continue
        if i + 1 < len(ops) and ops[i + 1][1].startswith(SUSPEND):
            out.append((t, m.group(1)))
    return out


def schedule(path):
    """[tick per reload] or None with no probe phase.

    The grid is probe_schedule.py's: group 0 closes tick 0, and a tick is one
    median group spacing. A reload is placed by its distance from group 0.
    """
    groups, _ = probe_schedule.collect(path)
    if len(groups) < 3:
        return None
    times = [t for t, _ in groups]
    tick = statistics.median(b - a for a, b in zip(times[1:], times[2:]))
    first_cost = max(1, round((times[1] - times[0]) / tick))
    spent = [0] + [first_cost + i for i in range(len(groups) - 1)]
    deadline = spent[-1] + 1

    pre, ticks = 0, []
    for t, _ in collect(path):
        if t < times[0]:
            pre += 1               # prima che la fase parta: non su un tick
            continue
        # Il tick e' la distanza dal primo gruppo in lunghezze di tick. Non
        # "il tick del gruppo che segue": fra il primo gruppo e il secondo ci
        # sono tre tick perche' il primo ne costa tre, e quel criterio li
        # collasserebbe tutti sull'ultimo.
        ticks.append(min(deadline, round((t - times[0]) / tick)))
    return pre, ticks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--sh", action="store_true",
                    help="emit a shell assignment instead of a report")
    a = ap.parse_args()

    got = schedule(a.capture)
    if got is None:
        print("no probe phase in this capture", file=sys.stderr)
        return 1
    pre, ticks = got

    if a.sh:
        print(f"AC_BEACON_RELOADS={pre}:" + ",".join(str(t) for t in ticks))
    else:
        print(f"reloads  {pre + len(ticks)}")
        print(f"pre      {pre} before the phase starts")
        print(f"ticks    {ticks if ticks else 'none'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
