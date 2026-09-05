#!/usr/bin/env python3
"""Per ogni funzione del port, cerca un testimone esclusivo e contalo nel
vendor sotto e sopra una soglia di frequenza.

Il metodo e' quello di test/README.md §6: il testimone di una fase e' un
registro che nel port tocca solo quella funzione. Trova le fasi che ne hanno
uno e tace su quelle che condividono tutti i loro registri, che e' un limite
del metodo e non un risultato.

I marcatori di AC_FN_MARKERS=1 sono una coppia, `----FN:x----` in entrata e
`----/FN:x----` in uscita, e annidano. Leggere solo l'entrata attribuisce al
chiamato tutto cio' che il chiamante emette dopo il ritorno: cca_pulse tocca
un registro solo e ne risulterebbe proprietaria di mille op. Qui si tiene la
pila e ogni op va alla funzione piu' interna attiva.

Uso: witness_scan.py <port-fn-markers.txt> <vendor-sotto.txt> <vendor-sopra.txt>
"""
import collections
import re
import sys

ENTER = re.compile(r'^-+FN:(\S+?)-+$')
LEAVE = re.compile(r'^-+/FN:(\S+?)-+$')
ADDR = re.compile(r'addr=(0x[0-9a-f]+)')
CLS = re.compile(r'\b([A-Z]{2,5})\.[A-Z]+\b')


def chiave(line):
    a = ADDR.search(line)
    c = CLS.search(line)
    return (c.group(1), a.group(1)) if a and c else None


def per_funzione(path):
    """(classe, addr) -> funzioni che lo toccano, con la pila dei marcatori."""
    owner = collections.defaultdict(set)
    conta = collections.defaultdict(collections.Counter)
    pila = []
    for line in open(path, errors='replace'):
        s = line.strip()
        m = ENTER.match(s)
        if m:
            pila.append(m.group(1))
            continue
        m = LEAVE.match(s)
        if m:
            # Tollera una pila sbilanciata invece di fidarsi: se il nome in
            # cima non e' quello che esce, si srotola fino a trovarlo.
            if m.group(1) in pila:
                while pila.pop() != m.group(1):
                    pass
            continue
        k = chiave(line)
        if k is None or not pila:
            continue
        owner[k].add(pila[-1])
        conta[pila[-1]][k] += 1
    return owner, conta


def conta_vendor(path):
    c = collections.Counter()
    for line in open(path, errors='replace'):
        k = chiave(line)
        if k:
            c[k] += 1
    return c


port, sotto, sopra = sys.argv[1], sys.argv[2], sys.argv[3]
owner, conta = per_funzione(port)
vs, va = conta_vendor(sotto), conta_vendor(sopra)

righe = []
for fn, keys in conta.items():
    esclusivi = [k for k in keys if owner[k] == {fn}]
    if not esclusivi:
        righe.append((fn, None, 0, 0, 0))
        continue
    k = max(esclusivi, key=lambda x: keys[x])
    righe.append((fn, k, keys[k], vs.get(k, 0), va.get(k, 0)))

salta = [r for r in righe if r[1] and r[3] > 0 and r[4] == 0]
gira = [r for r in righe if r[1] and r[3] > 0 and r[4] > 0]
assente = [r for r in righe if r[1] and r[3] == 0]
muto = [r for r in righe if not r[1]]


def stampa(titolo, gruppo):
    print(f"\n### {titolo}")
    if not gruppo:
        print("  (nessuna)")
        return
    print(f"  {'funzione':<44} {'testimone':<14} {'port':>5} {'sotto':>6} {'sopra':>6}")
    for fn, k, n, b, a in sorted(gruppo, key=lambda r: -r[2]):
        print(f"  {fn:<44} {k[0]:<4}{k[1]:<10} {n:>5} {b:>6} {a:>6}")


stampa("il vendor NON la esegue sopra la soglia: candidata al gate", salta)
stampa("la esegue da entrambe le parti: NON gatare", gira)
stampa("testimone assente anche sotto: op del port senza oracolo", assente)
print(f"\n### senza testimone esclusivo, il metodo non dice niente: {len(muto)}")
for fn, *_ in sorted(muto):
    print(f"  {fn}")
