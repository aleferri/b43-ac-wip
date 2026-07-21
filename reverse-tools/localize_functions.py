#!/usr/bin/env python3
"""
Locate where each driver function runs in the trace by matching its ORDERED
sequence of literal ops -- writes (addr,val) AND masksets/mask/set with a
literal affected-mask (addr,mask) -- as a fingerprint against the trace.
Ordered multi-op fingerprints are robust to the register reuse that defeats
single-register anchors. Also produces a full function segmentation of the
first bring-up window.  Run: python3 localize_functions.py [repo_root]
"""
import re, os, sys, glob
HERE=os.path.dirname(os.path.abspath(__file__))
ROOT=sys.argv[1] if len(sys.argv)>1 else os.path.dirname(HERE)
SRCDIR=os.path.join(ROOT,"src")
TRACE=os.path.join(HERE,"d6220-trace2-collapsed.txt")
if not os.path.exists(TRACE): TRACE=os.path.join(ROOT,"reverse-output/d6220-trace2-collapsed.txt")

sym={}
for h in glob.glob(SRCDIR+"/*.h")+glob.glob(SRCDIR+"/*.c"):
    for l in open(h,errors="ignore"):
        m=re.match(r"\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\b",l)
        if m: sym.setdefault(m.group(1),int(m.group(2),16))
def val(t):
    t=t.strip()
    m=re.match(r"^\(u16\)\s*~\s*(.+)$",t)          # (u16)~EXPR  -> ~resolve
    if m:
        inner=val(m.group(1))
        return None if inner is None else (~inner)&0xffff
    if t.startswith("~"):
        inner=val(t[1:]); return None if inner is None else (~inner)&0xffff
    if re.fullmatch(r"0x[0-9a-fA-F]+",t): return int(t,16)
    if re.fullmatch(r"\d+",t): return int(t)
    return sym.get(t)

CALL=re.compile(r"\bb43_(phy|radio)_(write|set|mask|maskset)\s*\(")
SIG=re.compile(r"^[A-Za-z].*\b(b43_[a-z0-9_]+)\s*\([^;]*$")
def args(s):
    o=[];d=0;c=""
    for ch in s:
        if ch=='(':d+=1;c+=ch
        elif ch==')':
            if d==0:o.append(c);return o
            d-=1;c+=ch
        elif ch==','and d==0:o.append(c);c=""
        else:c+=ch
    o.append(c);return o

# per-function ordered fingerprint elements: ('WR',fam,addr,val) or ('MOD',fam,addr,affmask)
func_fp={}
for cf in sorted(glob.glob(SRCDIR+"/*.c")):
    depth=0; cur=None
    for l in open(cf,errors="ignore").read().splitlines():
        if depth==0:
            m=SIG.match(l)
            if m: cur=m.group(1)
        for m in CALL.finditer(l):
            fam="PHY" if m.group(1)=="phy" else "RAD"; kind=m.group(2)
            a=[x.strip() for x in args(l[m.end():])]
            if len(a)<2 or not cur: continue
            addr=val(a[1])
            if addr is None: continue
            if kind=="write" and len(a)>=3:
                v=val(a[2])
                if v is not None: func_fp.setdefault(cur,[]).append(('WR',fam,addr,v))
            elif kind=="set" and len(a)>=3:
                mk=val(a[2])
                if mk is not None: func_fp.setdefault(cur,[]).append(('MOD',fam,addr,mk&0xffff))
            elif kind=="mask" and len(a)>=3:
                mk=val(a[2])                       # affected = ~arg
                if mk is not None: func_fp.setdefault(cur,[]).append(('MOD',fam,addr,(~mk)&0xffff))
            elif kind=="maskset" and len(a)>=4:
                keep=val(a[2])                     # affected = ~keep
                if keep is not None: func_fp.setdefault(cur,[]).append(('MOD',fam,addr,(~keep)&0xffff))
        depth+=l.count("{")-l.count("}")
        if depth<0: depth=0

# trace ops (WR + MOD) in order
tr=[]
for l in open(TRACE,errors="ignore"):
    if l.startswith("#"): continue
    p=l.split()
    if len(p)<5: continue
    op=p[3]
    if op not in ("PHY.WR","RAD.WR","PHY.MOD","RAD.MOD"): continue
    fam,cls=op.split(".")
    d=dict(kv.split("=",1) for kv in p[4:] if "=" in kv)
    try:
        seq=int(p[1].lstrip("#")); a=int(d["addr"],16)
        if cls=="WR": tr.append((seq,('WR',fam,a,int(d["val"],16))))
        else:         tr.append((seq,('MOD',fam,a,int(d.get("mask","0x0"),16))))
    except: pass
from collections import defaultdict
idx=defaultdict(list)
for i,(s,key) in enumerate(tr): idx[key].append(i)

def find_fp(fp,max_gap=400):
    if len(fp)<3: return []
    out=[]
    for i0 in idx.get(fp[0],[]):
        pos=i0; ok=True
        for e in fp[1:]:
            nx=[j for j in idx.get(e,[]) if pos<j<=pos+max_gap]
            if not nx: ok=False;break
            pos=nx[0]
        if ok: out.append((tr[i0][0],tr[pos][0]))
    return out
def dedup(ms):
    ms=sorted(ms);o=[]
    for m in ms:
        if o and m[0]-o[-1][0]<50: continue
        o.append(m)
    return o

res=[]
for fn,fp in func_fp.items():
    for (s0,s1) in dedup(find_fp(fp[:10])):
        res.append((s0,s1,fn,min(len(fp),10),len(fp)))
res.sort()
print("REAL CALL ORDER (fingerprint = ordered literal WR + mask-MOD)")
print("start   end     function                               fp_used/tot")
for s0,s1,fn,u,t in res:
    print("#%-6d #%-6d %-38s %d/%d"%(s0,s1,fn,u,t))

# full segmentation of first bring-up window: [start, next_start)
print("\nSEGMENTATION of first bring-up (#51200-#52050):")
w=[r for r in res if 51200<=r[0]<=52050]
for i,(s0,s1,fn,u,t) in enumerate(w):
    nxt=w[i+1][0] if i+1<len(w) else 52050
    # count trace ops (any) in [s0,nxt)
    print("  #%-6d .. #%-6d  %-36s"%(s0,nxt,fn))
print("\nUn-fingerprintable (<3 literal ops):")
print("  "+", ".join(sorted(fn for fn,fp in func_fp.items() if len(fp)<3)))
