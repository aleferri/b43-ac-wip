#!/usr/bin/env python3
"""La fase e' assente oltre una soglia, o solo un suo registro?

Per ogni funzione del port elenca TUTTI gli indirizzi che emette e conta
quante volte il vendor li tocca sotto e sopra la soglia. Una fase e' candidata
al gate se ogni indirizzo che le e' proprio sta a zero sopra; se ne sta a zero
uno solo, quello che si e' trovato e' un ramo dentro la fase.

Il controesempio da tenere a mente e' idle_tssi_meas: i suoi tre registri
esclusivi sono a zero sopra i 5250 MHz mentre la fase gira, 192 letture di
PHY 0x0013 contro 198. Un testimone esclusivo a zero prova assente il
registro, non la fase.

Gli indirizzi ubiqui -- le porte delle tabelle 0x000d/0x000e/0x000f/0x0010 e
il gate 0x019e -- non discriminano niente e sono esclusi dal rapporto: per una
fase che lavora attraverso una porta dati l'identita' e' la tabella, che si
guarda con --tabelle.

Uso:
  fase_absente.py <port-fn-markers.txt> <vendor-sotto.txt> <vendor-sopra.txt>
                  [--min N] [--fn NOME] [--tabelle]
"""
import argparse
import collections
import re

ENTER = re.compile(r'^-+FN:(\S+?)-+$')
LEAVE = re.compile(r'^-+/FN:(\S+?)-+$')
ADDR = re.compile(r'addr=(0x[0-9a-f]+)')
CLS = re.compile(r'\b([A-Z]{2,5})\.[A-Z]+\b')
TBL = re.compile(r'(TBL\.(?:WR|RD)) +(id=0x[0-9a-f]+) +(off=0x[0-9a-f]+)')

# Porte e gate che ogni fase attraversa: presenti sempre, quindi non dicono
# niente sull'assenza di una fase.
UBIQUI = {('PHY', '0x000d'), ('PHY', '0x000e'), ('PHY', '0x000f'),
          ('PHY', '0x0010'), ('PHY', '0x019e')}


def chiave(line, tabelle):
    if tabelle:
        m = TBL.search(line)
        if m:
            return (m.group(2), m.group(3))
        return None
    a, c = ADDR.search(line), CLS.search(line)
    return (c.group(1), a.group(1)) if a and c else None


def per_funzione(path, tabelle):
    emesse = collections.defaultdict(collections.Counter)
    pila = []
    for line in open(path, errors='replace'):
        s = line.strip()
        m = ENTER.match(s)
        if m:
            pila.append(m.group(1))
            continue
        m = LEAVE.match(s)
        if m:
            if m.group(1) in pila:
                while pila.pop() != m.group(1):
                    pass
            continue
        k = chiave(line, tabelle)
        if k and pila:
            emesse[pila[-1]][k] += 1
    return emesse


def conta(path, tabelle):
    c = collections.Counter()
    for line in open(path, errors='replace'):
        k = chiave(line, tabelle)
        if k:
            c[k] += 1
    return c


ap = argparse.ArgumentParser()
ap.add_argument('port')
ap.add_argument('sotto')
ap.add_argument('sopra')
ap.add_argument('--min', type=int, default=1,
                help="ignora le fasi con meno di N op nel port")
ap.add_argument('--fn', help="dettaglio indirizzo per indirizzo di una fase")
ap.add_argument('--tabelle', action='store_true',
                help="usa (id, off) della tabella invece dell'indirizzo")
args = ap.parse_args()

emesse = per_funzione(args.port, args.tabelle)
vs, va = conta(args.sotto, args.tabelle), conta(args.sopra, args.tabelle)

if args.fn:
    e = emesse[args.fn]
    print(f"{args.fn}: {len(e)} chiavi, {sum(e.values())} op nel port")
    print(f"  {'chiave':<22} {'port':>5} {'sotto':>7} {'sopra':>7}")
    for k, n in e.most_common():
        u = ' (ubiquo)' if k in UBIQUI else ''
        print(f"  {k[0]:<6}{k[1]:<16} {n:>5} {vs.get(k, 0):>7} {va.get(k, 0):>7}{u}")
    raise SystemExit

righe = []
for fn, e in emesse.items():
    nop = sum(e.values())
    if nop < args.min:
        continue
    propri = [k for k in e if k not in UBIQUI]
    if not propri:
        righe.append((fn, nop, 0, 0, 0))
        continue
    zero = sum(1 for k in propri if va.get(k, 0) == 0)
    # sotto la soglia la fase deve esserci, o il confronto non dice niente
    vivi = sum(1 for k in propri if vs.get(k, 0) > 0)
    righe.append((fn, nop, len(propri), zero, vivi))

print(f"{'funzione':<46} {'op':>6} {'propri':>7} {'a zero':>7} {'vivi sotto':>11}")
print('-' * 82)
pieno = [r for r in righe if r[2] and r[3] == r[2] and r[4] == r[2]]
quasi = [r for r in righe if r[2] and r[2] > r[3] >= r[2] * 0.6]
resto = [r for r in righe if r not in pieno and r not in quasi]

for titolo, gruppo in (("assente per intero: candidata al gate", pieno),
                       ("assente in maggioranza: guardare il dettaglio", quasi),
                       ("gira sopra la soglia, o senza indirizzi propri", resto)):
    print(f"\n### {titolo}")
    for fn, nop, p, z, v in sorted(gruppo, key=lambda r: -r[1]):
        print(f"{fn:<46} {nop:>6} {p:>7} {z:>7} {v:>11}")
