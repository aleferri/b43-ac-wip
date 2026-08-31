#!/usr/bin/env python3
"""Which registers does the port write differently from the vendor, and where?

Compares a port trace against the capture it reproduces, per (op, address,
mask) key, matching the values in emission order. Aggregated over several
channels it says what is actually wrong with the port today, as opposed to
what a single-channel decorrelation suggests might be.

  ./sweep_regdiff.py <dir>

expects <dir>/<channel>.port and <dir>/<channel>.vendor pairs.
"""

import collections
import glob
import os
import re
import sys

VENDOR = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+(.*?)\s*$")
PORT = re.compile(r"^\s*cpu\d+\s+(.*?)\s*$")
HEX = re.compile(r"0x0*([0-9a-fA-F]+)")


def key_val(line):
    op, _, rest = line.partition(" ")
    if op in ("PHY.AND", "PHY.OR"):
        op = "PHY.MOD"
    f = dict(re.findall(r"(\w+)=(\S+)", rest))
    if "addr" not in f or "val" not in f or f["val"] == "UNDEFINED":
        return None
    norm = lambda s: "0x" + HEX.sub(lambda m: m.group(1).lower(), s).lstrip("0x").lower() \
        if s.startswith("0x") else s
    addr = "0x%04x" % int(f["addr"], 16)
    mask = "0x%04x" % int(f["mask"], 16) if "mask" in f else "-"
    return (op, addr, mask), int(f["val"], 16)


def load(path, rx):
    seq = collections.defaultdict(list)
    for line in open(path, errors="replace"):
        m = rx.match(line)
        if not m:
            continue
        kv = key_val(m.group(1))
        if kv:
            seq[kv[0]].append(kv[1])
    return seq


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    chans = sorted(int(os.path.basename(p).split(".")[0])
                   for p in glob.glob(os.path.join(d, "*.port")))

    # key -> set of channels on which the port's values differ
    bad = collections.defaultdict(set)
    # key -> number of differing writes, summed over channels
    cost = collections.Counter()
    absent = collections.defaultdict(set)
    miscount = collections.defaultdict(set)
    delta = {}
    seen = set()

    for ch in chans:
        v = load(os.path.join(d, f"{ch}.vendor"), VENDOR)
        p = load(os.path.join(d, f"{ch}.port"), PORT)
        for k, vals in v.items():
            seen.add(k)
            pvals = p.get(k)
            if pvals is None:
                absent[k].add(ch)
                continue
            if len(vals) != len(pvals):
                # A different number of writes shifts every later comparison,
                # so the value diff would be noise. Counted separately.
                miscount[k].add(ch)
                delta[k] = len(pvals) - len(vals)
                continue
            n = sum(1 for a, b in zip(vals, pvals) if a != b)
            if n:
                bad[k].add(ch)
                cost[k] += n

    print(f"channels: {chans}")
    print(f"vendor keys seen: {len(seen)}")
    print(f"  wrong values, counts agree : {len(bad)}")
    print(f"  wrong number of writes     : {len(miscount)}")
    print(f"  never written by the port  : {len(absent)}")
    print()
    print(f"{'op':10s} {'addr':8s} {'mask':8s} {'ch wrong':>9s} "
          f"{'writes':>7s}  channels")
    print("-" * 78)
    for k in sorted(bad, key=lambda k: (-cost[k], k)):
        chs = sorted(bad[k])
        allch = "all" if len(chs) == len(chans) else \
            ("all but ch36" if chs == [c for c in chans if c != 36]
             else ",".join(str(c) for c in chs))
        print(f"{k[0]:10s} {k[1]:8s} {k[2]:8s} {len(chs):9d} "
              f"{cost[k]:7d}  {allch}")

    if miscount:
        print(f"\nkeys where the port writes a different number of times "
              f"(value diffs are meaningless until the count matches):")
        for k in sorted(miscount, key=lambda k: -abs(delta.get(k, 0)))[:20]:
            print(f"  {k[0]:10s} {k[1]:8s} {k[2]:8s} "
                  f"{delta.get(k, 0):+6d} writes on {len(miscount[k])} channels")

    if absent:
        print(f"\nkeys present in every capture but never written by the port:")
        for k in sorted(absent, key=lambda k: k[1]):
            if len(absent[k]) == len(chans):
                print(f"  {k[0]:10s} {k[1]:8s} {k[2]}")


if __name__ == "__main__":
    main()
