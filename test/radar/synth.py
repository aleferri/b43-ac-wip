#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""synth.py OUTDIR: radar FIFO reads for replay, synthesized.

Writes OUTDIR/<type>-<seed>.txt, one burst of a radar test signal with its
parameters drawn over the standard's ranges, and OUTDIR/noise-<rate>-<seed>.txt,
random pulses at <rate> a second for 60 s. The words are as the RT-AC66U B1's
FIFOs read known pulses (router-data/rt-ac66u-b1/radar-fifo.zip):

- times in 12.5 ns units, +-4 units of jitter, from a random start (so that
  some bursts cross the 32-bit wrap);
- widths in 0.2 us units: pulses under 1.5 us read 1.2-2.0 us, longer ones
  0-0.2 us over; fifo 1 one unit more; pulses past 27 us split in two entries
  (27.0 us, and 28.8 us after the start the rest);
- a fraction of the pulses missed (MISS, default 0.3), each FIFO on its own;
- read in polls every 150 ms, at a random phase.
"""
import os
import random
import sys

TICKS_US = 80
POLL_US = 150000

# name: (width us lo, hi, PRI us lo, hi, pulses lo, hi, chirp)
TYPES = {
    'fcc0': (1, 1, 1428, 1428, 18, 18),
    'fcc1': (1, 1, 518, 3066, None, None),
    'fcc2': (1, 5, 150, 230, 23, 29),
    'fcc3': (6, 10, 200, 500, 16, 18),
    'fcc4': (11, 20, 200, 500, 12, 16),
    'etsi1': (0.5, 5, 1000, 5000, 10, 10),
    'etsi2': (0.5, 15, 625, 5000, 15, 15),
    'etsi3': (0.5, 15, 250, 435, 25, 25),
    'etsi4': (20, 30, 250, 500, 20, 20),
}

# Staggered PRF (ETSI EN 301 893 Table D.4, reference signals 5 and 6): the
# pulses take 2 or 3 PRFs in turn, each PRF differing from the others by the
# least spacing given, and each PRF has the count of pulses of a burst.
# name: (width us lo, hi, PRF pps lo, hi, least PRF spacing, pulses per PRF)
STAGGERED = {
    'etsi5': (0.5, 2, 300, 400, 20, 10),
    'etsi6': (0.5, 2, 400, 1200, 80, 15),
}


def width_units(pw, rnd):
    if pw < 1.5:
        return rnd.randint(6, 10)
    return round(pw * 5) + rnd.randint(0, 1)


def entries(t_us, pw, rnd):
    """-> [(ts, width)] for one pulse as one FIFO reads it"""
    ts = int(t_us * TICKS_US) + rnd.randint(-4, 4)
    if pw <= 27:
        return [(ts, width_units(pw, rnd))]
    return [(ts, 135), (ts + int(28.8 * TICKS_US), max(1, round((pw - 28.8) * 5) + 1))]


def write(path, pulses, rnd, miss):
    """pulses: [(t_us, pw_us)], t from 0"""
    start = rnd.randrange(1 << 32)
    phase = rnd.uniform(0, POLL_US)
    fifo = [[], []]
    for t, pw in pulses:
        for f in (0, 1):
            if rnd.random() < miss:
                continue
            for ts, w in entries(t, pw, rnd):
                fifo[f].append((t, (start + ts) & 0xffffffff, min(255, w + f)))
    end = max(t for t, _ in pulses) if pulses else 0
    nxt = [0, 0]
    with open(path, 'w') as o:
        poll_t = -phase
        while poll_t < end + POLL_US:
            poll_t += POLL_US
            o.write('poll\n')
            for f in (0, 1):
                while nxt[f] < len(fifo[f]) and fifo[f][nxt[f]][0] < poll_t:
                    t, ts, w = fifo[f][nxt[f]]
                    nxt[f] += 1
                    w0, w1, w3 = ts >> 16, (ts >> 4) & 0xfff, ts & 0xf
                    for word in (w0, w1, 0xff00 | w, 0x20 | w3):
                        o.write(f'{f} {word:04x}\n')


def burst(kind, rnd):
    wlo, whi, plo, phi, nlo, nhi = TYPES[kind]
    pw = rnd.uniform(wlo, whi)
    pri = rnd.uniform(plo, phi)
    n = rnd.randint(nlo, nhi) if nlo else -(-19e6 // (360 * pri))
    lead = rnd.uniform(0, 100000)
    return [(lead + i * pri, pw) for i in range(int(n))]


def staggered(kind, rnd):
    wlo, whi, flo, fhi, gap, per = STAGGERED[kind]
    pw = rnd.uniform(wlo, whi)
    nprf = rnd.choice((2, 3))
    while True:
        prfs = [rnd.uniform(flo, fhi) for _ in range(nprf)]
        if all(abs(a - b) >= gap for i, a in enumerate(prfs) for b in prfs[i + 1:]):
            break
    pris = [1e6 / f for f in prfs]
    t = rnd.uniform(0, 100000)
    out = []
    for i in range(per * nprf):
        out.append((t, pw))
        t += pris[i % nprf]
    return out


def noise(rate, rnd, secs=60):
    t, out = 0.0, []
    while t < secs * 1e6:
        t += rnd.expovariate(rate / 1e6)
        out.append((t, rnd.uniform(0.2, 40)))
    return out


def main():
    out = sys.argv[1]
    miss = float(os.environ.get('MISS', '0.3'))
    seeds = int(os.environ.get('SEEDS', '50'))
    os.makedirs(out, exist_ok=True)
    for kind in TYPES:
        for s in range(seeds):
            rnd = random.Random(f'{kind}-{s}')
            write(f'{out}/{kind}-{s}.txt', burst(kind, rnd), rnd, miss)
    for kind in STAGGERED:
        for s in range(seeds):
            rnd = random.Random(f'{kind}-{s}')
            write(f'{out}/{kind}-{s}.txt', staggered(kind, rnd), rnd, miss)
    for rate in (20, 100, 500, 2000):
        for s in range(3):
            rnd = random.Random(f'noise-{rate}-{s}')
            write(f'{out}/noise-{rate}-{s}.txt', noise(rate, rnd), rnd, 0)


if __name__ == '__main__':
    main()
