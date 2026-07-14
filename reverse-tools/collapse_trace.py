#!/usr/bin/env python3
"""Collassa le table-op di un trace wl-diag: rimuove le raw che implementano
ogni TBL.WR/TBL.RD (porta PHY 0x00d id / 0x00e off / 0x00f,0x010,0x011 dati /
0x019e write-gate), lasciando la sola intestazione TBL.
Uso: python3 collapse_trace.py <trace.txt> <out.txt>"""
import re, sys
SRC,OUT=sys.argv[1],sys.argv[2]
PORT={0x00d,0x00e,0x00f,0x010,0x011,0x19e}
line=re.compile(r'^(\s*[\d.]+\s+#\d+\s+cpu\d+\s+)(\S+)\s+(.*)$')
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
