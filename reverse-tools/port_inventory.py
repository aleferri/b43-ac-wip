#!/usr/bin/env python3
"""
Reproducible porting inventory for the down->bss-up path.
Reads reverse-output/correlation.csv (per-op match) + the collapsed trace,
segments the timeline into COVERED (existing driver fn) and MISSING units,
merges per-core repeats (addr normalised mod 0x200), and classifies each
missing unit deterministic vs data-dependent by its read/write composition.
Run: python3 port_inventory.py [repo_root]   (default: parent-of-parent of this file)
"""
import csv, os, sys
from collections import Counter, defaultdict, OrderedDict
HERE=os.path.dirname(os.path.abspath(__file__))
ROOT=sys.argv[1] if len(sys.argv)>1 else os.path.dirname(HERE)
CSV=os.path.join(ROOT,"reverse-output/correlation.csv")
if not os.path.exists(CSV): CSV=os.path.join(HERE,"correlation.csv")
rows=list(csv.DictReader(open(CSV)))
def fam(op): return op.split(".")[0]
def cls(op): return op.split(".")[1] if "." in op else op
def norm(addr):
    try: return int(addr,16)%0x200
    except: return None
def fnof(site): return site.split(" ",1)[1] if " " in site else (site or "?")

# 1. run-length timeline: covered fn vs missing signature
def sig(run):
    regs=Counter((fam(r["op"]),norm(r["addr"])) for r in run if r["addr"])
    top=tuple(sorted(k for k,_ in regs.most_common(4)))
    dom=Counter(cls(r["op"]) for r in run).most_common(1)[0][0]
    return (dom,top)

units=[]                     # ordered timeline entries
i=0
while i<len(rows):
    r=rows[i]
    miss=(r["match"]=="NONE")
    j=i
    while j<len(rows) and (rows[j]["match"]=="NONE")==miss: j+=1
    run=rows[i:j]
    if miss:
        units.append(("MISS",sig(run),run))
    else:
        f=Counter(fnof(x["driver_site(s)"]) for x in run if x["driver_site(s)"]).most_common(1)
        units.append(("OK",f[0][0] if f else "?",run))
    i=j

# 2. distinct MISSING units (merge by signature)
mu=OrderedDict()
for kind,key,run in units:
    if kind!="MISS": continue
    m=mu.setdefault(key,{"occ":0,"ops":0,"rd":0,"wr":0,"mod":0,"regs":Counter(),"cores":set(),"seqs":[]})
    m["occ"]+=1; m["ops"]+=len(run); m["seqs"].append((run[0]["trace_seq"],run[-1]["trace_seq"]))
    for r in run:
        c=cls(r["op"]); m[c.lower()] = m.get(c.lower(),0)+1 if c.lower() in("rd","wr","mod") else m.get(c.lower(),0)
        if r["addr"]:
            m["regs"][(fam(r["op"]),r["addr"])]+=1
            try: m["cores"].add(int(r["addr"],16)//0x200)
            except: pass

# 3. print inventory
tot_missing=sum(len(run) for k,_,run in units if k=="MISS")
print("== TIMELINE (down->bss-up, run-length; OK=existing fn, ## = missing unit) ==")
prev=None; line=[]
for kind,key,run in units:
    tag = key if kind=="OK" else "## MISSING"
    if tag!=prev:
        print("  %-6s %s"%(len(run),tag if kind=="OK" else "## %s regs~%s"%(key[0], ",".join("%s%03x"%(f,a) for f,a in key[1] if a is not None))))
        prev=tag
print("\n== DISTINCT MISSING UNITS: %d  (total missing ops %d) =="%(len(mu),tot_missing))
def feas(m):
    rd=m.get("rd",0); wm=m.get("wr",0)+m.get("mod",0)
    # deterministic if standalone reads are negligible vs writes/mods
    if rd<=max(2,0.10*(rd+wm)): return "DETERMINISTIC"
    return "DATA-DEP(cal)"
order=sorted(mu.items(), key=lambda kv:-kv[1]["ops"])
for key,m in order:
    dom,top=key
    regs=", ".join("%s0x%s"%(f,a.lstrip('0x').rjust(4,'0') if False else a[2:] if a.startswith('0x') else a) for (f,a),_ in m["regs"].most_common(6))
    regs=", ".join("%s%s"%(f,a) for (f,a),_ in m["regs"].most_common(6))
    print("\n[%s]  occ=%d  ops=%d  rd=%d wr=%d mod=%d  cores=%d"%(
        feas(m),m["occ"],m["ops"],m.get("rd",0),m.get("wr",0),m.get("mod",0),len(m["cores"])))
    print("   dom=%s  seqs=%s%s"%(dom, ", ".join("%s-%s"%s for s in m["seqs"][:3]), " …" if len(m["seqs"])>3 else ""))
    print("   regs:",regs)
