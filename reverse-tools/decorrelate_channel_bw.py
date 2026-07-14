#!/usr/bin/env python3
"""Decorrela le write deterministiche fra 4 trace ch36/ch44/ch36-40/vht:
baseline / solo-canale / solo-ampiezza / centro-freq(canale+ampiezza) / DYN.
La variabile indipendente reale e' la freq centrale (canale primario+ampiezza),
per questo 'solo-canale' esce vuoto.
Uso: python3 decorrelate_channel_bw.py [reverse_output_dir]"""
import re,glob,os,sys,csv
from collections import defaultdict
D=sys.argv[1] if len(sys.argv)>1 else os.path.join(os.path.dirname(os.path.abspath(__file__)),"../reverse-output")
F={'A':'wl-diag-d6220-4352-wl1-attach-to-bss-ch36.txt','B':'wl-diag-d6220-4352-wl1-attach-to-bss-ch44.txt',
   'C':'wl-diag-d6220-4352-wl1-attach-to-bss-ch36-40.txt','D':'wl-diag-d6220-4352-wl1-attach-to-bss-vht.txt'}
WR={'PHY.WR','PHY.MOD','PHY.AND','PHY.OR','TBL.WR','RAD.WR','RAD.MOD','MAC.MHF','PMU.RC','PMU.PLL','GPIO.OUT'}
lr=re.compile(r'#\d+\s+cpu\d+\s+(\S+)\s+(.*)')
def parse(fn):
    d=defaultdict(list)
    for ln in open(os.path.join(D,fn)):
        m=lr.search(ln)
        if not m: continue
        op=m.group(1); kv=dict(re.findall(r'(\w+)=(\S+)',m.group(2)))
        if op not in WR or kv.get('val')=='UNDEFINED': continue
        d[(op,kv.get('addr','-'))].append((kv.get('val'),kv.get('mask','-')))
    return d
T={k:parse(v) for k,v in F.items()}
keys=set().union(*[set(d) for d in T.values()])
def one(d,k):
    if k not in d: return None
    s=set(d[k]); return next(iter(s)) if len(s)==1 else 'DYN'
out=[]
for k in sorted(keys):
    a,b,c,dd=(one(T[t],k) for t in 'ABCD'); v=[a,b,c,dd]
    if 'DYN' in v: cls='DYN-runtime'
    elif any(x is None for x in v): cls='partial-presence'
    elif a==b and a==c==dd: cls='baseline'
    elif a!=b and a==c==dd: cls='channel-only'
    elif a==b and not(a==c==dd): cls='width-only'
    else: cls='center-freq(chan+width)'
    g=lambda x:x[0] if isinstance(x,tuple) else x
    out.append((cls,k[0],k[1],g(a),g(b),g(c),g(dd)))
with open(os.path.join(D,'decorrelation-ch-vs-bw.csv'),'w',newline='') as f:
    w=csv.writer(f); w.writerow(['class','op','addr','v_36_20','v_44_20','v_36_40','v_36_80']); w.writerows(out)
from collections import Counter
print("decorrelazione:",dict(Counter(r[0] for r in out)))
