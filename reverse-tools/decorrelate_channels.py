#!/usr/bin/env python3
"""Decorrela le scritture rispetto a canale e larghezza, su N segmenti.

Prende i segmenti prodotti da `split_trace.py --on chanspec` (o --on gaps),
ognuno etichettato con
(canale, larghezza), e per ogni chiave (op, addr) confronta la SEQUENZA di
valori scritti fra segmenti. Classifica in:

  invariante     stessa sequenza in tutti i segmenti
  solo-canale    dipende dal canale, non dalla larghezza
  solo-larghezza dipende dalla larghezza, non dal canale
  centro-freq    dipende dalla combinazione, cioe' dalla frequenza centrale
  dinamico       varia fra due segmenti con lo STESSO (canale, larghezza),
                 quindi non e' funzione dei due: e' un risultato di
                 calibrazione o uno stato

L'ultima categoria e' la piu' importante da isolare: un valore dinamico non si
puo' tabellare, e il codice ha bisogno di calcolarlo o di leggerlo.

Il confronto e' sulla sequenza, non sull'insieme: lo stesso registro scritto due
volte con valori diversi in ordine diverso e' una differenza.

Uso:
  decorrelate_channels.py --seg dir/seg00-ch36.txt=36,20 [--seg ...] [--csv out.csv]
  decorrelate_channels.py --fase dir=20:36,40,44 [--fase dir2=40:36,44]
"""
import argparse
import collections
import csv
import glob
import os
import re
import sys

RX = re.compile(r'\s*[\d.]+\s+#\d+\s+cpu\d+\s+(\S+)\s+(.*)')
SCRITTURE = {'PHY.WR', 'PHY.MOD', 'PHY.AND', 'PHY.OR', 'TBL.WR',
             'RAD.WR', 'RAD.MOD', 'MAC.MHF', 'PMU.RC', 'PMU.PLL',
             'GPIO.OUT', 'OBJ.WR', 'TPL.RAMW', 'TPL.PTRW', 'TPL.DATW'}


BW_CS = {0x1000: 20, 0x1800: 40, 0x2000: 80, 0x2800: 160}
RX_SHM_CS = re.compile(r'OBJ\.WR\s+addr=0x0*a0\s+val=0x0*([0-9a-f]+)')


def etichetta_da_shm(path):
    """(canale, larghezza) dal chanspec che il driver scrive in shared memory a
    offset 0xa0. E' l'etichetta esatta: non dipende dall'ordine dei segmenti,
    che si sfasa se la cattura ha perso record.

    Si prende la PRIMA scrittura: l'ultima puo' essere il chanspec del ciclo
    successivo, che arriva a fine segmento. Per 40 e 80 MHz il valore porta il
    canale CENTRALE, e viene riportato al canale basso del blocco."""
    for line in open(path, errors='replace'):
        m = RX_SHM_CS.search(line)
        if not m:
            continue
        cs = int(m.group(1), 16)
        bw = BW_CS.get(cs & 0x3800)
        if bw is None:
            continue
        centro = cs & 0xff
        basso = {20: centro, 40: centro - 2, 80: centro - 6, 160: centro - 14}[bw]
        return basso, bw
    return None


def leggi(path):
    """(op, addr, mask) -> tupla dei valori scritti, in ordine"""
    seq = collections.defaultdict(list)
    for line in open(path, errors='replace'):
        m = RX.match(line)
        if not m:
            continue
        op = m.group(1)
        if op not in SCRITTURE:
            continue
        kv = dict(re.findall(r'(\w+)=(\S+)', m.group(2)))
        if kv.get('val') == 'UNDEFINED':
            continue
        key = (op, kv.get('addr', '-'), kv.get('mask', '-'))
        seq[key].append(kv.get('val', '-'))
    return {k: tuple(v) for k, v in seq.items()}


def normalizza(seq):
    """collassa i duplicati consecutivi: lo stesso registro riscritto N volte
    con lo stesso valore non e' una dipendenza, e i segmenti hanno lunghezze
    diverse perche' su alcuni canali il bring-up si ferma prima."""
    if seq is None:
        return None
    out = []
    for v in seq:
        if not out or out[-1] != v:
            out.append(v)
    return tuple(out)


def classifica(per_seg, etich):
    """per_seg: {nome: {key: seq}}, etich: {nome: (ch, bw)}"""
    keys = set()
    for d in per_seg.values():
        keys |= set(d)

    out = {}
    for k in sorted(keys):
        # valore per segmento; assente = None
        val = {n: normalizza(per_seg[n].get(k)) for n in per_seg}
        # dinamico: due segmenti con la stessa etichetta e valore diverso
        per_et = collections.defaultdict(set)
        for n, v in val.items():
            if v is not None:
                per_et[etich[n]].add(v)
        if any(len(s) > 1 for s in per_et.values()):
            out[k] = ('dinamico', val)
            continue

        presenti = {v for v in val.values() if v is not None}
        if len(presenti) <= 1:
            out[k] = ('invariante', val)
            continue

        # stesso insieme di valori, sequenza diversa: tipico dei gate, che si
        # aprono e chiudono un numero di volte diverso. Non e' una dipendenza
        # dal canale, e va tenuto separato per non gonfiare il conteggio.
        if len({frozenset(v) for v in presenti}) == 1:
            out[k] = ('stesso-insieme', val)
            continue

        # dipende dal canale a larghezza fissa?
        per_bw = collections.defaultdict(set)
        per_ch = collections.defaultdict(set)
        for n, v in val.items():
            if v is None:
                continue
            ch, bw = etich[n]
            per_bw[bw].add(v)
            per_ch[ch].add(v)
        varia_con_ch = any(len(s) > 1 for s in per_bw.values())
        varia_con_bw = any(len(s) > 1 for s in per_ch.values())

        if varia_con_ch and not varia_con_bw:
            out[k] = ('solo-canale', val)
        elif varia_con_bw and not varia_con_ch:
            out[k] = ('solo-larghezza', val)
        else:
            out[k] = ('centro-freq', val)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seg', action='append', default=[],
                    help='file=canale,larghezza')
    ap.add_argument('--fase', action='append', default=[],
                    help='dir=larghezza:ch1,ch2,... (in ordine di segmento)')
    ap.add_argument('--auto', action='append', default=[],
                    help='dir: etichetta ogni segmento dal chanspec in SHM')
    ap.add_argument('--minchiavi', type=int, default=0,
                    help='scarta i segmenti con meno chiavi di cosi\': un '
                         'segmento troncato produce false dipendenze')
    ap.add_argument('--csv')
    ap.add_argument('--mostra', default='dinamico,solo-larghezza,solo-canale,centro-freq')
    ap.add_argument('--max', type=int, default=25)
    args = ap.parse_args()

    per_seg, etich = {}, {}

    for spec in args.seg:
        f, lab = spec.rsplit('=', 1)
        ch, bw = lab.split(',')
        per_seg[os.path.basename(f)] = leggi(f)
        etich[os.path.basename(f)] = (int(ch), int(bw))

    for spec in args.auto:
        for f in sorted(glob.glob(os.path.join(spec, 'seg*.txt'))):
            lab = etichetta_da_shm(f)
            n = f"{os.path.basename(spec)}/{os.path.basename(f)}"
            if lab is None:
                print(f"  {n}: nessun chanspec in SHM, salto", file=sys.stderr)
                continue
            per_seg[n] = leggi(f)
            etich[n] = lab

    for spec in args.fase:
        d, lab = spec.rsplit('=', 1)
        bw, chans = lab.split(':')
        chans = chans.split(',')
        files = sorted(glob.glob(os.path.join(d, 'seg*.txt')))
        if len(files) != len(chans):
            print(f"  {d}: {len(files)} segmenti ma {len(chans)} canali, "
                  f"salto (i nomi posizionali non sarebbero affidabili)",
                  file=sys.stderr)
            continue
        for f, ch in zip(files, chans):
            n = f"{os.path.basename(d)}/{os.path.basename(f)}"
            per_seg[n] = leggi(f)
            etich[n] = (int(ch), int(bw))

    if args.minchiavi:
        scarti = [n for n in per_seg if len(per_seg[n]) < args.minchiavi]
        for n in scarti:
            print(f"  scarto {n}: {len(per_seg[n])} chiavi < {args.minchiavi}",
                  file=sys.stderr)
            del per_seg[n]
            del etich[n]

    if len(per_seg) < 2:
        sys.exit("servono almeno 2 segmenti")

    print(f"{len(per_seg)} segmenti, etichette:")
    for n in sorted(per_seg):
        print(f"   {n:34} ch{etich[n][0]:<4} bw{etich[n][1]}  "
              f"{len(per_seg[n])} chiavi")

    res = classifica(per_seg, etich)
    conta = collections.Counter(c for c, _ in res.values())
    print(f"\n{len(res)} chiavi (op, addr, mask) distinte")
    for c in ('invariante', 'stesso-insieme', 'solo-canale', 'solo-larghezza',
              'centro-freq', 'dinamico'):
        print(f"   {c:16} {conta[c]:6}")

    mostra = args.mostra.split(',')
    for c in mostra:
        sel = [(k, v) for k, (cc, v) in res.items() if cc == c]
        if not sel:
            continue
        print(f"\n=== {c}  ({len(sel)} chiavi, prime {min(len(sel), args.max)})")
        for k, val in sel[:args.max]:
            vs = sorted({v for v in val.values() if v is not None},
                        key=lambda t: (len(t), t))
            campione = '; '.join(','.join(x[:6]) for x in vs[:3])
            assenti = sum(1 for v in val.values() if v is None)
            print(f"   {k[0]:8} {k[1]:>8} mask={k[2]:>6}  "
                  f"{len(vs)} valori distinti"
                  + (f", assente in {assenti}" if assenti else "")
                  + f"  [{campione}]")

    if args.csv:
        with open(args.csv, 'w', newline='') as fh:
            w = csv.writer(fh)
            nomi = sorted(per_seg)
            w.writerow(['classe', 'op', 'addr', 'mask'] + nomi)
            for k, (c, val) in sorted(res.items(), key=lambda t: (t[1][0], t[0])):
                w.writerow([c, k[0], k[1], k[2]]
                           + ['' if val[n] is None else ','.join(val[n])
                              for n in nomi])
        print(f"\ntabella completa in {args.csv}")


if __name__ == '__main__':
    main()
