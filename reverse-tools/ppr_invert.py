#!/usr/bin/env python3
"""Invert the PPR chain on a segment, under either model of the maximum.

The chain is the same in both models:

    field[i] = (M - cp[i]) / 2      cp[i] = min(ppr[i], cap)
    ppr[i]   = maxp5ga[sb] - 2 * nib[i]

What differs is what M is, and that single assumption decides whether the
problem needs a search or has a closed form:

  capped   M = max(cp), the maximum AFTER the cap. Then M is not a free
           unknown -- given the cap it is determined -- so the cap alone is
           enumerated and every value that reproduces the eight fields is
           kept. If one survives the point is determined; if several do the
           point is degenerate and constrains only a relation. That happens
           when the cap flattens every rate, and then the data fix only
           M - cap.

  srom     M = maxp5ga[sb] - 2 * min(nib), the SROM maximum, not capped. Then
           cp follows from the observation alone (cp[i] = M - 2*field[i]) and
           the cap follows from cp: where cp equals ppr it does not bite,
           where it is lower cp is the cap, and the capped rates must share
           it. No enumeration, no free parameter.

The two are not interchangeable and neither is a refinement of the other: they
are two readings of what the vendor's reference level is, and which one holds
is what the data has to decide. Running both on the same segment is the point.

Usage:
  ppr_invert.py --maxp N --po 0xNNNNN --fields a,b,c,d,e,f,g,h [--model both]
"""
import argparse
import collections
import sys

# 6, 9, 12, 18 Mb/s share mcs0; 24, 36, 48, 54 take mcs1..4
MAP_MCS = [0, 0, 0, 0, 1, 2, 3, 4]

# state: 'determined' | 'degenerate' | 'inconsistent'
Result = collections.namedtuple("Result", "model ppr max caps state note")


def nib(po, i):
    return (po >> (4 * i)) & 0xf


def ppr_rates(maxp, po):
    """The ppr of the eight legacy OFDM rates. Unsigned nibbles."""
    return [maxp - 2 * nib(po, MAP_MCS[i]) for i in range(8)]


def compatible_caps(fields, maxp, po, lo=32, hi=132):
    """Every cap that reproduces the observed fields. None = no cap."""
    ppr = ppr_rates(maxp, po)
    out = []
    for cap in [None] + list(range(lo, hi + 1, 2)):
        cp = ppr if cap is None else [min(p, cap) for p in ppr]
        m = max(cp)
        if all((m - cp[i]) % 2 == 0 and (m - cp[i]) // 2 == fields[i]
               for i in range(8)):
            out.append(cap)
    return ppr, out


def solve_capped(fields, maxp, po, lo=32, hi=132):
    """M = max(cp): enumerate the cap."""
    ppr, caps = compatible_caps(fields, maxp, po, lo, hi)
    if not caps:
        return Result("capped", ppr, None, None, "inconsistent",
                      "no cap reproduces the fields")
    # "no cap" and every cap >= max(ppr) are the same case.
    effective = sorted({c for c in caps if c is not None and c < max(ppr)})
    without = None in caps or any(c is None or c >= max(ppr) for c in caps)
    if effective and not without:
        state = "determined" if len(effective) == 1 else "degenerate"
        m = max(min(p, effective[0]) for p in ppr)
        return Result("capped", ppr, m, effective, state, None)
    if without and not effective:
        return Result("capped", ppr, max(ppr), [None], "determined",
                      "the cap does not bite")
    return Result("capped", ppr, None, [None] + effective, "degenerate",
                  "with and without a cap are both compatible")


def solve_srom(fields, maxp, po):
    """M = SROM maximum: closed form."""
    ppr = ppr_rates(maxp, po)
    m = maxp - 2 * min(nib(po, i) for i in range(8))
    cp = [m - 2 * c for c in fields]
    capped = [i for i in range(8) if cp[i] != ppr[i]]
    if not capped:
        return Result("srom", ppr, m, [None], "determined",
                      "the cap does not bite")
    if any(cp[i] > ppr[i] for i in capped):
        return Result("srom", ppr, m, None, "inconsistent",
                      "cap above ppr, which is absurd")
    values = sorted({cp[i] for i in capped})
    if len(values) != 1:
        return Result("srom", ppr, m, values, "inconsistent",
                      "the capped rates disagree on the cap")
    cap = values[0]
    if not all(cp[i] == min(ppr[i], cap) for i in range(8)):
        return Result("srom", ppr, m, [cap], "inconsistent",
                      "the cap does not reproduce the untouched rates")
    return Result("srom", ppr, m, [cap], "determined", None)


def solve(fields, maxp, po, model="capped", lo=32, hi=132):
    if model == "capped":
        return solve_capped(fields, maxp, po, lo, hi)
    if model == "srom":
        return solve_srom(fields, maxp, po)
    raise ValueError(f"unknown model: {model!r}")


def report(r):
    print(f"  model {r.model:8} {r.state}")
    print(f"    ppr   {r.ppr}")
    print(f"    max   {r.max}")
    print(f"    cap   {r.caps}")
    if r.note:
        print(f"    note  {r.note}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--maxp", type=int, required=True,
                    help="maxp5ga of the sub-band, in quarter dBm")
    ap.add_argument("--po", required=True,
                    help="the packed offset nibbles, e.g. 0x64200")
    ap.add_argument("--fields", required=True,
                    help="the eight observed fields, comma separated")
    ap.add_argument("--model", default="both",
                    choices=("capped", "srom", "both"))
    args = ap.parse_args()

    fields = [int(x) for x in args.fields.split(",")]
    if len(fields) != 8:
        sys.exit(f"eight fields are needed, got {len(fields)}")
    po = int(args.po, 0)

    models = ("capped", "srom") if args.model == "both" else (args.model,)
    for m in models:
        report(solve(fields, args.maxp, po, m))


if __name__ == "__main__":
    main()
