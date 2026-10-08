#!/usr/bin/env python3
"""RX-IQ solve points from the sweeps, and the score of the solver on them.

A point is one write of PHY 0x?a0/0x?a1 with a non-zero value, paired with the
accumulator rounds (six reads 0x?c0-0x?c5) of the same core that precede it
since the previous write. Rounds preceded by a touch of PHY 0x0b22 belong to
the loopback gain search; among the others, the short estimate (0x0272 =
0x400 samples, a sixteenth of the power) is dropped too. What remains are the
measurement tones: two up to 40 MHz, six at 80.

Three models are scored: the one the driver first had, summing the
accumulators over the tones; the mean over the tones of the per-tone (a, b);
and the one it has now, polar, the stock driver's: per tone an angle and a
magnitude 2^10 sqrt(qq/ii) in its fixed point, both averaged, then the
CORDIC back to (a, b) -- see b43_phy_ac_iq_mismatch() and
b43_phy_ac_iq_solve() in b43/phy_ac.c. The CORDIC table is the one of
lib/math/cordic.c.

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
    a_sum = 0
    for r in rs:
        k = max(r['ii'].bit_length() - 16, 0)
        a_sum += rdiv(-((r['iq'] >> k) << 24), r['ii'] >> k)
    a = rdiv(a_sum, n << 14)
    b_sum = 0
    for r in rs:
        root = math.isqrt(r['qq'] * r['ii'] - r['iq'] * r['iq'])
        b_sum += ((root << 11) + r['ii']) // (r['ii'] << 1)
    return a, (b_sum + n // 2) // n - 1024


ATAN_Q16 = [2949120, 1740967, 919879, 466945, 234379, 117304, 58666, 29335,
            14668, 7334, 3667, 1833, 917, 458, 229, 115, 57, 29]
CORDIC_GAIN = 39797


def cordic(theta):
    """Degrees Q16 -> (cos, sin) in Q16, |theta| < 90."""
    i, q, angle = CORDIC_GAIN, 0, 0
    for k, at in enumerate(ATAN_Q16):
        if theta > angle:
            i, q = i - (q >> k), q + (i >> k)
            angle += at
        else:
            i, q = i + (q >> k), q - (i >> k)
            angle -= at
    return i, q


def inv_cordic(y, x):
    """Vector (x > 0, y), both under 2^27 -> its angle in degrees Q16."""
    y <<= 4
    x <<= 4
    angle = 0
    for k, at in enumerate(ATAN_Q16):
        ty, tx = y >> k, x >> k
        if y >= 0:
            angle += at
            y, x = y - tx, x + ty
        else:
            angle -= at
            y, x = y + tx, x - ty
    return angle


def div_near(n, d):
    """Numerator biased by half the denominator, then truncated."""
    return cdiv(n + (d >> 1 if n > 0 else -(d >> 1)), d)


def iq_mismatch(iq, ii, qq):
    """One tone's mismatch: angle in degrees Q16 and 2^10 sqrt(qq/ii), in
    the stock driver's fixed point (see b43_phy_ac_iq_mismatch())."""
    nb_iq, nb_max = abs(iq).bit_length(), (ii | qq).bit_length()
    if nb_max < 31:
        prod = isqrt_near(qq << (30 - nb_max)) * isqrt_near(ii << (30 - nb_max))
    else:
        prod = isqrt_near(qq >> (nb_max - 30)) * isqrt_near(ii >> (nb_max - 30))
    num = (-iq) << (30 - nb_iq)
    den = prod << (nb_max - nb_iq - 16) if nb_iq + 16 < nb_max \
        else prod >> (nb_iq + 16 - nb_max)
    sin_q16 = div_near(num, den) if den else 0
    half = sin_q16 >> 1
    cos_q16 = isqrt_near((1 << 30) - half * half) << 1
    angle = inv_cordic(sin_q16, cos_q16)
    d = qq - ii
    nb = abs(d).bit_length()
    nb += nb & 1
    num = d << (30 - nb)
    den = ii >> (nb - 10) if nb >= 11 else ii << (10 - nb)
    mag = isqrt_near(div_near(num, den) + (1 << 20)) if den else 1 << 10
    return angle, mag


def solve_polar(rs):
    n = len(rs)
    ang_sum = mag_sum = 0
    for r in rs:
        angle, mag = iq_mismatch(r['iq'], r['ii'], r['qq'])
        ang_sum += angle
        mag_sum += mag
    mag, angle = div_near(mag_sum, n), div_near(ang_sum, n)
    c, s = cordic(angle)
    return (mag * s + (1 << 15)) >> 16, ((mag * c + (1 << 15)) >> 16) - 1024


def score(pts, verbose):
    for name, fn in (('accumulator sum', solve_sum), ('per-tone mean', solve_mean),
                     ('polar (cordic)', solve_polar)):
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
