#!/usr/bin/env python3
"""Shorten a cold segment by removing whole blocks of steady watchdog turns.

The weather-radar segments of a sweep run for ten minutes and more, most of
it the watchdog ticking once a second and the radar timer polling every 150
ms, either while the availability check runs or after the bss-up while the
capture waits. The comparison of such a segment takes longer than any other
and says nothing the first minute does not: the turns are the same turns.

What makes it safe to cut is that the driver keeps its own count of turns
and acts on it periodically -- the temperature reading every temps_period
turns (5 or 10), the region dump every 30 -- so a cut must remove a whole
number of every period. Blocks of BLOCK=60 turns do. The other state the
harness carries across turns is the beacon reload counter, whose parity
picks the template buffer, so a cut also removes an even number of TPL
events. The noise ring of the CRS rule is refilled within four samples.

Runs of turns are taken between anchors -- the first turn after the
bring-up, the bss-up, and the end of the segment -- and each run keeps KEEP
turns on both sides, so the transitions around every anchor are compared
whole. Everything in the removed interval goes, the reads with their RETVAL
and ARGX continuation lines, and the timestamps after it are shifted back by
its length, so the timeline the harness replays has no hole.

Usage:
    compress_turns.py <segment.txt> <out.txt> [--keep 60]
The input is a raw or folded segment; the output has the same form.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BLOCK = 60

LINE = re.compile(r"^(\s*)([\d.]+)(\s+#(\d+)\s.*)$")
PARENT = re.compile(r"\b(?:RETVAL|ARGX)\s+for=#(\d+)")


def timeline_events(path):
    """[(t, ep, kind)] from timeline.py, run on a folded copy."""
    with tempfile.TemporaryDirectory() as d:
        folded = os.path.join(d, "folded")
        tl = os.path.join(d, "timeline")
        subprocess.run([sys.executable, os.path.join(HERE, "trace_filter.py"),
                        "--retvals", path, folded],
                       check=True, capture_output=True)
        subprocess.run([sys.executable, os.path.join(HERE, "timeline.py"),
                        folded, tl], check=True, capture_output=True)
        ev = []
        for l in open(tl):
            t, ep, kind = l.split()[:3]
            ev.append((float(t), int(ep), kind))
        return ev


def plan(events, keep):
    """Episode intervals [lo, hi) to remove, each with its duration."""
    turns = [(t, ep) for t, ep, k in events if k == "WD"]
    tpl = [ep for _, ep, k in events if k == "TPL"]
    anchors = [ep for _, ep, k in events if k == "BSS_UP"]
    runs, cur = [], []
    for t, ep in turns:
        if anchors and ep > anchors[0]:
            runs.append(cur)
            cur = []
            anchors.pop(0)
        cur.append((t, ep))
    runs.append(cur)

    cuts = []
    for run in runs:
        room = len(run) - 2 * keep - 1
        n = (room // BLOCK) * BLOCK if room > 0 else 0
        found = None
        while n > 0 and not found:
            # The start may slide: only the length has to be whole blocks.
            for s in range(keep, min(keep + BLOCK, len(run) - keep - n)):
                a, b = run[s], run[s + n]
                if sum(1 for ep in tpl if a[1] <= ep < b[1]) % 2 == 0:
                    found = (a[1], b[1], b[0] - a[0], n)
                    break
            n -= BLOCK
        if found:
            cuts.append(found)
    return cuts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("segment")
    ap.add_argument("out")
    ap.add_argument("--keep", type=int, default=BLOCK)
    a = ap.parse_args()

    cuts = plan(timeline_events(a.segment), a.keep)
    if not cuts:
        print(f"{a.segment}: nothing to remove", file=sys.stderr)

    dropped = set()
    kept = removed = 0
    with open(a.segment, errors="replace") as fi, open(a.out, "w") as fo:
        for line in fi:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                fo.write(line)
                continue
            t, ep = float(m.group(2)), int(m.group(4))
            p = PARENT.search(line)
            if p and int(p.group(1)) in dropped:
                removed += 1
                continue
            if any(lo <= ep < hi for lo, hi, _, _ in cuts):
                dropped.add(ep)
                removed += 1
                continue
            shift = sum(dt for lo, hi, dt, _ in cuts if ep >= hi)
            width = len(m.group(2))
            fo.write(f"{m.group(1)}{t - shift:{width}.6f}{m.group(3)}\n")
            kept += 1

    for lo, hi, dt, n in cuts:
        print(f"removed {n} turns, #{lo}..#{hi}, {dt:.1f} s", file=sys.stderr)
    print(f"{a.out}: {kept} lines kept, {removed} removed", file=sys.stderr)


if __name__ == "__main__":
    main()
