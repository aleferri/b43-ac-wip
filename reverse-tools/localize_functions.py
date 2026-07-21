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
ROOT=os.path.dirname(HERE)
SRCDIR=os.path.join(ROOT,"src")
# trace to analyse: argv[1] if given (must be collapse_trace.py output),
# else the default d6220 collapsed. Fingerprints always come from ROOT/src.
# Two modes:
#   new:  localize_functions.py <generated_with_FN_markers> <target_trace>
#         per-function fingerprints come from the harness output (exact
#         boundaries, real ops -- handles loops and computed addresses).
#   old:  localize_functions.py [<target_trace>]
#         fingerprints are guessed from ROOT/src (fallback, less accurate).
GEN=None
if len(sys.argv)>2:
    GEN,TRACE=sys.argv[1],sys.argv[2]
elif len(sys.argv)>1:
    TRACE=sys.argv[1]
else:
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

# op parsing shared by the target trace and the FN-marker segmentation.
# Robust to both formats: the vendor collapsed trace (has a #seq token) and
# the harness output (starts with "cpu1", no seq). Only WR/MOD carry a literal
# value/mask usable as a fingerprint element; RD (val=UNDEFINED) is skipped.
OPRE=re.compile(r'\b(PHY|RAD)\.(WR|MOD)\b')
def op_key(line):
    m=OPRE.search(line)
    if not m: return None
    fam,cls=m.group(1),m.group(2)
    d=dict(kv.split("=",1) for kv in line.split() if kv.count("=")==1)
    try:
        a=int(d["addr"],16)
        key=('WR',fam,a,int(d["val"],16)) if cls=="WR" \
            else ('MOD',fam,a,int(d.get("mask","0x0"),16))
    except Exception:
        return None
    sm=re.search(r'#(\d+)',line)
    return (int(sm.group(1)) if sm else None, key)

def parse_ops(path):
    out=[]
    for l in open(path,errors="ignore"):
        k=op_key(l)
        if k: out.append(k)
    return out

# fingerprints straight from the harness FN markers: exact per-function op
# sequences, no source guessing. enter/leave nest, so each op is attributed to
# the innermost active function (a function's own ops, not its callees').
def fp_from_markers(path):
    fp={}; stack=[]
    for l in open(path,errors="ignore"):
        m=re.match(r'----FN:(\w+)----',l)
        if m:
            stack.append(m.group(1)); fp.setdefault(m.group(1),[]); continue
        m=re.match(r'----/FN:(\w+)----',l)
        if m:
            if stack and stack[-1]==m.group(1): stack.pop()
            continue
        if not stack: continue
        k=op_key(l)
        if k: fp[stack[-1]].append(k[1])
    return fp

tr=parse_ops(TRACE)
if GEN is not None:
    func_fp=fp_from_markers(GEN)
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
print("\nSEGMENTATION (each match to the next):")
for i,(s0,s1,fn,u,t) in enumerate(res):
    nxt=res[i+1][0] if i+1<len(res) else s1
    print("  #%-6d .. #%-6d  %-36s"%(s0,nxt,fn))
print("\nUn-fingerprintable (<3 literal ops):")
print("  "+", ".join(sorted(fn for fn,fp in func_fp.items() if len(fp)<3)))
