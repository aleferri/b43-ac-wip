#!/usr/bin/env python3
"""Mappa dell'ordine MACRO di set_channel: fasi del driver (firme dal sorgente)
localizzate nel riferimento harness e negli episodi agcombo via accoppiamento
strutturale. Falsificabile: ogni fase e' ancorata a una firma esplicita."""
import re, bisect, sys
from collections import defaultdict, Counter

SIG = [
 ('channel_switch_prep',   'PHY.MOD addr=0x0003 val=0x0100 mask=0x0100'),
 ('radio_2069_chan_setup', 'PHY.MOD addr=0x0728 val=0x0100 mask=0x0100'),
 ('phy_channel_setup',     'PHY.MOD addr=0x02d1 val=0x0040 mask=0x00f0'),
 ('chan_tables',           'PHY.WR addr=0x01ec val=0x9c40'),
 ('rxgain_init',           'PHY.WR addr=0x073e val=0x0000'),
 ('post_noise_shaping',    'PHY.WR addr=0x016c val=0x0000'),
 ('afecal',                'PHY.MOD addr=0x0739 val=0x0080 mask=0x0080'),
 ('adc_reset',             'PHY.MOD addr=0x0070 val=0x0000 mask=0xe000'),
 ('idle_tssi_meas',        'PHY.MOD addr=0x0072 val=0x0004 mask=0x0004'),
 ('txpwrctrl_setup_1',     'PHY.MOD addr=0x0071 val=0x00c8 mask=0x00ff'),
 ('txpwrctrl_setup_2',     'PHY.MOD addr=0x0071 val=0x00c8 mask=0x00ff'),
 ('rxgainctrl_regs',       'PHY.WR addr=0x073e val=0x0440'),
 ('rxcal_radio_setup',     'RAD.MOD addr=0x0161 val=0x4000 mask=0x4000'),
 ('rxcal_tone_setup',      'PHY.WR addr=0x0739 val=0x00fa'),
 ('rxcal_gaincal_core0',   'TBL.WR id=0x000c off=0x0063'),
 ('rxcal_cleanup',         'PHY.WR addr=0x073e val=0x0000'),
 ('rxiq_est_debug',        'PHY.WR addr=0x0272 val=0x0400'),
]
def ck(op):
    op=re.sub(r'\s+',' ',op.strip())
    if op.startswith('TBL.'):
        m=re.match(r'(TBL\.\S+)\s+(id=\S+ off=\S+ len=\S+)',op); return f'{m.group(1)} {m.group(2)}' if m else op
    m=re.match(r'(\S+)\s+(addr=\S+)',op)
    if m:
        t=m.group(1); t='PHY.MOD' if t in ('PHY.OR','PHY.AND') else t; return f'{t} {m.group(2)}'
    return op.split(' ')[0]
CAP=re.compile(r'^\s*[0-9.]+\s+#(\d+)\s+cpu\d+\s+(.+?)\s*(?:;.*)?$'); BARE=re.compile(r'^cpu\d+\s+(.+?)\s*$')
def parse(p):
    o=[];e=[]
    for raw in open(p):
        l=raw.rstrip('\n'); m=CAP.match(l)
        if m: o.append(m.group(2)); e.append(int(m.group(1))); continue
        m=BARE.match(l)
        if m: o.append(m.group(1)); e.append(None)
    return o,e
def squash(seq,eps):
    O=[];E=[]
    for op,x in zip(seq,eps):
        if O and '.RD ' in op and ck(op)==ck(O[-1]): continue
        O.append(op); E.append(x)
    return O,E
if len(sys.argv) < 3:
    sys.exit("usage: macro_order_map.py <harness.collapsed> <vendor.collapsed>\n"
             "  entrambi gli input vanno prima passati da collapse_trace.py")
R,_=parse(sys.argv[1]); R,_=squash(R,[None]*len(R))
V,VE=parse(sys.argv[2]); V,VE=squash(V,VE)
RK=[ck(o) for o in R]; VK=[ck(o) for o in V]; Rn=[re.sub(r'\s+',' ',o) for o in R]
def match_seg(r0,r1,v0,v1,d=0):
    if r0>=r1 or v0>=v1: return []
    cr=Counter(RK[r0:r1]); cv=Counter(VK[v0:v1]); uq={k for k in cr if cr[k]==1 and cv.get(k)==1}
    if uq and d<12:
        rp={RK[j]:j for j in range(r0,r1) if RK[j] in uq}; vp={VK[i]:i for i in range(v0,v1) if VK[i] in uq}
        anc=sorted((rp[k],vp[k]) for k in uq); seq=[v for _,v in anc]; tl=[];ti=[];pv=[-1]*len(seq)
        for t,x in enumerate(seq):
            p=bisect.bisect_left(tl,x)
            if p==len(tl): tl.append(x);ti.append(t)
            else: tl[p]=x;ti[p]=t
            pv[t]=ti[p-1] if p>0 else -1
        keep=set(); t=ti[len(tl)-1] if tl else -1
        while t!=-1: keep.add(t); t=pv[t]
        anc=[a for t,a in enumerate(anc) if t in keep]
        if anc:
            pr=[]; a,b=r0,v0
            for j,i in anc:
                pr+=match_seg(a,j,b,i,d+1); pr.append((j,i)); a,b=j+1,i+1
            pr+=match_seg(a,r1,b,v1,d+1); return pr
    vo=defaultdict(list)
    for i in range(v0,v1): vo[VK[i]].append(i)
    pt=defaultdict(int); pr=[]
    for j in range(r0,r1):
        k=pt[RK[j]]
        if k<len(vo[RK[j]]): pr.append((j,vo[RK[j]][k])); pt[RK[j]]+=1
    return pr
pd=dict(sorted(match_seg(0,len(R),0,len(V))))
found=[]; cur=0
for name,s in SIG:
    for j in range(cur,len(R)):
        if Rn[j].startswith(s): found.append((name,j)); cur=j+1; break
segs=[(n,p,found[k+1][1] if k+1<len(found) else len(R)) for k,(n,p) in enumerate(found)]
rows=[]
for n,p,q in segs:
    eps=sorted(VE[pd[j]] for j in range(p,q) if j in pd and VE[pd[j]] is not None)
    tot=q-p; cov=len(eps)
    rows.append((n,p,q,eps[0] if eps else None,eps[-1] if eps else None,cov,tot))
for n,p,q,lo,hi,cov,tot in rows:
    print(f"{n:24s} ref {p:5d}-{q:5d}  ag #{lo}..#{hi}  {cov}/{tot} ({cov/tot*100:.0f}%)")
