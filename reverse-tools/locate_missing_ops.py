#!/usr/bin/env python3
"""Dove vanno, nel port, le op che la cattura registra e il port non emette.

Non serve disassemblare: la traccia da' l'ordine, e le op vicine a quella nuova
sono scritture che il port GIA' fa. Si cerca la coppia (indirizzo, valore) del
vicino nel sorgente, e la funzione che la contiene e' il posto dove la nuova op
va inserita.

Uso: locate_missing_ops.py cattura.txt src/phy_ac.c [--classe OBJ.WR] [--max 20]
"""
import argparse
import bisect
import collections
import re
import signal
import sys

signal.signal(signal.SIGPIPE, signal.SIG_DFL)

RX = re.compile(r'\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(\S+)'
                r'(?:\s+addr=0x0*([0-9a-f]+))?(?:\s+val=0x0*([0-9a-f]+))?'
                r'(?:\s+mask=0x0*([0-9a-f]+))?')

# op che il port emette e che quindi si possono cercare nel sorgente
ANCORE = {'PHY.WR', 'PHY.MOD', 'PHY.AND', 'PHY.OR', 'TBL.WR', 'RAD.WR', 'RAD.MOD'}


def indice_funzioni(src):
    """offset di inizio -> nome, per attribuire una posizione a una funzione"""
    f = []
    for m in re.finditer(r'^(?:static\s+)?[a-zA-Z_][\w \t*]*?\b(b43_[a-z0-9_]+)\s*\([^;]*?\)\s*\{',
                         src, re.M):
        f.append((m.start(), m.group(1)))
    f.sort()
    return [o for o, _ in f], [n for _, n in f]


def cerca(src, offs, nomi, op, addr, val):
    """le funzioni che scrivono quella coppia (indirizzo, valore)"""
    if addr is None or val is None:
        return []
    a, v = int(addr, 16), int(val, 16)
    pat = []
    if op.startswith('PHY'):
        pat.append(rf'b43_phy_(?:write|maskset|mod|and|or)\(dev,\s*0x0*{a:x}\b[^;]*?0x0*{v:x}\b')
        pat.append(rf'b43_phy_(?:write|maskset)\(dev,\s*0x0*{a:04x}\b[^;]*?{v}\b')
    elif op.startswith('RAD'):
        pat.append(rf'b43_radio_(?:write|maskset|mask_set)\(dev,\s*0x0*{a:x}\b[^;]*?0x0*{v:x}\b')
    out = []
    for p in pat:
        for m in re.finditer(p, src, re.S):
            i = bisect.bisect_right(offs, m.start()) - 1
            if i >= 0:
                out.append(nomi[i])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cattura')
    ap.add_argument('sorgente')
    ap.add_argument('--classe', default='OBJ.WR')
    ap.add_argument('--max', type=int, default=20)
    ap.add_argument('--finestra', type=int, default=400,
                    help='le op nuove stanno in blocchi: entro poche op ci sono\n'
                         'solo altre op della stessa classe, serve largo')
    args = ap.parse_args()

    src = open(args.sorgente, errors='replace').read()
    offs, nomi = indice_funzioni(src)

    ev = []
    for line in open(args.cattura, errors='replace'):
        m = RX.match(line)
        if m:
            ev.append((m.group(2), m.group(3), m.group(4), m.group(5)))

    bersagli = [i for i, e in enumerate(ev) if e[0] == args.classe]
    print(f"{len(bersagli)} op '{args.classe}' nella cattura, "
          f"{len({e[1] for i, e in enumerate(ev) if e[0] == args.classe})} offset distinti\n")

    # Per ogni occorrenza si prende l'ancora piu' PROSSIMA la cui coppia
    # (indirizzo, valore) compare UNA SOLA volta nel sorgente: un'ancora
    # ambigua non attribuisce niente, e contare tutte quelle nella finestra
    # premia i valori banali come 0x0, che combaciano in decine di punti.
    per_off = collections.defaultdict(collections.Counter)
    cache = {}
    for i in bersagli:
        off = ev[i][1]
        for d in sorted(range(1, args.finestra)) if True else []:
            trovato = False
            for j in (i - d, i + d):
                if not (0 <= j < len(ev)):
                    continue
                op, a, v, _ = ev[j]
                if op not in ANCORE:
                    continue
                k = (op, a, v)
                if k not in cache:
                    cache[k] = cerca(src, offs, nomi, op, a, v)
                fns = set(cache[k])
                if len(fns) == 1:
                    per_off[off][(next(iter(fns)), d)] += 1
                    trovato = True
                    break
            if trovato:
                break

    print(f"{'offset':>8}  {'n':>5}  funzione del port piu' probabile")
    conta = collections.Counter(ev[i][1] for i in bersagli)
    senza = 0
    for off, n in conta.most_common(args.max):
        c = per_off.get(off)
        if not c:
            senza += 1
            print(f"  0x{off:>5}  {n:5}  (nessun vicino riconosciuto)")
            continue
        # aggrega per funzione, tenendo la distanza minima come confidenza
        agg = collections.Counter()
        dist = {}
        for (f, d), k in c.items():
            agg[f] += k
            dist[f] = min(dist.get(f, 10 ** 9), d)
        top = agg.most_common(2)
        print(f"  0x{off:>5}  {n:5}  " +
              ', '.join(f"{f} ({k}, dist {dist[f]})" for f, k in top))
    if senza:
        print(f"\n{senza} offset senza vicini riconoscibili nella finestra "
              f"di {args.finestra} op")


if __name__ == '__main__':
    main()
