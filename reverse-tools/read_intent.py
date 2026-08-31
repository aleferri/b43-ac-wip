#!/usr/bin/env python3
"""What does the stock driver do with the reads this port discards?

"Read for bus order" is not an explanation, it is what is left when nobody
worked it out. A driver reads a register for one of a few reasons, and each
leaves a signature in the capture:

  sample loop   the same address read many times in a row, the values varying;
                the driver is averaging or accumulating. This is what PHY 0x0012
                turned out to be, and calling it bus order hid the bug.
  poll          long runs too, but the values converge to a fixed one and the
                run ends there: the driver is waiting on a bit.
  read-modify   a write to the same address follows immediately; the read
                supplies the bits the write preserves.
  read-to-clear the value is non-zero, then zero on the next read of the same
                address with no write in between.
  probe         a lone read whose value never varies anywhere. Nothing is being
                learnt, so either the read has a side effect or it is genuinely
                only ordering.

  ./read_intent.py <dir> [discarded-list]

<dir> holds <channel>.vendor captures. The optional list is the output of
consumed_reads.sh, to restrict the report to the addresses the port discards.
"""

import collections
import glob
import os
import re
import sys

OP = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+"
                r"(PHY|RAD)\.(RD|WR|MOD|AND|OR)\s+addr=0x([0-9a-fA-F]+)"
                r"(?:\s+val=0x([0-9a-fA-F]+))?")

KIND = {"PHY": "phy", "RAD": "radio"}


def load(path):
    """Ordered (kind, addr, op, val) for every register access."""
    out = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if not m:
            continue
        val = int(m.group(4), 16) if m.group(4) else None
        out.append((KIND[m.group(1)], int(m.group(3), 16),
                    "rd" if m.group(2) == "RD" else "wr", val))
    return out


def classify(paths, restrict):
    runs = collections.defaultdict(int)        # longest consecutive run
    run_tail = collections.defaultdict(set)    # last value of each long run
    run_spread = collections.defaultdict(set)  # values seen inside long runs
    rmw = collections.Counter()                # read then write, same addr
    reads = collections.Counter()
    clears = collections.Counter()
    vals = collections.defaultdict(set)
    last_read = {}

    for p in paths:
        ops = load(p)
        i = 0
        while i < len(ops):
            kind, addr, op, val = ops[i]
            key = (kind, addr)
            if op != "rd":
                last_read.pop(key, None)
                i += 1
                continue

            reads[key] += 1
            if val is not None:
                vals[key].add(val)

            # read-to-clear: previous read of the same address was non-zero,
            # this one is zero, and nothing was written in between
            prev = last_read.get(key)
            if prev is not None and prev and val == 0:
                clears[key] += 1
            last_read[key] = val

            # consecutive run of reads of the same address
            j = i
            while (j + 1 < len(ops) and ops[j + 1][0] == kind
                   and ops[j + 1][1] == addr and ops[j + 1][2] == "rd"):
                j += 1
            n = j - i + 1
            if n > runs[key]:
                runs[key] = n
            if n >= 4:
                run_tail[key].add(ops[j][3])
                for k in range(i, j + 1):
                    run_spread[key].add(ops[k][3])

            if j + 1 < len(ops) and ops[j + 1][:2] == (kind, addr) \
                    and ops[j + 1][2] == "wr":
                rmw[key] += 1
            i = j + 1

    rows = []
    for key in reads:
        if restrict and key not in restrict:
            continue
        n = reads[key]
        run = runs[key]
        spread = len(run_spread.get(key, ()))
        tails = run_tail.get(key, set())
        nvals = len(vals[key])

        if run >= 4 and spread > 2:
            verdict = "sample loop" if len(tails) > 1 else "poll"
        elif rmw[key] >= max(1, n // 4):
            verdict = "read-modify-write"
        elif clears[key]:
            verdict = "read-to-clear?"
        elif nvals <= 1:
            verdict = "probe, value never varies"
        else:
            verdict = "UNEXPLAINED"
        rows.append((verdict, key, n, run, nvals, rmw[key], clears[key]))
    return rows


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    restrict = set()
    if len(sys.argv) > 2:
        for line in open(sys.argv[2]):
            f = line.split()
            if len(f) >= 4 and f[3] == "DISCARDED":
                restrict.add((f[0], int(f[1], 16)))

    paths = sorted(glob.glob(os.path.join(d, "*.vendor")))
    rows = classify(paths, restrict)

    order = ["sample loop", "poll", "read-modify-write", "read-to-clear?",
             "probe, value never varies", "UNEXPLAINED"]
    rows.sort(key=lambda r: (order.index(r[0]), -r[2]))

    tot = collections.Counter(r[0] for r in rows)
    print(f"captures: {len(paths)}   addresses examined: {len(rows)}")
    for k in order:
        if tot[k]:
            print(f"  {k:28s} {tot[k]}")
    print()
    print(f"{'verdict':28s}{'reg':11s}{'reads':>7s}{'run':>6s}"
          f"{'values':>8s}{'rmw':>6s}{'clr':>5s}")
    print("-" * 72)
    for verdict, (kind, addr), n, run, nvals, nrmw, nclr in rows:
        print(f"{verdict:28s}{kind:5s}{addr:#06x}{n:7d}{run:6d}"
              f"{nvals:8d}{nrmw:6d}{nclr:5d}")


if __name__ == "__main__":
    main()
