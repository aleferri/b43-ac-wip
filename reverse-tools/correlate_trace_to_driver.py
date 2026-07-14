#!/usr/bin/env python3
"""
Reproducible correlation of trace ops -> driver register-access call sites.

Input : the CLEANED+COLLAPSED 2nd trace (TBL markers kept, table body removed)
        the driver scratch sources (./*.{c,h})
Output: correlation.csv  (one row per trace op, except TBL.*)
        prints a coverage summary.

Correlation key = (family PHY/RAD, op-class RD/WR/MOD, register address).
- WR  : also value-exact when the driver value is a literal  -> match=EXACT
- any : single driver site for (fam,addr,opclass)             -> match=UNIQUE
- any : several driver sites                                  -> match=MULTI (all listed)
- any : no driver site touches that (fam,addr)                -> match=NONE (ucode / not-ported)
No ordering/behavioural inference is done here: every row is justified by an
address (and, where possible, a value) that literally appears in the source.
"""
import re, sys, csv, glob, os
from collections import defaultdict, Counter

# repo root: explicit arg, else one level above this script (reverse-tools/ -> repo)
ROOT = sys.argv[1] if len(sys.argv)>1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACE = os.path.join(ROOT,"router-data/d6220/wl-diag-wl1-down-to-bss-up.txt")
# prefer the pre-built collapsed trace in reverse-output/
COLL = os.path.join(ROOT,"reverse-output/d6220-trace2-collapsed.txt")
SRCDIR = os.path.join(ROOT,"src")
OUTCSV = os.path.join(ROOT,"reverse-output/correlation.csv")

# ---- 1. symbol table (#define NAME 0xHEX) -----------------------------------
sym={}
for h in glob.glob(os.path.join(SRCDIR,"*.h"))+glob.glob(os.path.join(SRCDIR,"*.c")):
    for l in open(h,errors="ignore"):
        m=re.match(r"\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\b",l)
        if m: sym.setdefault(m.group(1),int(m.group(2),16))
def val(tok):
    tok=tok.strip()
    if re.fullmatch(r"0x[0-9a-fA-F]+",tok): return int(tok,16)
    if re.fullmatch(r"\d+",tok): return int(tok)
    if tok in sym: return sym[tok]
    return None                       # expression / variable -> unresolved

# register sets of driver register tables emitted via a loop, so an
# array-indexed argument (NAME[i].reg or NAME[i][0]) resolves to the whole set:
#  - struct r2069_chan_write NAME[] = {{reg, idx}, ...}   (NAME[i].reg)
#  - u16 NAME[][2] = {{reg, val}, ...}                    (NAME[i][0], e.g. prefregs)
TABLE_REGS=set()                      # union, used for the anonymous writes[i].reg form
ARRAY_REGS={}                         # name -> reg set, for NAME[i]... resolution
for c in glob.glob(os.path.join(SRCDIR,"*.c")):
    txt=open(c,errors="ignore").read()
    for name,blk in re.findall(r"struct\s+r2069_chan_write\s+(\w+)\[\]\s*=\s*\{(.*?)\};", txt, re.S):
        rs={int(r,16) for r in re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,", blk)}
        ARRAY_REGS.setdefault(name,set()).update(rs); TABLE_REGS|=rs
    for name,blk in re.findall(r"\b(\w+)\s*\[\]\s*\[\s*2\s*\]\s*=\s*\{(.*?)\};", txt, re.S):
        rs={int(r,16) for r in re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,", blk)}
        ARRAY_REGS.setdefault(name,set()).update(rs); TABLE_REGS|=rs
    # flat u16 arrays: "u16 NAME[N] = { 0xHEX, 0xHEX, ... }"
    for name,blk in re.findall(r"\bu16\s+(\w+)\s*\[\d*\]\s*=\s*\{([^}]+)\}", txt):
        rs={int(r,16) for r in re.findall(r"0x[0-9a-fA-F]+", blk)}
        if rs:
            ARRAY_REGS.setdefault(name,set()).update(rs)

def addrs(tok):
    """Resolve a register argument to a list of concrete addresses.
    Handles literals/symbols, the per-core stride idiom (base + co/stride), and
    table-driven writes (NAME[i].reg / NAME[i][0] -> every reg in that table)."""
    tok=tok.strip()
    v=val(tok)
    if v is not None: return [v]
    # per-core stride: BASE + {co|stride|<var>*0x[0]200|0x[0]200*<var>|<var><<9}.
    # The per-core stride constant (0x200) is what pins the idiom, not the loop
    # variable name (c/core/...), so match any \w+ against that fixed stride to
    # avoid false negatives like `0x0690 + c * 0x0200`. A different stride (e.g.
    # *0x40) will not match, so table strides are not misread as per-core.
    m=re.match(r"^(0x[0-9a-fA-F]+)\s*\+\s*(?:co\b|stride\b|\w+\s*\*\s*0x0?200\b|0x0?200\s*\*\s*\w+|\w+\s*<<\s*9)",tok)
    if m:
        b=int(m.group(1),16); return [b, b+0x200, b+0x400]
    # rccal per-core apply idiom: (REG | stride) & 0xNNNN , stride = core*0x200
    m=re.match(r"^\(\s*([\w]+)\s*\|\s*stride\s*\)\s*&\s*(0x[0-9a-fA-F]+)",tok)
    if m:
        b=val(m.group(1)); am=int(m.group(2),16)
        if b is not None:
            return [(b|s)&am for s in (0,0x200,0x400)]
    # e->member[i] where member is a struct field: resolve from write target reg
    m=re.match(r"^\w+->\w+\s*\[", tok)
    if m:
        return []  # unresolvable without type info, but don't block further patterns
    m=re.search(r"(\w+)\s*\[\w+\]\s*(?:\.reg|\[\s*0\s*\])", tok)   # NAME[i].reg or NAME[i][0]
    if m:
        return sorted(ARRAY_REGS.get(m.group(1), TABLE_REGS))
    # bare NAME[i] (flat u16 array used as register address)
    m=re.match(r"^(\w+)\s*\[", tok)
    if m and m.group(1) in ARRAY_REGS:
        return sorted(ARRAY_REGS[m.group(1)])
    # cast arithmetic: (u16)(0xBASE + var) — small loop over contiguous regs
    m=re.match(r"^\(u16\)\s*\(\s*(0x[0-9a-fA-F]+)\s*\+\s*\w+\s*\)", tok)
    if m:
        base=int(m.group(1),16); return list(range(base, base+32))
    # SYMBOL + var — sequential iteration over contiguous regs (e.g. BW1A + i)
    m=re.match(r"^(\w+)\s*\+\s*\w+$", tok)
    if m:
        b=val(m.group(1))
        if b is not None:
            return list(range(b, b+32))
    return []

# ---- 2. driver register-access call sites -----------------------------------
CALL=re.compile(r"\b(?:b43_(phy|radio)_(write|read|read_log|set|mask|maskset)|(r2069_mod))\s*\(")
def split_args(s):                    # s = text right after '(' ; return top-level args
    args=[];depth=0;cur=""
    for ch in s:
        if ch=='(' : depth+=1;cur+=ch
        elif ch==')':
            if depth==0: args.append(cur);return args
            depth-=1;cur+=ch
        elif ch==',' and depth==0: args.append(cur);cur=""
        else: cur+=ch
    args.append(cur);return args

sites=[]   # dict: file,line,func,fam,opclass,addr,val,mask,raw
funcre=re.compile(r"^[A-Za-z].*\b(b43_[a-z0-9_]+)\s*\(")
for c in sorted(glob.glob(os.path.join(SRCDIR,"*.c"))):
    func="?"; base=os.path.basename(c)
    lines=open(c,errors="ignore").read().splitlines()
    for i,l in enumerate(lines,1):
        fm=funcre.match(l)
        if fm and l.rstrip().endswith(")") is False and "(" in l and ";" not in l:
            func=fm.group(1)
        for m in CALL.finditer(l):
            wrapper = m.group(3)                 # 'r2069_mod' or None
            fam="RAD" if wrapper else ("PHY" if m.group(1)=="phy" else "RAD")
            prim="maskset" if wrapper else m.group(2)
            after=l[m.end():]
            a=split_args(after)
            if not a: continue
            a=[x.strip() for x in a]
            reg = a[1] if len(a)>1 else a[0]     # (dev, reg, ...)
            v=msk=None; opclass=None
            if prim in ("write",):
                opclass="WR"; v=val(a[2]) if len(a)>2 else None
            elif prim in ("read","read_log"):
                opclass="RD"
            elif prim=="set":
                opclass="MOD"; msk=val(a[2]) if len(a)>2 else None; v=msk
            elif prim=="mask":
                opclass="MOD"; msk=val(a[2]) if len(a)>2 else None
            elif prim=="maskset":
                # b43_*_maskset(dev,reg,~keep,val) OR r2069_mod(dev,reg,mask,val)
                opclass="MOD"; msk=val(a[2]) if len(a)>2 else None; v=val(a[3]) if len(a)>3 else None
            for addr in addrs(reg):
                sites.append(dict(file=base,line=i,func=func,fam=fam,opclass=opclass,
                                  addr=addr,val=v,mask=msk,raw=l.strip()))

# index
by_fa=defaultdict(list)               # (fam,opclass,addr) -> sites
for s in sites:
    if s["addr"] is not None:
        by_fa[(s["fam"],s["opclass"],s["addr"])].append(s)

# ---- 3. trace ops (skip TBL, skip comments) ---------------------------------
src = COLL if os.path.exists(COLL) else TRACE
def parse(l):
    p=l.split()
    if len(p)<4 or l.startswith("#"): return None
    d={"seq":p[1].lstrip("#"),"cpu":p[2],"op":p[3]}
    for kv in p[4:]:
        if "=" in kv:
            k,x=kv.split("=",1); d[k]=x
    return d
OPCLASS={"PHY.WR":("PHY","WR"),"PHY.RD":("PHY","RD"),"PHY.MOD":("PHY","MOD"),
         # PHY.AND/PHY.OR (wl_diag op 19/20): lato driver and/or/maskset
         # collassano tutti su opclass MOD, quindi correlano come PHY.MOD per
         # indirizzo. Tightening possibile (AND->b43_phy_mask, OR->b43_phy_set
         # per valore) ma non necessario per non perdere la copertura.
         "PHY.AND":("PHY","MOD"),"PHY.OR":("PHY","MOD"),
         "RAD.WR":("RAD","WR"),"RAD.RD":("RAD","RD"),"RAD.MOD":("RAD","MOD")}
def h(x):
    try:return int(x,16)
    except:return None

rows=[]; stat=Counter(); no_addr=Counter()
for l in open(src,errors="ignore"):
    d=parse(l)
    if not d: continue
    if d["op"].startswith("TBL"): continue          # excluded per request
    if d["op"] not in OPCLASS:                       # PMU/GPIO etc.
        stat["OTHER(%s)"%d["op"]]+=1
        rows.append([d["seq"],d["op"],d.get("addr",""),d.get("val",""),d.get("mask",""),"NONE","","",""]); 
        no_addr[d["op"]]+=1; continue
    fam,opc=OPCLASS[d["op"]]; addr=h(d.get("addr",""))
    tv=h(d.get("val","")); tm=h(d.get("mask",""))
    cand=by_fa.get((fam,opc,addr),[])
    level="NONE"; chosen=[]
    if cand:
        if opc=="WR" and tv is not None:
            exact=[s for s in cand if s["val"]==tv]
            if exact: level="EXACT"; chosen=exact
            elif len(cand)==1: level="UNIQUE"; chosen=cand
            else: level="MULTI"; chosen=cand
        elif opc=="MOD":
            exact=[s for s in cand if (s["mask"]==tm and (s["val"] in (tv,None)))]
            if exact and len(exact)==1: level="EXACT"; chosen=exact
            elif len(cand)==1: level="UNIQUE"; chosen=cand
            else: level="MULTI"; chosen=cand
        else:
            level="UNIQUE" if len(cand)==1 else "MULTI"; chosen=cand
    stat[level]+=1
    # FASE 2: for an address-covered op, cross-check value/mask against the
    # chosen driver site(s). EXACT is VAL-OK by construction. A UNIQUE/MULTI
    # with a *resolvable* driver literal that conflicts with the trace is a
    # probable transcription bug (the P1_B class); None-valued args (expr/var)
    # stay VAL-UNRESOLVED. RD and NONE have no value to check.
    def _conflict(s):
        return ((s["mask"] is not None and tm is not None and s["mask"]!=tm) or
                (s["val"]  is not None and tv is not None and s["val"] !=tv))
    def _resolvable(s):
        return s["mask"] is not None or s["val"] is not None
    if level=="EXACT":
        vcheck="VAL-OK"
    elif level in ("UNIQUE","MULTI") and opc in ("WR","MOD"):
        if opc=="WR":
            ok = any(s["val"]==tv for s in chosen) if tv is not None else False
        else:  # MOD
            ok = any(s["mask"]==tm and (s["val"] in (tv,None)) for s in chosen)
        if ok:                              vcheck="VAL-OK"
        elif any(_conflict(s) for s in chosen) and all(_resolvable(s) for s in chosen):
            vcheck="VAL-MISMATCH"
        else:                               vcheck="VAL-UNRESOLVED"
    else:
        vcheck=""
    sitestr=";".join("%s:%d %s"%(s["file"],s["line"],s["func"]) for s in chosen[:4])
    rows.append([d["seq"],d["op"],d.get("addr",""),d.get("val",""),d.get("mask",""),
                 level,str(len(cand)),sitestr,vcheck])

# second pass: reclassify the read-modify-write implementation ops that
# b43_radio/phy_maskset produces. The trace logs the logical MOD *and* the
# underlying RD+WR, so a matched MOD at [k] is followed by up to two NONE
# ops on the same (fam, addr): the read-ahead [k+1] and write-back [k+2].
# Skip past already-matched RDs (explicit readbacks the correlator explains).
MATCHED={"EXACT","UNIQUE","MULTI"}
MODS={"PHY.MOD","RAD.MOD","PHY.AND","PHY.OR"}
for k in range(len(rows)-1):
    r=rows[k]
    if r[1] not in MODS or r[5] not in MATCHED: continue
    fam=r[1].split("src")[0]; addr=r[2]; site=r[7]
    for j in range(k+1, min(k+4, len(rows))):
        rj=rows[j]
        rj_fam=rj[1].split("src")[0]
        if rj_fam!=fam or rj[2]!=addr: break
        if rj[5]!="NONE":
            continue
        if rj[1] in (fam+".RD",):
            rj[5]="RMW-RD"; rj[7]=site
        elif rj[1] in (fam+".WR",):
            rj[5]="RMW-WB"; rj[7]=site

with open(OUTCSV,"w",newline="") as f:
    w=csv.writer(f); w.writerow(["trace_seq","op","addr","val","mask","match","n_driver_sites","driver_site(s)","value_check"])
    w.writerows(rows)

# ---- 4. summary -------------------------------------------------------------
stat=Counter(r[5] for r in rows)
tot=sum(stat.values())
print("source trace :",os.path.basename(src))
print("driver sites : %d register-access calls, %d distinct (fam,op,addr) keys"%(len(sites),len(by_fa)))
print("trace ops correlated (TBL excluded): %d"%tot)
for k in ("EXACT","UNIQUE","MULTI","RMW-WB","RMW-RD","NONE"):
    if stat[k]: print("   %-6s %6d  (%4.1f%%)"%(k,stat[k],100*stat[k]/tot))
matched=stat["EXACT"]+stat["UNIQUE"]+stat["MULTI"]+stat["RMW-WB"]+stat["RMW-RD"]
print("=> %.1f%% of trace ops explained by a driver op (incl. RMW write-backs)"%(100*matched/tot))

# ---- FASE 2: value/mask cross-check on address-covered ops ------------------
vc=Counter(r[8] for r in rows if r[8])
print("\nvalue-check on address-covered ops:")
for k in ("VAL-OK","VAL-UNRESOLVED","VAL-MISMATCH"):
    if vc[k]: print("   %-14s %5d"%(k,vc[k]))

# The raw VAL-MISMATCH set is noisy: a register written by several routines (or
# by a periodic recalc) with different legit values, or a MOD bit-op matched to
# an unrelated set on the same address, all show up. The P1_B bug had a tighter
# signature: a single absolute-write site with a literal, and the register takes
# ONE constant value across the whole trace that differs from that literal.
wr_vals=defaultdict(set)
for r in rows:
    if r[1].endswith(".WR"):
        a=h(r[2]); v=h(r[3])
        if a is not None and v is not None: wr_vals[(r[1].split("src")[0],a)].add(v)
hi=[]; seen=set()
for r in rows:
    if r[8]=="VAL-MISMATCH" and r[5]=="UNIQUE" and r[1].endswith(".WR"):
        key=(r[1],r[2])
        if len(wr_vals.get((r[1].split("src")[0],h(r[2])),()))==1 and key not in seen:
            seen.add(key); hi.append(r)
if hi:
    print("\nHIGH-CONFIDENCE transcription-bug candidates (P1_B class: single write")
    print("site, register carries ONE constant in the trace != the driver literal):")
    for r in hi:
        print("   %s addr=%s  trace=%s  driver=%s"%(r[1],r[2],r[3],r[7]))
print("\n(%d raw VAL-MISMATCH total: the rest are multi-value/periodic regs or MOD"
      " bit-ops -- review manually, lower confidence)"%vc["VAL-MISMATCH"])

# addresses with no driver counterpart
none_addr=Counter()
for r in rows:
    if r[5]=="NONE" and r[2]:
        none_addr[(r[1].split("src")[0],r[2])]+=1
print("\nTop trace addresses with NO driver counterpart (fam,addr,count):")
for (fam,addr),n in none_addr.most_common(15):
    print("   %-3s %-8s %5d"%(fam,addr,n))

# driver register sites never seen in the trace
seen=set()
for r in rows:
    a=h(r[2]) if r[2] else None
    if a is not None and r[5]!="NONE":
        seen.add((r[1].split("src")[0],a))
never=Counter()
for s in sites:
    if s["addr"] is not None and (s["fam"],s["addr"]) not in seen:
        never[(s["fam"],"0x%04x"%s["addr"],s["func"])]+=1
print("\nDriver register accesses NOT exercised by this trace: %d call sites"%sum(never.values()))
for (fam,addr,fn),n in never.most_common(12):
    print("   %-3s %-8s %s"%(fam,addr,fn))

# ---- FASE 2 (address class): the real P1_B was an ADDRESS typo (0x04a vs
# 0x043), not a value one. Signature: a driver WR-site whose address the trace
# never touches, while the trace writes the SAME literal value to a *nearby*
# address that itself has no driver counterpart. Same value + small delta +
# both otherwise unexplained = probable wrong address in the driver.
trace_none_wr=defaultdict(set)                    # (fam,addr) -> {values}
for r in rows:
    if r[5]=="NONE" and r[1].endswith(".WR"):
        a=h(r[2]); v=h(r[3])
        if a is not None and v is not None: trace_none_wr[(r[1].split("src")[0],a)].add(v)
nearmiss=[]
for s in sites:
    if s["opclass"]!="WR" or s["addr"] is None or s["val"] is None: continue
    if (s["fam"],s["addr"]) in seen: continue     # driver addr IS in trace -> fine
    for (fam,B),vals in trace_none_wr.items():
        if fam==s["fam"] and B!=s["addr"] and abs(B-s["addr"])<=15 and s["val"] in vals:
            nearmiss.append((s["fam"],s["addr"],B,s["val"],s["func"],s["file"],s["line"]))
seen_nm=set(); nearmiss=[x for x in nearmiss if not (x[:4] in seen_nm or seen_nm.add(x[:4]))]
print("\nADDRESS-typo candidates (driver writes an addr the trace never touches,"
      " same value lands on a nearby unexplained addr -- the real P1_B signature):")
if nearmiss:
    for fam,A,B,V,fn,fl,ln in nearmiss:
        print("   %s driver 0x%04x=0x%04x (%s:%d) vs trace 0x%04x=0x%04x  (Δ=%+d)"
              %(fam,A,V,fl,ln,B,V,B-A))
else:
    print("   none")
