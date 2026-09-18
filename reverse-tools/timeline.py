#!/usr/bin/env python3
"""The environment's events after the bring-up, read off a capture.

Once the channel switch is over, what the vendor emits is not the driver's
flow but its reaction to things that happen to it: a timer fires, the stack
loads a template, the AP comes up, the radio goes down. On hardware those come
from the kernel and mac80211; the trace harness has neither, so it replays
them from the capture, each at the instant the capture has it. What the driver
does on each event is the driver's, and none of it is in this file.

  WD      a turn of the 1 s watchdog. Marker: the mode change PHY.MOD 0x0520
          mask=0xc, one per turn and none without, at the same instant as the
          turn's head 0x07af. The first turn after the switch has neither --
          it only reads the statistics window -- and its marker is the first
          head word of that read, OBJ.RD 0x010e after the host-flag clear.
  POLL    a turn of the 150 ms radar-detector timer: PHY.RD 0x0251. The one
          glued to the arm is the driver's own and is not an event.
  TPL     a beacon template reload by the core (mac80211's bss_info_changed):
          the length write in BTL0/BTL1 followed by a mac_suspend, which is
          what tells these from the ones inside the BSS configuration. Only
          the reloads after the first watchdog turn are events; the ones
          before it sit inside the bring-up tail and the count comes out as
          AC_BEACON_RELOADS.
  BSS_UP  the AP is up after the channel availability check: the first write
          of the BSS row of the address match table with its flags, AMT.WR
          idx=0x3f a3!=0, after the arm.

Lines are `<t> <op#> <kind>`, sorted by time. Two things about the driver's
state at the start of the flow come out of the same pass, because the harness
cannot know them otherwise:

  AC_WD_PHASE   the value of the watchdog's turn counter, mod 30, at the first
                turn of the segment. The measure block (every 10 turns) and the
                region dump (every 30) are periodic on that counter since the
                driver came up; on a cold attach the measure lands on the tenth
                turn, on a hot cycle wherever the period happens to be. Read
                off the first dump, or the first measure block when the
                segment has no dump.

  ./timeline.py <capture> <outfile>

prints `AC_TIMELINE=<outfile> AC_WD_PHASE=<n> AC_BEACON_RELOADS=<pre>` for the
caller's environment.
A capture with no watchdog turn produces an empty file and exit status 1.
"""

import argparse
import re

OP = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(\S+)\s+(.*)$")


def match(op, rest, name, addr):
    return op == name and re.match(r"^addr=0x0*%x\b" % addr, rest) is not None


def load(path):
    ops = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if m:
            ops.append((float(m.group(1)), int(m.group(2)),
                        m.group(3), m.group(4)))
    return ops


def events(ops):
    turns = [i for i, (_, _, op, rest) in enumerate(ops)
             if op == "PHY.MOD"
             and re.match(r"^addr=0x0*520\b.*\bmask=0x0*c\b", rest)]
    if not turns:
        return [], 0, 0

    # The host-flag clear that ends the channel switch. The first turn after
    # it reads the statistics window without latching it and carries no
    # sampling phase, so its marker is the first head word of the poll.
    mhf = max(i for i, (_, _, op, rest) in enumerate(ops[:turns[0]])
              if op == "MAC.MHF" and re.search(r"\bmask=0x0*4000\b", rest))
    first = next(i for i in range(mhf, turns[0])
                 if match(ops[i][2], ops[i][3], "OBJ.RD", 0x10e))
    turns = [first] + turns

    out = [(ops[i][0], ops[i][1], "WD") for i in turns]
    t_first = ops[first][0]

    # The arm is the last maskset of 0x02e4 that a detector poll follows within
    # a few operations; the earlier ones belong to the radio bring-up.
    arm = None
    for i, (_, _, op, rest) in enumerate(ops):
        if (op == "PHY.MOD" and re.match(r"^addr=0x0*2e4 val=0x0*f00\b", rest)
                and any(match(ops[j][2], ops[j][3], "PHY.RD", 0x251)
                        for j in range(i + 1, min(i + 9, len(ops))))):
            arm = i
    if arm is not None:
        polls = [i for i in range(arm + 1, len(ops))
                 if match(ops[i][2], ops[i][3], "PHY.RD", 0x251)]
        # The first poll is the arm's own turn, emitted by the driver.
        out += [(ops[i][0], ops[i][1], "POLL") for i in polls[1:]]
        for i in range(arm + 1, len(ops)):
            _, _, op, rest = ops[i]
            if (op == "AMT.WR" and re.match(r"^idx=0x0*3f\b", rest)
                    and re.search(r"\ba3=0x0*[1-9a-f]", rest)):
                out.append((ops[i][0], ops[i][1], "BSS_UP"))
                break

    pre = 0
    for i in range(mhf, len(ops) - 1):
        t, n, op, rest = ops[i]
        if (op == "OBJ.WR" and re.match(r"^addr=0x0*1[8a]\b", rest)
                and ops[i + 1][2] == "MAC.MCTRL"
                and ops[i + 1][3].startswith("val=0x00000000 mask=0x00000001")):
            if i < first:
                pre += 1
            else:
                out.append((t, n, "TPL"))

    out.sort(key=lambda e: (e[0], e[1]))

    # The watchdog counter's phase: the measure block falls where the
    # counter reads 9 mod 10, the region dump where it reads 29 mod 30. The
    # dump pins the phase mod 30; without one in the segment the measure block
    # pins it mod 10 and the residue is chosen so that no dump falls inside
    # the segment, which is what the capture shows.
    phase = None
    turn_t = [ops[i][0] for i in turns]
    nturns = len(turns)
    for t, _, op, rest in ops:
        if (op == "OBJ.BULKR" and t >= t_first
                and re.match(r"^addr=0x0*e0 len=128\b", rest)):
            idx = sum(1 for x in turn_t if x <= t) - 1
            phase = (29 - idx) % 30
            break
    if phase is None:
        phase = 0
        for i, (t, _, op, rest) in enumerate(ops):
            if not match(op, rest, "PHY.RD", 0x73c) or t < t_first:
                continue
            if any(o[2] == "AMT.WR" for o in ops[max(0, i - 60):i]):
                continue
            idx = sum(1 for x in turn_t if x <= t) - 1
            if t - turn_t[idx] > 1.5:
                continue
            phase = (9 - idx) % 10
            break
        for cand in (phase, phase + 10, phase + 20):
            if (29 - cand) % 30 >= nturns:
                phase = cand
                break
    return out, phase, pre


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("outfile")
    a = ap.parse_args()

    out, phase, pre = events(load(a.capture))
    with open(a.outfile, "w") as f:
        for t, n, kind in out:
            f.write(f"{t:.6f} {n} {kind}\n")
    if not out:
        return 1
    print(f"AC_TIMELINE={a.outfile} AC_WD_PHASE={phase} AC_BEACON_RELOADS={pre}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
