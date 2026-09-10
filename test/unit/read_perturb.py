#!/usr/bin/env python3
"""Which of the vendor's consumed reads does the port actually consume?

For every read that `reads.py consumers` marks as copy or rmw in the vendor trace,
run the cold flow with that read perturbed (bits flipped in the value the
oracle hands out) and see whether the port's emitted ops change. CONSUMED if
they do; DISCARDED if the port reads the cell and writes a constant. The
DISCARDED rows with a copy/rmw relation are reads whose value the vendor uses
and the port throws away.

One mask is not enough: a flipped bit the consumer masks away never reaches the
output, so a single-bit run reports as discarded reads that are consumed. Every
mask in MASKS is tried and the read counts as consumed if any of them changes
the trace -- the same correction consumed_reads.sh makes. Two cases stay
undecidable by construction: a read whose consumer rewrites every bit of the
word (the mask has nowhere to land), and a read consumed at one site and
discarded at another, since the verdict is per address and not per site.

Usage, after a gates.sh run that kept its working files:
    GATE_TMP=/tmp/gate test/unit/gates.sh
    python3 reverse-tools/reads.py consumers /tmp/gate/merged > census.txt
    python3 test/unit/read_perturb.py census.txt /tmp/gate/merged | sort
"""
import subprocess, re, sys, os

AC_TRACE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ac_trace')
MASKS = os.environ.get('MASKS',
    '0x0001 0x0002 0x0004 0x0010 0x0040 0x0100 0x0400 0x1000 0x4000 0x8000').split()

# candidates: every read key in the census
cands=[]
for l in open(sys.argv[1]):
    m=re.match(r'(PHY|RAD|OBJ) 0x([0-9a-f]+)\s+n=\s*(\d+)\s+(.*?)\s+e\.g\.',l)
    t=re.match(r'TBL 0x([0-9a-f]+)\[0x([0-9a-f]+)\]\s+n=\s*(\d+)\s+(.*?)\s+e\.g\.',l)
    if m: cands.append((m.group(1), int(m.group(2),16), None, m.group(4)))
    elif t: cands.append(('TBL', int(t.group(2),16), int(t.group(1),16), t.group(4)))
env0=dict(os.environ, AC_CHANNEL='36', AC_BW='20', AC_MAC_WIDTH='0', AC_FIRST_INIT='1', AC_READ_ORACLE=sys.argv[2], AC_READ_ORACLE_FROM='528')
def run(env):
    r=subprocess.run([AC_TRACE,'full','d6220'],env=env,capture_output=True,text=True)
    # drop the read lines themselves (perturbed value shows there trivially)
    return [l for l in r.stdout.split('\n') if '.RD ' not in l]
base=run(env0)
print(len(cands),'candidates', file=sys.stderr)
for cls,addr,tid,kinds in cands:
    if 'copy' not in kinds and 'rmw' not in kinds: continue
    changed, via = 0, None
    for mask in MASKS:
        env=dict(env0, AC_READ_PERTURB=hex(addr), AC_READ_PERTURB_MASK=mask)
        if cls=='TBL': env['AC_READ_PERTURB_KIND']=f'tbl:{tid:#x}'
        elif cls=='RAD': env['AC_READ_PERTURB_KIND']='radio'
        elif cls=='OBJ': env['AC_READ_PERTURB_KIND']='obj'
        out=run(env)
        d=sum(1 for a,b in zip(base,out) if a!=b)+abs(len(base)-len(out))
        if d>changed: changed, via = d, mask
    key=f"TBL 0x{tid:04x}[0x{addr:04x}]" if cls=='TBL' else f"{cls} 0x{addr:04x}"
    print(f"{'CONSUMED' if changed else 'DISCARDED'}  {key:22s} changed_lines={changed:4d} via={via or '-':6s} vendor: {kinds}")
