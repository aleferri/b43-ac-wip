#!/usr/bin/env python3
"""Probe-phase schedule of a capture, read off its timestamps.

The probe/measure phase at the end of the RXIQ calibration runs to a wall-clock
deadline, one probe group per ~1 s tick, and the periodic watchdog fires into it
asynchronously. Neither is visible to the trace harness, which has no clock, so
`ac_trace` takes both from the caller: AC_PROBE_TICKS and AC_WATCHDOG_TICKS.
Hardcoding them fits one capture and misses every other, because the deadline is
clock jitter and moves from segment to segment.

Both are in the capture, in the timestamps:

  probe group   PHY.RD 0x07af, the first read of the group
  measure block PHY.RD 0x073c, the first read of the RX AFE reconfig

The first group is irregular and consumes three ticks instead of one, so the
tick a group lands on is 0 for the first and 3, 4, 5 ... for the rest; the
deadline is one past the last. The tick cost of the first group is measured
rather than assumed, from the gap between the first two groups over the tick
length, so a capture where the first group is regular reads correctly too.

A watchdog firing is placed on the tick of the group that follows it. A firing
past the last group belongs to the tick the phase closed on, which is the
deadline itself -- the loop in b43_phy_ac_rxiqcal_finalize() checks for it below
the loop for exactly this case.

  ./probe_schedule.py <capture> [--sh]

--sh emits `AC_PROBE_TICKS=.. AC_WATCHDOG_TICKS=..`, ready to prefix a command.
Exit status is 1 when the capture has no probe phase, so a caller can fall back
to the harness defaults.
"""

import argparse
import re
import statistics
import sys

GROUP = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+PHY\.RD\s+addr=0x0*7af\b")
BLOCK = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+PHY\.RD\s+addr=0x0*73c\b")


def collect(path):
    groups, blocks = [], []
    for line in open(path, errors="replace"):
        m = GROUP.match(line)
        if m:
            groups.append((float(m.group(1)), int(m.group(2))))
            continue
        m = BLOCK.match(line)
        if m:
            blocks.append((float(m.group(1)), int(m.group(2))))
    return groups, blocks


def schedule(groups, blocks):
    """(deadline, [watchdog ticks]) or None when there is no probe phase."""
    if len(groups) < 3:
        return None

    times = [t for t, _ in groups]
    # Tick length from the regular groups. The median absorbs the late
    # wakeups: some segments pace at 1.32 s instead of 1.00 for a tick or
    # two, and a mean would drag every tick index with them.
    tick = statistics.median(b - a for a, b in zip(times[1:], times[2:]))
    first_cost = max(1, round((times[1] - times[0]) / tick))

    spent = [0] + [first_cost + i for i in range(len(groups) - 1)]
    deadline = spent[-1] + 1

    watchdog = []
    for t, _ in blocks:
        if t < times[0]:
            continue                     # before the phase: cal rounds
        later = [s for s, (tg, _) in zip(spent, groups) if tg > t]
        watchdog.append(later[0] if later else deadline)

    return deadline, watchdog


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--sh", action="store_true",
                    help="emit shell assignments instead of a report")
    a = ap.parse_args()

    groups, blocks = collect(a.capture)
    got = schedule(groups, blocks)
    if got is None:
        print("no probe phase in this capture", file=sys.stderr)
        return 1
    deadline, watchdog = got

    if a.sh:
        ticks = ",".join(str(w) for w in watchdog[:2]) or "65535"
        print(f"AC_PROBE_TICKS={deadline} AC_WATCHDOG_TICKS={ticks}")
    else:
        print(f"groups   {len(groups)}")
        print(f"deadline {deadline} ticks")
        print(f"watchdog {watchdog if watchdog else 'never'}")
        if len(watchdog) > 2:
            print(f"note: {len(watchdog)} firings, the harness carries two",
                  file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
