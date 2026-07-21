#!/usr/bin/env python3
"""Per-function sequence coverage of a harness flow against a vendor trace.

IMPORTANT: pass the *raw* vendor capture (wl-diag-...txt), NOT a
collapse_trace.py output. compare.py (the regression gate) compares against
the raw trace; collapse strips the data-port/table raw ops, so functions that
touch tables would spuriously miss.

Reads a harness trace generated with AC_FN_MARKERS=1 (----FN:name---- /
----/FN:name---- boundaries) plus the raw vendor trace, and for each
instrumented function reports how many of its ops are found, in order, in the
vendor trace (matched/total), where it localizes, and -- across all located
functions -- the GAPS: vendor ops in the bring-up span no function covers.

Note: table-writing functions (tables_init, tables_zero_cal) emit hundreds of
data-port PHY.WR ops that the vendor interleaves with TBL.WR/TBL.RD in a
different order; they do not align by sequence and should be compared by table
*content*, not by op sequence. Radio/register functions align cleanly.

Usage: coverage_by_function.py <harness_generated_with_markers> <raw_vendor_trace>
"""
OPRE=re.compile(r'\b(PHY|RAD)\.(WR|MOD)\b')
def op_key(line):
    m=OPRE.search(line)
    if not m: return None
    fam,cls=m.group(1),m.group(2)
    d=dict(kv.split("=",1) for kv in line.split() if kv.count("=")==1)
    try:
        a=int(d["addr"],16)
        return ('WR',fam,a,int(d["val"],16)) if cls=="WR" else ('MOD',fam,a,int(d.get("mask","0x0"),16))
    except: return None
def seq_of(line):
    m=re.search(r'#(\d+)',line); return int(m.group(1)) if m else None
gen,ven=sys.argv[1],sys.argv[2]
V=[]
for l in open(ven,errors="ignore"):
    k=op_key(l)
    if k: V.append((seq_of(l),k))
Vk=[k for _,k in V]
idx={}
for i,k in enumerate(Vk): idx.setdefault(k,[]).append(i)
def nextpos(key,after,gap):
    lst=idx.get(key)
    if not lst: return None
    j=bisect.bisect_right(lst,after)
    if j<len(lst) and lst[j]<=after+gap: return lst[j]
    return None
stack=[]; ops={}; order=[]
for l in open(gen,errors="ignore"):
    m=re.match(r'----FN:(\w+)----',l)
    if m:
        stack.append(m.group(1)); ops.setdefault(m.group(1),[])
        if m.group(1) not in order: order.append(m.group(1))
        continue
    m=re.match(r'----/FN:(\w+)----',l)
    if m:
        if stack and stack[-1]==m.group(1): stack.pop()
        continue
    if not stack: continue
    k=op_key(l)
    if k: ops[stack[-1]].append(k)
def cover(oph):
    if not oph: return (0,0,None,None,None)
    best=0; span=None; bi=None
    anchors=idx.get(oph[0],[])[:60]
    for st in anchors:
        pos=st; matched=1; last=st
        for e in oph[1:]:
            nx=nextpos(e,pos,400)
            if nx is not None: matched+=1; pos=nx; last=nx
        if matched>best: best=matched; span=(V[st][0],V[last][0]); bi=(st,last)
    return (best,len(oph),span[0] if span else None,span[1] if span else None,bi)
loc=[]
for fn in order:
    m,t,s0,s1,bi=cover(ops[fn]); loc.append((fn,m,t,s0,s1,bi))
    pct=100*m/t if t else 0
    sp=f"#{s0}..#{s1}" if s0 is not None else "not found"
    print(f"  {fn:36s} {m:4d}/{t:<4d} {pct:5.1f}%  {sp}")
placed=[bi for (_,_,_,_,_,bi) in loc if bi]
if placed:
    lo=min(a for a,_ in placed); hi=max(b for _,b in placed)
    cov=[False]*len(V)
    for a,b in placed:
        for i in range(a,b+1): cov[i]=True
    print("  --- GAP (op vendor nella span non coperte) ---")
    i=lo; tg=0
    while i<=hi:
        if not cov[i]:
            j=i
            while j<=hi and not cov[j]: j+=1
            run=V[i:j]; tg+=len(run)
            fams={}
            for _,k in run: fams[f"{k[1]}.{k[0]}"]=fams.get(f"{k[1]}.{k[0]}",0)+1
            top=", ".join(f"{n}:{c}" for n,c in sorted(fams.items(),key=lambda x:-x[1])[:3])
            if len(run)>=5: print(f"      #{V[i][0]}..#{V[j-1][0]}  {len(run):4d} op  [{top}]")
            i=j
        else: i+=1
    print(f"      => {tg} op nei gap su ~{hi-lo+1} ({100*tg/(hi-lo+1):.1f}% scoperto)")
