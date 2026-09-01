#!/usr/bin/env python3
"""Op-for-op diff of one phase of a capture against the port's output.

Anchors on the first op the two have in common inside the window, then walks
forward and reports where they part company. Three things are normalised away,
each because it is a property of the tracer or a resolved finding rather than a
divergence:

  ret=...        the vendor prints the return value of a PMU.RC, the port does
                 not. Formatting.
  OR/AND vs MOD  a set or clear renders as "PHY.OR ... (set M)" or
                 "PHY.AND ... (clr M)" on the vendor side and as
                 "PHY.MOD ... mask=0x0000" on the port's. Same
                 b43_phy_set/b43_phy_mask call, two tracers; all three collapse
                 to one form, the value already distinguishing set from clear.
  SI.COREREG     the two real register accesses under every PMU.RC, 0x0658 the
                 selector and 0x065c the data. The harness emits the synthetic
                 PMU.RC line and not these, so their absence is harness
                 fidelity and not missing driver code.
  OBJ 0x0180..7  the probe-response region of shared memory. b43 disables
                 firmware probe responses outright, so these are out of scope;
                 see docs/retrace-todo.md.

  ./phase_diff.py <capture> <port> [--from EP] [--until EP] [--context N]
                  [--all]
"""

import argparse
import re

VENDOR = re.compile(r"^\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(.*?)\s*$")
PORT = re.compile(r"^\s*cpu\d+\s+(.*?)\s*$")

SKIP = re.compile(r"^(SI\.COREREG|OBJ\.(WR|RD)\s+addr=0x0*18[0-7]\b)")


def norm(op):
    # A set or clear of some bits renders three ways: PHY.OR / PHY.AND with a
    # trailing "(set M)" or "(clr M)" on the vendor side, and PHY.MOD with an
    # empty mask on the port's. The value already tells set from clear on both,
    # so all three collapse to one form.
    op = re.sub(r"^(PHY|RAD|OBJ)\.(?:OR|AND)(\s+addr=\S+\s+val=\S+).*$",
                r"\1.BITS\2", op)
    op = re.sub(r"^(PHY|RAD|OBJ)\.MOD(\s+addr=\S+\s+val=\S+)\s+mask=0x0+$",
                r"\1.BITS\2", op)
    op = re.sub(r"val=0x0*([0-9a-fA-F]+)",
                lambda m: "val=0x" + m.group(1).lower(), op)
    op = re.sub(r"mask=0x0*([0-9a-fA-F]+)",
                lambda m: "mask=0x" + m.group(1).lower(), op)
    op = re.sub(r"\s+ret=\S+", "", op)
    return re.sub(r"\s+", " ", op).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("port")
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--until", dest="until", type=int, default=None)
    ap.add_argument("--context", type=int, default=4)
    ap.add_argument("--search", type=int, default=400,
                    help="how far into each side to look for the anchor")
    ap.add_argument("--all", action="store_true",
                    help="report every divergence, not just the first")
    a = ap.parse_args()

    vend = []
    for line in open(a.capture, errors="replace"):
        m = VENDOR.match(line)
        if not m:
            continue
        ep, op = int(m.group(1)), norm(m.group(2))
        if ep < a.start or (a.until is not None and ep >= a.until):
            continue
        if not SKIP.match(op):
            vend.append((ep, op))

    port = [norm(m.group(1)) for m in
            (PORT.match(l) for l in open(a.port, errors="replace")) if m]
    port = [o for o in port if not SKIP.match(o)]

    if not vend or not port:
        print("  nothing to compare")
        return 1

    # Anchor on the longest common run, not on the first op the two share.
    #
    # Taking the first shared op and its first occurrence in the port scores
    # whatever alignment happens to fall out: the port opens every flow with a
    # prologue of its own, and if an op of that prologue also appears at the
    # start of the window the anchor slides backwards and the count that
    # follows means nothing. The longest run is the only choice that cannot be
    # satisfied by a coincidence.
    vo = [o for _, o in vend]
    best = (0, 0, 0)
    for pi in range(min(len(port), a.search)):
        for vi in range(min(len(vo), a.search)):
            k = 0
            while (pi + k < len(port) and vi + k < len(vo)
                   and vo[vi + k] == port[pi + k]):
                k += 1
            if k > best[0]:
                best = (k, pi, vi)
    if not best[0]:
        print("  no common run: wrong flow for this phase?")
        return 1
    _, pi, vi = best
    if vi or pi:
        print(f"  anchored at vendor +{vi} (episode {vend[vi][0]}), "
              f"port +{pi}")

    v = vend[vi:]
    p = port[pi:]
    i = 0
    first = None
    diverged = 0
    while i < len(v) and i < len(p):
        if v[i][1] == p[i]:
            i += 1
            continue
        diverged += 1
        if first is None:
            first = i
        if not a.all:
            break
        i += 1

    if first is None and len(v) == len(p):
        print(f"  {len(v)} ops, identical")
        return 0

    print(f"  {first if first is not None else min(len(v), len(p))} ops match, "
          f"then vendor={len(v)} port={len(p)}")
    if first is None:
        return 1
    print(f"  first divergence at episode {v[first][0]}")
    print()
    print(f"  {'vendor':46s} port")
    lo = max(0, first - a.context)
    for k in range(lo, min(first + 8, max(len(v), len(p)))):
        x = v[k][1] if k < len(v) else ""
        y = p[k] if k < len(p) else ""
        print(f"  {x:46s} {y}{'' if x == y else '  <-'}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
