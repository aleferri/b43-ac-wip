#!/usr/bin/env python3
"""Verifica gli ancoraggi `#NNNN` dei commenti contro le catture presenti.

I commenti dei sorgenti citano l'indice dell'op nella cattura da cui la
sequenza e' stata trascritta. Un ancoraggio che in nessuna cattura presente
cade su un'op della classe che il codice emette li' e' orfano: punta a una
cattura che non c'e' piu', quindi l'ordinamento che giustifica non e'
verificabile e va riancorato prima di fidarsene.

Uso: verifica_ancoraggi.py <sorgente.c> <cattura.txt> [<cattura.txt> ...]
"""
import collections
import re
import sys

ANCORA = re.compile(r'#(\d{3,7})')
OP = re.compile(r'^\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(\S+)')

# Che classe di op emette una riga di codice, quando si capisce dal nome.
CLASSE = [
    (re.compile(r'b43_maccontrol|b43_mac_enable|b43_mac_suspend'), 'MAC.MCTRL'),
    (re.compile(r'b43_hf_write|b43_hf_read'), 'MAC.MHF'),
    (re.compile(r'b43_phy_(write|read|maskset|set|mask|read_log)\b'), 'PHY'),
    (re.compile(r'b43_radio_(write|read|maskset|set|mask)\b'), 'RAD'),
    (re.compile(r'b43_actab_(write|read)'), 'TBL'),
    (re.compile(r'b43_shm_(write|read)'), 'OBJ'),
]


def classe_attesa(riga):
    for pat, cls in CLASSE:
        if pat.search(riga):
            return cls
    return None


def carica(path):
    """indice -> classe. Le righe RETVAL portano un indice proprio e
    appartengono alla lettura che le precede, quindi si risolvono su quella:
    un ancoraggio che cade sul RETVAL di una lettura ancora quella lettura."""
    d = {}
    for line in open(path, errors='replace'):
        m = OP.match(line)
        if not m:
            continue
        i, cls = int(m.group(1)), m.group(2)
        if cls == 'RETVAL':
            f = re.search(r'for=#(\d+)', line)
            cls = d.get(int(f.group(1)), 'RETVAL') if f else 'RETVAL'
        d[i] = cls
    return d


# Una scrittura di tabella si rende come marcatore TBL piu' le op sulla porta
# dati, quindi un ancoraggio su una TBL puo' cadere su una PHY e va accettato.
EQUIVALENTI = {'TBL': {'TBL', 'PHY'}}


sorgente, catture = sys.argv[1], sys.argv[2:]
indici = {p: carica(p) for p in catture}

righe = open(sorgente, errors='replace').read().splitlines()
orfani = []
ok = collections.Counter()
for n, riga in enumerate(righe, 1):
    for m in ANCORA.finditer(riga):
        a = int(m.group(1))
        # la classe attesa la da' la riga stessa o, se e' un commento puro,
        # la prima riga di codice che segue entro cinque
        att = classe_attesa(riga)
        if att is None:
            for r in righe[n:n + 5]:
                att = classe_attesa(r)
                if att:
                    break
        if att is None:
            continue
        trovata = []
        for p, d in indici.items():
            cls = d.get(a)
            base = att.split('.')[0]
            ammesse = EQUIVALENTI.get(base, {base})
            if cls and cls.split('.')[0] in ammesse:
                trovata.append(p)
        if trovata:
            ok[att] += 1
        else:
            visto = {p.rsplit('/', 1)[-1]: d.get(a, '-') for p, d in indici.items()}
            orfani.append((n, a, att, visto))

print(f"{sum(ok.values())} ancoraggi confermati, {len(orfani)} orfani\n")
for att, k in ok.most_common():
    print(f"  confermati {att:<10} {k}")
print()
for n, a, att, visto in orfani:
    trovate = ' '.join(f"{k}={v}" for k, v in visto.items())
    print(f"  {sorgente}:{n}  #{a} atteso {att:<10} -> {trovate}")
