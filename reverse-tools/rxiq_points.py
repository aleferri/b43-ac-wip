#!/usr/bin/env python3
"""RX-IQ solve points from the sweeps, and the score of the solver on them.

A point is one write of PHY 0x?a0/0x?a1 with a non-zero value, paired with the
accumulator rounds (six reads 0x?c0-0x?c5) of the same core that precede it
since the previous write. Rounds preceded by a touch of PHY 0x0b22 belong to
the loopback gain search; among the others, the short estimate (0x0272 =
0x400 samples, a sixteenth of the power) is dropped too. What remains are the
measurement tones: two up to 40 MHz, six at 80.

The two models scored are the one the driver had, summing the accumulators
over the tones, and the one it has now, the mean over the tones of the
per-tone coefficients -- see b43_phy_ac_iq_solve() in src/phy_ac.c for the
arithmetic and the numbers.

Usage:
    python3 rxiq_points.py extract SEG.txt... > points.json
    python3 rxiq_points.py score points.json [--verbose]

The segments are the raw ones from cold-sweep.zip / hot-sweep.zip; they are
folded with trace_filter.py here.
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
from collections import Counter

RE = re.compile(r'#(\d+)\s+cpu\d\s+(PHY\.(?:RD|WR|MOD))\s+addr=0x([0-9a-f]+)'
                r'\s+val=0x([0-9a-f]+)')
HERE = os.path.dirname(os.path.abspath(__file__))


def fold(path):
    out = '/tmp/rxiq_points_fold.txt'
    subprocess.run([sys.executable, os.path.join(HERE, 'trace_filter.py'),
                    '--retvals', path, out], check=True, capture_output=True)
    return out


def extract(seg, path):
    pts = []
    pend = {c: {} for c in range(3)}
    rounds = {c: [] for c in range(3)}
    search = {c: False for c in range(3)}
    a_val = {}
    for line in open(fold(path)):
        m = RE.search(line)
        if not m:
            continue
        op, addr, val = m.group(2), int(m.group(3), 16), int(m.group(4), 16)
        if addr == 0x0b22 and op != 'PHY.RD':
            for c in search:
                search[c] = True
            continue
        if not 0x600 <= addr < 0xc00:
            continue
        core = (addr - 0x600) // 0x200
        base = addr - core * 0x200
        if op == 'PHY.RD' and 0x6c0 <= base <= 0x6c5:
            pend[core][base] = val & 0xffff
            if len(pend[core]) == 6:
                p = pend[core]
                iq = (p[0x6c1] << 16) | p[0x6c0]
                if iq & 0x80000000:
                    iq -= 1 << 32
                rounds[core].append(dict(ii=(p[0x6c3] << 16) | p[0x6c2],
                                         qq=(p[0x6c5] << 16) | p[0x6c4],
                                         iq=iq, search=search[core]))
                search[core] = False
                pend[core] = {}
        elif op == 'PHY.WR' and base == 0x6a0:
            a_val[core] = val
        elif op == 'PHY.WR' and base == 0x6a1:
            if val and rounds[core]:
                a = a_val.get(core, 0)
                pts.append(dict(seg=seg, core=core,
                                a=a - 1024 if a & 0x200 else a,
                                b=val - 1024 if val & 0x200 else val,
                                rounds=rounds[core]))
            rounds[core] = []
            pend[core] = {}
    return pts


def meas_rounds(pt):
    rs = [r for r in pt['rounds'] if not r['search']]
    top = max(r['ii'] for r in rs)
    return [r for r in rs if r['ii'] * 4 > top]


def cdiv(n, d):
    """C division: truncate toward zero."""
    q = abs(n) // abs(d)
    return q if (n >= 0) == (d > 0) else -q


def rdiv(n, d):
    """Division rounded half away from zero, d > 0."""
    return cdiv(n + (d // 2 if n >= 0 else -(d // 2)), d)


def isqrt_near(v):
    r = math.isqrt(v)
    return r + 1 if v - r * r > r else r


def solve_sum(rs):
    ii = sum(r['ii'] for r in rs)
    qq = sum(r['qq'] for r in rs)
    iq = sum(r['iq'] for r in rs)
    a = rdiv(-(iq << 10), ii)
    bs = []
    for r in rs:
        ar = rdiv(-(r['iq'] << 10), r['ii'])
        bs.append(isqrt_near(rdiv(r['qq'] << 20, r['ii']) - ar * ar))
    return a, -((-sum(bs)) // len(rs)) - 1024


def solve_mean(rs):
    n = len(rs)
    a_sum = sum(rdiv(-(r['iq'] << 16), r['ii']) for r in rs)
    a = rdiv(a_sum, n << 6)
    b_sum = 0
    for r in rs:
        root = math.isqrt(r['qq'] * r['ii'] - r['iq'] * r['iq'])
        b_sum += ((root << 11) + r['ii']) // (r['ii'] << 1)
    return a, (b_sum + n // 2) // n - 1024


def score(pts, verbose):
    for name, fn in (('accumulator sum', solve_sum), ('per-tone mean', solve_mean)):
        da, db, both = Counter(), {c: Counter() for c in range(3)}, 0
        for pt in pts:
            a, b = fn(meas_rounds(pt))
            da[a - pt['a']] += 1
            db[pt['core']][b - pt['b']] += 1
            both += a == pt['a'] and b == pt['b']
            if verbose and (a, b) != (pt['a'], pt['b']):
                print(f"  {name}: {pt['seg']} core {pt['core']} vendor "
                      f"({pt['a']}, {pt['b']}) model ({a}, {b})")
        n = len(pts)
        print(f"{name:16s} a {da[0]:3d}/{n}  b {sum(c[0] for c in db.values()):3d}/{n}"
              f"  both {both:3d}/{n}  b residual per core: "
              + '  '.join(f"c{c} {dict(sorted(db[c].items()))}"
                          for c in range(3) if db[c]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract')
    e.add_argument('segments', nargs='+')
    s = sub.add_parser('score')
    s.add_argument('points')
    s.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    if args.cmd == 'extract':
        pts = []
        for path in args.segments:
            seg = os.path.basename(path).replace('.txt', '')
            pts.extend(extract(seg, path))
        json.dump(pts, sys.stdout)
    else:
        score(json.load(open(args.points)), args.verbose)


if __name__ == '__main__':
    main()
