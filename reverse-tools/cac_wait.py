#!/usr/bin/env python3
"""How many watchdog turns the channel availability check lasts, in a capture.

On a channel with the radar duty the calibrations that transmit do not start
until the check has closed, and the watchdog turns in the meantime. That phase
is b43_phy_ac_cac_wait(), and how long it lasts is the stock driver waiting on
an event -- mac80211's CAC_FINISHED on hardware -- so it is not something the
driver can know. It comes out of the capture, like AC_PROBE_TICKS,
AC_BEACON_RELOADS and AC_CAC_POLLS, and goes in as AC_CAC_WAIT_TICKS.

The window has a marker at each end, and both are the core suspending and
restoring the address match row of the BSS while the check runs:

  arm      PHY.MOD 0x02e4 val=0x0f00, the maskset that opens the check, taken
           as the last one the capture has that a poll of PHY 0x0251 follows
           within a few operations. Earlier occurrences belong to the radio
           bring-up and are not this phase.
  restore  the first AMT.WR on row 0x3f carrying the flags (a3 non-zero) after
           the arm. The clear that pairs with it sits two operations after the
           arm, bare; see docs/retrace-todo.md for why the port does not emit
           either of the two yet.

Between them a turn is one read of PHY 0x07af, the head of the sample-phase
block, which b43_phy_ac_cac_wait_turn() emits exactly once per turn. Counting
the head and not the body matters: the body is skipped on some turns and the
flat sweep of 0x0768 runs more than once in others, so neither counts turns.

**This is a measurement and not an input.** How long the check lasts is its
timer -- 60 s, and 600 in the weather-radar sub-band -- and the driver counts
it in watchdog turns, one per second, in b43_phy_ac_cac_tick(). Nothing is
passed to the harness. What this tool is for is checking that constant against
the captures and telling apart the three kinds of segment the sweep has:

  window closed   arm and restore both present. 21 segments, and the turns
                  between them are the timer's worth: 58 on eighteen of them,
                  61 on two, 68 on one, with the turns 1.004 s apart.
  window open     an arm and no restore: the check had not closed when the
                  recording stopped. The six weather-radar configurations,
                  whose 600 s do not fit in a 35 s segment.
  no window       no arm: the channel carries no radar duty. 16 segments.

  ./cac_wait.py <capture> [--sh]

--sh emits `AC_CAC_WAIT_CAP=<n>` for a caller that wants the count, and nothing
at all when the capture has no arm. No gate consumes it.
"""

import argparse
import re

OP = re.compile(r"^\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(\S+)\s+(.*)$")
ARM = re.compile(r"^addr=0x0*2e4 val=0x0*f00\b")
POLL = re.compile(r"^addr=0x0*251\b")
ROW = re.compile(r"^idx=0x0*3f\b")
FLAGS = re.compile(r"\ba3=0x0*[1-9a-f][0-9a-f]*\b")
HEAD = re.compile(r"^addr=0x0*7af\b")

# How far after the arm the first poll may sit before the arm is taken to be
# something else. On the sweep it is two operations; the margin is for a
# capture that traces a class these do not.
POLL_WINDOW = 8


def window(path):
    """(arm, restore) indices into the op list, or (None, None)."""
    ops = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if m:
            ops.append((m.group(2), m.group(3)))

    arm = None
    for i, (op, rest) in enumerate(ops):
        if op != "PHY.MOD" or not ARM.match(rest):
            continue
        if any(ops[j][0] == "PHY.RD" and POLL.match(ops[j][1])
               for j in range(i + 1, min(i + 1 + POLL_WINDOW, len(ops)))):
            arm = i
    if arm is None:
        return ops, None, None

    for i in range(arm + 1, len(ops)):
        op, rest = ops[i]
        if op == "AMT.WR" and ROW.match(rest) and FLAGS.search(rest):
            return ops, arm, i
    # No restore: the check was still outstanding when the recording stopped,
    # so the phase runs to the end of it.
    return ops, arm, len(ops)


def turns(path):
    ops, arm, restore = window(path)
    if arm is None:
        return None
    return sum(1 for op, rest in ops[arm + 1:restore]
               if op == "PHY.RD" and HEAD.match(rest))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--sh", action="store_true",
                    help="emit a shell assignment instead of a report")
    a = ap.parse_args()

    n = turns(a.capture)

    if a.sh:
        if n:
            print(f"AC_CAC_WAIT_CAP={n}")
    else:
        print(f"turns    {n if n is not None else 'no arm'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
