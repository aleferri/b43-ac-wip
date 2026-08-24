#!/usr/bin/env python3
"""Da dove viene un valore che il driver scrive.

Il problema ricorrente: il port ha una costante cablata da una cattura, il
vendor scrive qualcosa che cambia a ogni corsa, e va trovato da cosa dipende.
Provare una forma per volta a mano non scala e porta a concludere troppo presto
su un caso singolo.

Questo cerca su tutti i candidati x tutte le trasformazioni, e riporta solo
quelle che tengono su TUTTI i casi -- o quasi, con la frazione esplicita.

Candidati: ogni valore letto e ogni valore scritto nel segmento, piu' gli stessi
nel segmento PRECEDENTE (stato che attraversa i cicli).

Trasformazioni: identita', +k, -k, xor k, and 0xff, >>8, scambio byte, not,
shift 1..12, e le coppie (a<<8)|b su valori che stanno in un byte.

Un avvertimento sull'interpretazione, imparato a caro prezzo: con abbastanza
candidati qualcosa combacia per caso. Lo strumento riporta quante trasformazioni
sono state provate, cosi' si puo' pesare un successo contro il numero di prove.
E una relazione che tiene su tutti i casi con una costante ricavata da uno solo
non e' una scoperta: e' un fit. Serve che la costante sia la stessa per tutti.

Uso: come modulo, oppure

    trova_sorgente.py --segmenti dir --bersaglio 'REGEX con un gruppo'
"""
import argparse
import collections
import glob
import os
import re
import signal
import sys

signal.signal(signal.SIGPIPE, signal.SIG_DFL)

RX_OP = re.compile(r'\s*[\d.]+\s+#\d+\s+cpu\d+\s+(\S+)\s+(.*)$')
RX_VAL = re.compile(r'(?:addr|id|off)=0x0*([0-9a-f]+).*?val=0x0*([0-9a-f]+)')

M16 = 0xffff


def trasformazioni():
    """(nome, funzione a un argomento) -- quelle senza costante libera"""
    t = [('v', lambda v: v),
         ('~v', lambda v: (~v) & M16),
         ('v & 0xff', lambda v: v & 0xff),
         ('v >> 8', lambda v: (v >> 8) & 0xff),
         ('scambio byte', lambda v: ((v & 0xff) << 8) | ((v >> 8) & 0xff))]
    for n in range(1, 13):
        t.append((f'v >> {n}', lambda v, n=n: v >> n))
        t.append((f'v << {n}', lambda v, n=n: (v << n) & M16))
    return t


def con_costante():
    """(nome, funzione a due argomenti) -- k si ricava dal primo caso e va
    verificata identica su tutti gli altri"""
    return [('v + k', lambda v, k: (v + k) & M16),
            ('v - k', lambda v, k: (v - k) & M16),
            ('v ^ k', lambda v, k: v ^ k),
            ('k - v', lambda v, k: (k - v) & M16)]


def cerca(casi, min_frazione=1.0):
    """casi: lista di (bersaglio, {nome_sorgente: [valori]})

    Ritorna una lista di (descrizione, frazione di casi spiegati)."""
    nomi = set()
    for _, src in casi:
        nomi |= set(src)
    out = []
    prove = 0

    for nome in sorted(nomi):
        # senza costante libera
        for etichetta, f in trasformazioni():
            prove += 1
            n = 0
            for bersaglio, src in casi:
                if any(f(v) == bersaglio for v in src.get(nome, ())):
                    n += 1
            if n / len(casi) >= min_frazione:
                out.append((f'{nome}: {etichetta}', n / len(casi), None))

        # con una costante, che deve essere LA STESSA per tutti
        for etichetta, f in con_costante():
            prove += 1
            ks = None
            for bersaglio, src in casi:
                # k si ricava invertendo la trasformazione, e si tiene
                # l'intersezione fra i casi: se resta vuota, non esiste una
                # costante uniforme.
                qui = set()
                for v in src.get(nome, ()):
                    if etichetta == 'v + k':
                        qui.add((bersaglio - v) & M16)
                    elif etichetta == 'v - k':
                        qui.add((v - bersaglio) & M16)
                    elif etichetta == 'v ^ k':
                        qui.add(v ^ bersaglio)
                    elif etichetta == 'k - v':
                        qui.add((bersaglio + v) & M16)
                ks = qui if ks is None else (ks & qui)
                if not ks:
                    break
            if ks:
                out.append((f'{nome}: {etichetta}', 1.0, sorted(ks)[:3]))

    return out, prove


def leggi_segmento(path):
    """{nome sorgente: [valori]} per un segmento"""
    src = collections.defaultdict(list)
    for line in open(path, errors='replace'):
        m = RX_OP.match(line)
        if not m:
            continue
        op, resto = m.group(1), m.group(2)
        mm = RX_VAL.search(resto)
        if mm:
            src[f'{op} 0x{int(mm.group(1), 16):04x}'].append(int(mm.group(2), 16))
    return dict(src)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--segmenti', required=True)
    ap.add_argument('--bersaglio', required=True,
                    help='regex con un gruppo che cattura il valore, in hex')
    ap.add_argument('--frazione', type=float, default=1.0)
    ap.add_argument('--precedente', action='store_true',
                    help='includi anche le sorgenti del segmento prima')
    args = ap.parse_args()

    rxb = re.compile(args.bersaglio)
    files = sorted(glob.glob(os.path.join(args.segmenti, '*.txt')))
    casi = []
    prec = {}
    for f in files:
        bersaglio = None
        for line in open(f, errors='replace'):
            m = rxb.search(line)
            if m:
                bersaglio = int(m.group(1), 16)
                break
        src = leggi_segmento(f)
        if bersaglio is not None:
            unito = dict(src)
            if args.precedente:
                for k, v in prec.items():
                    unito['PREC ' + k] = v
            casi.append((bersaglio, unito))
        prec = src

    if not casi:
        sys.exit('nessun caso: il bersaglio non compare')

    print(f'{len(casi)} casi, bersagli distinti: '
          f'{len({b for b, _ in casi})}')
    ris, prove = cerca(casi, args.frazione)
    print(f'{prove} trasformazioni provate\n')
    if not ris:
        print('  nessuna relazione tiene sulla frazione richiesta')
        return
    for desc, fraz, k in sorted(ris, key=lambda t: -t[1])[:20]:
        kk = f'   k = {[hex(x) for x in k]}' if k else ''
        print(f'  {fraz * 100:5.1f}%  {desc}{kk}')
    print(f'\nPesare i successi contro le {prove} prove: con abbastanza')
    print('candidati qualcosa combacia per caso.')


if __name__ == '__main__':
    main()
