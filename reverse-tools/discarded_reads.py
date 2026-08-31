#!/usr/bin/env python3
"""Which reads does the port discard whose value actually varies?

The port sends reads it does not need through b43_phy_read_log() and
b43_radio_read_log() into a discarded variable, so that the bus order matches
the stock driver's. That is deliberate. It is also where a whole class of
latent bug lives: if the vendor reads a register and its later writes depend
on what came back, then discarding the value means the port is reproducing a
constant that happened to hold on the board the capture came from.

The captures can tell the two apart. A discarded read whose value is the same
in every capture is at worst fragile. A discarded read whose value *varies*
across configurations is a point where the port cannot be reacting to
anything, so any dependent write is transcribed rather than derived.

  ./discarded_reads.py <dir>

expects <dir>/<channel>.vendor capture files carrying merged read values, and
reads the port sources to find which addresses are discarded.
"""

import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

READ_LOG = re.compile(r"b43_(phy|radio)_read_log\(dev,\s*([^;)]+)\)")
LITERAL = re.compile(r"^0x([0-9a-fA-F]+)$")

VENDOR_RD = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+"
                       r"(PHY|RAD)\.RD\s+addr=0x([0-9a-fA-F]+)\s+"
                       r"val=0x([0-9a-fA-F]+)")


def discarded_addrs():
    """(kind, addr) for every literal read_log site, plus the computed ones."""
    lit = collections.Counter()
    computed = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".c"):
            continue
        for lineno, line in enumerate(open(os.path.join(SRC, name)), 1):
            for kind, arg in READ_LOG.findall(line):
                arg = arg.strip()
                m = LITERAL.match(arg)
                if m:
                    lit[(kind, int(m.group(1), 16))] += 1
                else:
                    computed.append((name, lineno, kind, arg))
    return lit, computed


def capture_reads(paths):
    """(kind, addr) -> set of values seen, and the per-file value sets."""
    vals = collections.defaultdict(set)
    per_file = collections.defaultdict(dict)
    for p in paths:
        for line in open(p, errors="replace"):
            m = VENDOR_RD.match(line)
            if not m:
                continue
            kind = "phy" if m.group(1) == "PHY" else "radio"
            key = (kind, int(m.group(2), 16))
            v = int(m.group(3), 16)
            vals[key].add(v)
            per_file[key].setdefault(p, set()).add(v)
    return vals, per_file


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    paths = sorted(glob.glob(os.path.join(d, "*.vendor")))
    if not paths:
        print(f"no capture files in {d}")
        return 2

    lit, computed = discarded_addrs()
    vals, per_file = capture_reads(paths)

    print(f"captures: {len(paths)}")
    print(f"discarded read sites with a literal address: {sum(lit.values())} "
          f"over {len(lit)} addresses")
    print(f"discarded read sites with a computed address: {len(computed)}")
    print()

    varying, constant, unseen = [], [], []
    for key, n in lit.items():
        seen = vals.get(key)
        if not seen:
            unseen.append((key, n))
        elif len(seen) > 1:
            # does it vary *between* configurations, or only within one?
            across = len({frozenset(s) for s in per_file[key].values()}) > 1
            varying.append((key, n, sorted(seen), across))
        else:
            constant.append((key, n, sorted(seen)[0]))

    varying.sort(key=lambda x: (-len(x[2]), x[0][1]))

    print("LATENT BUGS: value varies across the captures, and is discarded.")
    print("Any write that depends on one of these is transcribed, not derived.")
    print()
    print(f"  {'reg':10s} {'sites':>5s} {'values':>7s}  across configs  values")
    print("  " + "-" * 72)
    for (kind, addr), n, seen, across in varying:
        vs = ", ".join(f"{v:#x}" for v in seen[:6])
        if len(seen) > 6:
            vs += f", ... ({len(seen)} total)"
        print(f"  {kind:5s}{addr:#06x} {n:5d} {len(seen):7d}  "
              f"{'yes' if across else 'no ':^14s}  {vs}")

    print()
    print(f"CONSTANT across every capture: {len(constant)} addresses. Fragile "
          f"rather than wrong -- the value could still differ on another board.")
    print(f"NOT READ in any capture: {len(unseen)} addresses. Either the flow "
          f"is not exercised or the read is the port's own invention.")
    if unseen:
        for (kind, addr), n in sorted(unseen, key=lambda x: x[0][1]):
            print(f"  {kind:5s}{addr:#06x}  {n} site(s)")

    if computed:
        print()
        print(f"COMPUTED addresses, not resolvable from the source alone "
              f"({len(computed)} sites); these need the trace to classify:")
        for name, lineno, kind, arg in computed[:15]:
            print(f"  {name}:{lineno}  {kind}  {arg}")
        if len(computed) > 15:
            print(f"  ... and {len(computed) - 15} more")

    return 0


if __name__ == "__main__":
    sys.exit(main())
