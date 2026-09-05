#!/usr/bin/env python3
"""Report a port trace against the capture it reproduces, over a sweep.

Two reports over the same input, answering the two questions one asks of a
sweep in sequence:

  score     How much of each capture the port reproduces, one row per
            channel. It is the coarse signal: it says which channels moved.

  regdiff   Which registers the port writes differently, aggregated over all
            the channels. It is what says what is actually wrong today, as
            opposed to what a single-channel decorrelation suggests might be.

WARNING about the score: the denominator here is the vendor ops alone, while
test/cmp_skip.py uses the union of the two streams and so penalizes the ops
the port emits and the vendor does not. **The two numbers are not
comparable**, and the citable one is cmp_skip's. This score exists to compare
channels against each other, not to be quoted.

The restricted score drops the op classes the port does not implement or the
harness does not model, so that a per-channel regression is not buried under a
constant offset that is the same on every channel. That list is this tool's
own and is NOT the perimeter of test/compare.py, which is built on a different
criterion (what the harness cannot emit, with the proof written next to each
entry). Do not read one as the other.

Usage:
  sweep_report.py score <vendor> <port> [label]
  sweep_report.py regdiff <dir>       expects <dir>/<ch>.port and <ch>.vendor
"""
import argparse
import collections
import difflib
import glob
import os
import re
import sys

import tracelib as T

# Op classes the port does not implement or the harness does not model. This
# is a coarse per-class cut, not an ownership argument: see the warning above.
SKIP = ("OBJ.", "TPL.", "SI.COREREG", "PMU.", "OTP.", "SROMCTL.",
        "CAL.INIT", "MAC.BW")


def load_ops(path, vendor):
    rx = T.RE_VENDOR if vendor else T.RE_PORT
    grp = 3 if vendor else 1
    out = []
    with T.open_trace(path) as f:
        for line in f:
            m = rx.match(line)
            if m:
                out.append(T.norm(m.group(grp)))
    return out


# ---------------------------------------------------------------------------
# score
# ---------------------------------------------------------------------------

def matched_in_order(a, b):
    """Ops matched in order, the same measure as test/cmp_skip.py."""
    sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
    return sum(n for _, _, n in sm.get_matching_blocks())


def score(vendor, port):
    if not vendor:
        return 0, 0, 0.0
    n = matched_in_order(vendor, port)
    return n, len(vendor), 100.0 * n / len(vendor)


def mode_score(args):
    vendor = load_ops(args.vendor, True)
    port = load_ops(args.port, False)
    keep = [o for o in vendor if not any(o.startswith(p) for p in SKIP)]
    keep_p = [o for o in port if not any(o.startswith(p) for p in SKIP)]

    _, _, full = score(vendor, port)
    n_r, d_r, restricted = score(keep, keep_p)

    status = "MATCH" if restricted >= 99.995 else f"{d_r - n_r} op"
    print(f"{args.label:>5s} {len(vendor):8d} {len(port):8d} "
          f"{full:9.2f}% {restricted:9.2f}% {status}")
    return 0


# ---------------------------------------------------------------------------
# regdiff
# ---------------------------------------------------------------------------

def key_val(op):
    """((op, addr, mask), value) of a write, or None.

    The key is the cell and how it is written; the value is compared
    separately, in emission order, so a register written N times is checked
    write by write.
    """
    name, _, rest = op.partition(" ")
    f = dict(re.findall(r"(\w+)=(\S+)", rest))
    if "addr" not in f or "val" not in f or f["val"] == "UNDEFINED":
        return None
    addr = "0x%04x" % int(f["addr"], 16)
    mask = "0x%04x" % int(f["mask"], 16) if "mask" in f else "-"
    return (name, addr, mask), int(f["val"], 16)


def load_keyed(path, vendor):
    seq = collections.defaultdict(list)
    for op in load_ops(path, vendor):
        kv = key_val(op)
        if kv:
            seq[kv[0]].append(kv[1])
    return seq


def mode_regdiff(args):
    d = args.dir
    chans = sorted(int(os.path.basename(p).split(".")[0])
                   for p in glob.glob(os.path.join(d, "*.port")))
    if not chans:
        sys.exit(f"no <channel>.port file in {d}")

    bad = collections.defaultdict(set)
    cost = collections.Counter()
    absent = collections.defaultdict(set)
    miscount = collections.defaultdict(set)
    delta, seen = {}, set()

    for ch in chans:
        v = load_keyed(os.path.join(d, f"{ch}.vendor"), True)
        p = load_keyed(os.path.join(d, f"{ch}.port"), False)
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
        print("\nkeys where the port writes a different number of times "
              "(value diffs are meaningless until the count matches):")
        for k in sorted(miscount, key=lambda k: -abs(delta.get(k, 0)))[:20]:
            print(f"  {k[0]:10s} {k[1]:8s} {k[2]:8s} "
                  f"{delta.get(k, 0):+6d} writes on {len(miscount[k])} channels")

    if absent:
        print("\nkeys present in every capture but never written by the port:")
        for k in sorted(absent, key=lambda k: k[1]):
            if len(absent[k]) == len(chans):
                print(f"  {k[0]:10s} {k[1]:8s} {k[2]}")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("score", help="one row per channel, coarse signal")
    p.add_argument("vendor")
    p.add_argument("port")
    p.add_argument("label", nargs="?", default="-")
    p.set_defaults(fn=mode_score)

    p = sub.add_parser("regdiff", help="which registers differ, over a sweep")
    p.add_argument("dir", nargs="?", default=".")
    p.set_defaults(fn=mode_regdiff)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
