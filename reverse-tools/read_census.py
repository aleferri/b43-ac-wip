#!/usr/bin/env python3
"""For every read in a folded wl-diag trace (with --retvals), look for the write
that consumes its value: the same value written elsewhere (copy), the same
address rewritten with some bits changed (read-modify-write), or a repeat of
the read itself with no write in between (poll). Reads with none are 'unused'.

Output, per read address: count, how the reads are consumed, and the write
targets, so that a discarded read in the port can be checked against what the
vendor does with the value.

Usage: read_census.py FOLDED.txt [--window N] [--addr CLASS:0xADDR]
"""
import argparse
import re
from collections import Counter, defaultdict

OP = re.compile(r'#(\d+)\s+cpu\d\s+(\S+)\s+(.*)$')
KV = re.compile(r'(\w+)=0x([0-9a-f]+)')

TRIVIAL = {0x0000, 0xffff, 0x0001}


def parse(path):
    ops = []
    for line in open(path):
        m = OP.search(line)
        if not m:
            continue
        kv = {k: int(v, 16) for k, v in KV.findall(m.group(3))}
        ops.append((int(m.group(1)), m.group(2), kv))
    # fold table accesses: TBL.RD/TBL.WR marker + data-port ops -> one op per cell
    out = []
    i = 0
    while i < len(ops):
        ep, op, kv = ops[i]
        if op in ('TBL.RD', 'TBL.WR'):
            n = kv.get('len', 1)
            vals = []
            j = i + 1
            while j < len(ops) and len(vals) < n:
                _, op2, kv2 = ops[j]
                if op2 in ('TBL.RD', 'TBL.WR'):
                    break
                if op2 in ('PHY.RD', 'PHY.WR') and kv2.get('addr') in (0xf, 0x10, 0x11):
                    vals.append(kv2['val'])
                j += 1
            if 'val' in kv and not vals:
                vals = [kv['val']]
            for k, v in enumerate(vals):
                out.append((ep, op, ('TBL', (kv['id'], kv['off'] + k)), v))
            i = j if j > i + 1 else i + 1
            # drop the raw data-port ops we consumed
            continue
        if op in ('PHY.RD', 'PHY.WR', 'RAD.RD', 'RAD.WR', 'OBJ.RD', 'OBJ.WR'):
            if kv.get('addr') in (0xd, 0xe, 0xf, 0x10, 0x11) and op.startswith('PHY'):
                i += 1
                continue
            cls = op.split('.')[0]
            out.append((ep, op, (cls, kv.get('addr')), kv.get('val')))
        elif op in ('PHY.MOD', 'RAD.MOD'):
            cls = op.split('.')[0]
            out.append((ep, op, (cls, kv.get('addr')), (kv.get('val'), kv.get('mask'))))
        i += 1
    return out


def census(ops, window):
    res = defaultdict(lambda: dict(n=0, copy=Counter(), rmw=Counter(), poll=0,
                                   unused=0, compare=0, examples=[]))
    for idx, (ep, op, key, val) in enumerate(ops):
        if not op.endswith('.RD'):
            continue
        r = res[key]
        r['n'] += 1
        found = None
        # is the value a candidate for tracing? trivial values match anything
        traceable = val not in TRIVIAL
        for j in range(idx + 1, min(idx + 1 + window, len(ops))):
            ep2, op2, key2, val2 = ops[j]
            if op2.endswith('.RD'):
                if key2 == key and found is None and j - idx <= 3:
                    found = ('poll', None)
                    break
                continue
            if op2.endswith('.WR'):
                if key2 == key:
                    if val2 == val:
                        found = ('rmw', ('=', 0))
                    else:
                        # additive relation first (gain steps), else bit flip
                        d = (val2 - val) & 0xffff
                        found = ('rmw', ('+', d) if d < 0x8000 and d & (d - 1) else
                                 ('xor', val ^ val2))
                    break
                if traceable and val2 == val:
                    found = ('copy', key2)
                    break
                if traceable and isinstance(val, int) and val2 in ((val + 1) & 0xffff, (val - 1) & 0xffff):
                    found = ('copy', key2 + ('+-1',))
                    break
            if op2.endswith('.MOD') and key2 == key:
                found = ('rmw', ('mod', val2[1]))
                break
        if found is None:
            r['unused'] += 1
        elif found[0] == 'poll':
            r['poll'] += 1
        elif found[0] == 'copy':
            r['copy'][found[1]] += 1
        else:
            r['rmw'][found[1]] += 1
        if len(r['examples']) < 2:
            r['examples'].append((ep, val, found))
    return res


def fmt_key(key):
    if key[0] == 'TBL':
        return f"TBL 0x{key[1][0]:04x}[0x{key[1][1]:04x}]"
    return f"{key[0]} 0x{key[1]:04x}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--window', type=int, default=600)
    ap.add_argument('--only', choices=['copy', 'rmw', 'unused', 'all'], default='all')
    args = ap.parse_args()
    ops = parse(args.trace)
    res = census(ops, args.window)
    for key in sorted(res, key=lambda k: (k[0], k[1] if isinstance(k[1], int) else k[1][0] * 0x10000 + k[1][1])):
        r = res[key]
        kinds = []
        if r['copy']:
            kinds.append('copy->' + ','.join(f"{fmt_key(k[:2])}{'~' if len(k) > 2 else ''}x{c}" for k, c in r['copy'].most_common(3)))
        if r['rmw']:
            kinds.append('rmw' + ','.join(f"({m[0]}{'' if m[0]=='=' else '0x%04x' % m[1]})x{c}" for m, c in r['rmw'].most_common(4)))
        if r['poll']:
            kinds.append(f"poll x{r['poll']}")
        if r['unused']:
            kinds.append(f"unused x{r['unused']}")
        dominant = 'copy' if r['copy'] else 'rmw' if r['rmw'] else 'unused' if r['unused'] else 'poll'
        if args.only != 'all' and dominant != args.only:
            continue
        ex = ' '.join(f"#{e} 0x{v:04x}" for e, v, _ in r['examples'] if isinstance(v, int))
        print(f"{fmt_key(key):22s} n={r['n']:3d}  {' | '.join(kinds):60s} e.g. {ex}")


if __name__ == '__main__':
    main()
