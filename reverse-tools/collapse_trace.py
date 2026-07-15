#!/usr/bin/env python3
"""Collassa le table-op di un trace wl-diag: rimuove le raw che implementano
ogni TBL.WR/TBL.RD (porta PHY 0x00d id / 0x00e off / 0x00f,0x010,0x011 dati /
0x019e write-gate), lasciando la sola intestazione TBL.

Perche' serve, e come "pulisce": una scrittura di tabella nel driver OEM e' una
sequenza di op sul port (peek 0x019e write-gate + RMW per aprirlo, id su 0x00d,
offset su 0x00e, dati su 0x00f/0x010/0x011, chiusura gate). Il tracer wl-diag
logga anche la RD interna che phy_reg_mod fa prima del PHY.MOD sul gate 0x019e,
e il conteggio/ordine di queste op-di-meccanismo dipende da COME il port carica
la tabella, non dall'effetto. Un confronto posizionale (compare.py) inciampa su
questo (es. vendor MOD-clear del gate vs driver che ci infila la table-write):
falsi mismatch che non sono bug del driver. Rimuovendo le op del port da
ENTRAMBI i lati resta lo scheletro dei register-write reali, confrontabile.

Il flusso di pulizia e' quindi: collassare sia la cattura vendor sia l'output
dell'harness con questo script, poi darli a compare.py. Questo tool accetta i
due formati: la cattura vendor ("<time> #<ep> cpu<n> OP ...") e l'output bare
dell'harness ("cpu<n> OP ..."), preservando l'eventuale #ep per il --range.
Uso: python3 collapse_trace.py <trace.txt> <out.txt>"""
import re, sys
SRC,OUT=sys.argv[1],sys.argv[2]
PORT={0x00d,0x00e,0x00f,0x010,0x011,0x19e}
line=re.compile(r'^(\s*(?:[\d.]+\s+#\d+\s+)?cpu\d+\s+)(\S+)\s+(.*)$')
kept=[];rm=0;tot=0
for ln in open(SRC):
    m=line.match(ln.rstrip('\n'))
    if not m: continue
    tot+=1; op=m.group(2); kv=dict(re.findall(r'(\w+)=(\S+)',m.group(3)))
    if op.startswith('PHY.') and 'addr' in kv:
        try: a=int(kv['addr'],16)
        except: a=None
        if a in PORT: rm+=1; continue
    kept.append(ln.rstrip('\n'))
open(OUT,'w').write(f"# {SRC} - TABLE OPS COLLAPSED ({rm}/{tot} raw rimosse)\n"+'\n'.join(kept)+'\n')
print(f"{OUT}: tenute {len(kept)}, rimosse {rm} su {tot}")
