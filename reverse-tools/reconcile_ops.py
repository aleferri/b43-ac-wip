#!/usr/bin/env python3
"""
Riconcilia le operazioni del port (./*.c) contro un trace
wl-diag CIECO: senza euristiche di code-smell, confrontando l'EFFETTO-BIT di
ogni singola operazione (quali bit setta / quali pulisce), non l'insieme dei
tipi-op per indirizzo.

Perche' cieco: il tracer distingue WR/OR/AND/MOD. Ogni op si normalizza a una
firma d'effetto:
  WR(v)      -> ('WR', v)                 (scrittura piena)
  SET/OR(m)  -> (set=m,  clr=0)
  AND(k)     -> (set=0,  clr=~k)          (reg&=k pulisce ~k)
  MOD(v,m)   -> (set=v&m, clr=m&~v)
Una op del port e' OK se la sua firma d'effetto ESISTE nel trace a
quell'indirizzo. Se non esiste:
  - OR/AND-SWAP: port SET(m) mentre il trace fa AND con lo STESSO letterale m
    (clr==~m) -> lo stesso bug di 0x0471/0x0460 (letterale AND scritto come OR).
    Rilevato senza sapere nulla del pattern maskset(~0x0000): e' "cieco".
  - WR-vs-PARZIALE: port scrive pieno dove il trace tocca solo bit -> clobber.
  - VAL-DIFF: stesso tipo/bit ma valore diverso (spesso canale/DYN).
  - PORT-EXTRA: tocca bit che nessuna op del trace tocca.

Collassa la RD+WR interna del radio-mod (il radio logga mod + la sua RMW) ed
esclude le table-port PHY 0x0d..0x11 + write-gate 0x19e. Fold per-core (+0x200,
+0x400) solo sulle pagine realmente bandizzate.
Uso: python3 reconcile_ops.py <trace.txt> [repo_root] [--src-from-git REF]
"""
import re, glob, os, sys, csv, subprocess
HERE=os.path.dirname(os.path.abspath(__file__))
args=[a for a in sys.argv[1:] if not a.startswith('--')]
gitref=None
if '--src-from-git' in sys.argv:
    gitref=sys.argv[sys.argv.index('--src-from-git')+1]
TRACE=args[0] if args else os.path.join(HERE,"../router-data/d6220/wl-diag-wl1-attach-to-bss-ch36.txt")
ROOT=args[1] if len(args)>1 else os.path.dirname(HERE)
SRCDIR=os.path.join(ROOT,"src")
DECO=os.path.join(ROOT,"reverse-output/decorrelation-ch-vs-bw.csv")
OUT=os.path.join(ROOT,"reverse-output/op-reconciliation-ch36.csv")
EXCL={0x0d,0x0e,0x0f,0x10,0x11,0x19e}
def banded(fam,a):
    if fam=='PHY': return 0x0600<=a<=0x0bff or 0x1600<=a<=0x1bff
    return 0x0000<=a<=0x05ff or 0x0800<=a<=0x0dff

def srcfiles():
    fs=sorted(glob.glob(SRCDIR+"/*.c")+glob.glob(SRCDIR+"/*.h"))
    if not gitref: return {f:open(f,errors="ignore").read() for f in fs}
    out={}
    for f in fs:
        rel=os.path.relpath(f,ROOT)
        try: out[f]=subprocess.check_output(["git","-C",ROOT,"show",f"{gitref}:{rel}"],text=True,stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError: out[f]=""
    return out
SRC=srcfiles()
sym={}
for _,txt in SRC.items():
    for l in txt.splitlines():
        m=re.match(r"\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\b",l)
        if m: sym.setdefault(m.group(1),int(m.group(2),16))
def val(t):
    t=t.strip(); m=re.match(r"^\(u16\)\s*~\s*(.+)$",t)
    if m: i=val(m.group(1)); return None if i is None else (~i)&0xffff
    if t.startswith("~"): i=val(t[1:]); return None if i is None else (~i)&0xffff
    if re.fullmatch(r"0x[0-9a-fA-F]+",t): return int(t,16)
    if re.fullmatch(r"\d+",t): return int(t)
    return sym.get(t)
def sa(s):
    o=[];d=0;c=""
    for ch in s:
        if ch=='(':d+=1;c+=ch
        elif ch==')':
            if d==0:o.append(c);return o
            d-=1;c+=ch
        elif ch==',' and d==0:o.append(c);c=""
        else:c+=ch
    o.append(c);return o
# effetto canonico: ('WR',v) | ('P', set, clr)  (set/clr None se valore ignoto)
def e_wr(v): return ('WR',v)
def e_or(m): return ('P', m, 0) if m is not None else ('P',None,0)
def e_and(k): return ('P', 0, (~k)&0xffff) if k is not None else ('P',0,None)
def e_mod(v,m):
    if m is None: return ('P',None,None)
    if v is None: return ('P',None,m)   # bit toccati = m, split ignoto
    return ('P', v&m, m&~v)
CALL=re.compile(r"\bb43_(phy|radio)_(write|set|mask|maskset)\s*\(")
port={}
for _,txt in SRC.items():
    for l in txt.splitlines():
        for m in CALL.finditer(l):
            fam="PHY" if m.group(1)=="phy" else "RAD"; k=m.group(2)
            a=[x.strip() for x in sa(l[m.end():])]
            if len(a)<2: continue
            ad=val(a[1])
            if ad is None or (fam=='PHY' and ad in EXCL): continue
            s=port.setdefault((fam,ad),set())
            if   k=="write"   and len(a)>=3: s.add(e_wr(val(a[2])))
            elif k=="set"     and len(a)>=3: s.add(e_or(val(a[2])))
            elif k=="mask"    and len(a)>=3: s.add(e_and(val(a[2])))
            elif k=="maskset" and len(a)>=4:
                keep=val(a[2]); st=val(a[3])
                if st is None: s.add(('P',None,(~(keep if keep is not None else 0xffff))&0xffff))
                else: s.add(('P', st, (~(keep if keep is not None else 0xffff)) & ~st & 0xffff))
# trace
lp=re.compile(r'#(\d+)\s+cpu\d+\s+(\S+)\s+(.*)')
recs=[(mm.group(2),mm.group(3)) for mm in (lp.search(l) for l in open(TRACE)) if mm]
recs=[(o.split('.',1)[0],o.split('.',1)[1],dict(re.findall(r'(\w+)=(0x[0-9a-fA-F]+|UNDEFINED)',r)))
      for o,r in recs if '.' in o]
trace={}; i=0; coll=0
while i<len(recs):
    fam,cls,kv=recs[i]
    if fam=='RAD' and cls in ('MOD','OR','AND') and 'addr' in kv:
        a=int(kv['addr'],16); j=i+1
        while j<len(recs) and recs[j][0]=='RAD' and recs[j][1] in ('RD','WR') and recs[j][2].get('addr')==kv['addr']:
            coll+=1; j+=1
        v=int(kv['val'],16); mk=int(kv.get('mask','0x0'),16)
        trace.setdefault(('RAD',a),set()).add(e_mod(v,mk) if cls=='MOD' else (e_or(v) if cls=='OR' else e_and((~mk)&0xffff)))
        i=j; continue
    if fam in ('PHY','RAD') and 'addr' in kv and kv.get('val')!='UNDEFINED':
        a=int(kv['addr'],16)
        if not (fam=='PHY' and a in EXCL):
            v=int(kv['val'],16); mk=int(kv['mask'],16) if 'mask' in kv else None
            e={'WR':e_wr(v),'OR':e_or(v),'AND':e_and(v),'MOD':e_mod(v,mk)}.get(cls)
            if e: trace.setdefault((fam,a),set()).add(e)
    i+=1
def trace_at(fam,a):
    T=set()
    for d in (0,0x200,0x400):
        if d and not banded(fam,a): break
        if (fam,a+d) in trace: T|=trace[(fam,a+d)]
    return T
deco={}
if os.path.exists(DECO):
    for r in csv.DictReader(open(DECO)): deco[(r['op'].split('.')[0],r['addr'])]=r['class']

def classify_op(pe,T):
    tset=T
    if pe[0]=='WR':
        if pe in tset: return 'OK'
        if any(e[0]=='WR' for e in tset): return 'VAL-DIFF'
        if any(e[0]=='P' for e in tset): return 'OP-MISMATCH'   # WR pieno vs parziale
        return 'PORT-EXTRA'
    _,ps,pc=pe
    if pe in tset: return 'OK'
    # valore variabile: OK se stessa maschera-bit toccata compare nel trace
    if ps is None or pc is None:
        aff=(ps or 0)|(pc or 0)
        if any(e[0]=='P' and ((e[1] or 0)|(e[2] or 0))&aff for e in tset): return 'OK'
        return 'PORT-EXTRA'
    # OR/AND-SWAP: port setta bit m, il trace pulisce ESATTAMENTE ~... con stesso literal
    if pc==0 and ps:  # port e' un SET puro di ps
        for e in tset:
            if e[0]=='P' and e[1]==0 and e[2]==((~ps)&0xffff) and e[2]!=0:
                return 'OP-MISMATCH'      # SET(m) vs AND(m): swap OR/AND sullo stesso letterale
    if ps==0 and pc:  # port e' CLR puro; trace setta esattamente quei bit
        for e in tset:
            if e[0]=='P' and e[2]==0 and e[1]==((~pc)&0xffff) and e[1]!=0:
                return 'OP-MISMATCH'
    aff=ps|pc
    cand=[e for e in tset if e[0]=='P' and ((e[1] or 0)|(e[2] or 0))&aff]
    for e in cand:
        if (ps & (e[2] or 0)) or (pc & (e[1] or 0)): return 'OP-MISMATCH'  # conflitto di direzione
    if cand: return 'VAL-DIFF'
    if any(e[0]=='WR' for e in tset): return 'OK'
    return 'PORT-EXTRA'

rows=[]
for (fam,a),P in port.items():
    T=trace_at(fam,a)
    if not T: continue
    verdicts=[classify_op(pe,T) for pe in P]
    order=['OP-MISMATCH','PORT-EXTRA','VAL-DIFF','OK']
    cls=next(x for x in order if x in verdicts)
    rows.append((fam,f'0x{a:04x}',cls,deco.get((fam,f'0x{a:04x}'),'-')))
rows.sort(key=lambda r:(['OP-MISMATCH','PORT-EXTRA','VAL-DIFF','OK'].index(r[2]),r[0],r[1]))
with open(OUT,'w',newline='') as f:
    w=csv.writer(f); w.writerow(['fam','addr','class','decorrelation_class']); w.writerows(rows)
from collections import Counter
c=Counter(r[2] for r in rows)
tag=f"[src={gitref}]" if gitref else "[src=working]"
print(f"{tag} trace={os.path.basename(TRACE)} radio-innerWR-collassate={coll}")
print(f"  OK={c['OK']} VAL-DIFF={c['VAL-DIFF']} PORT-EXTRA={c['PORT-EXTRA']} OP-MISMATCH={c['OP-MISMATCH']}")
for r in rows:
    if r[2]=='OP-MISMATCH': print(f"    OP-MISMATCH {r[0]} {r[1]} [{r[3]}]")
