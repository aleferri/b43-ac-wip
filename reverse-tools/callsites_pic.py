#!/usr/bin/env python3
"""Trova i siti di chiamata in un modulo MIPS PIC (-mabicalls).

In un modulo del kernel MIPS non ci sono `jal`: le chiamate materializzano
l'indirizzo con una coppia lui/addiu (rilocazioni HI16/LO16) e poi fanno
`jalr $reg`. Cercare R_MIPS_26 non trova nulla -- quelle sono salti locali.

Per patchare un sito servono tre verifiche, perche' la coppia da sola non basta:

  1. la coppia deve essere seguita da un `jalr` sullo STESSO registro entro
     poche istruzioni. Altrimenti e' un indirizzo preso e non una chiamata
     (puntatore a funzione salvato in una tabella, per esempio).
  2. l'`addiu` puo' essere CONDIVISO fra siti diversi: il compilatore riusa la
     parte bassa. Patchare una coppia condivisa cambia piu' siti insieme.
  3. il registro del `jalr` deve essere quello caricato dalla coppia, non un
     altro rimasto in giro.

Uso: callsites_pic.py file.o simbolo [simbolo ...]
"""
import bisect
import re
import struct
import subprocess
import sys

R_HI16, R_LO16 = 5, 6
SPECIAL_JR, SPECIAL_JALR = 0x08, 0x09


def load(path):
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    sec = {}
    for m in re.finditer(r'\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', out):
        sec[int(m.group(1))] = dict(name=m.group(2), addr=int(m.group(4), 16),
                                    off=int(m.group(5), 16), size=int(m.group(6), 16))
    syms, byidx = [], {}
    for l in subprocess.run(['readelf', '-sW', path], capture_output=True, text=True).stdout.splitlines():
        c = l.split()
        if len(c) < 8:
            continue
        try:
            i = int(c[0].rstrip(':'))
            e = (i, int(c[1], 16), int(c[2]), c[3],
                 int(c[6]) if c[6].isdigit() else -1, c[7])
        except (ValueError, IndexError):
            continue
        syms.append(e)
        byidx[i] = e
    return open(path, 'rb').read(), sec, syms, byidx


def main():
    path, targets = sys.argv[1], sys.argv[2:]
    data, sec, syms, byidx = load(path)
    text = next(i for i, s in sec.items() if s['name'] == '.text')
    rel = next(i for i, s in sec.items() if s['name'] == '.rel.text')
    tx, rs = sec[text], sec[rel]

    funcs = sorted((a, sz, n) for i, a, sz, t, sh, n in syms if t == 'FUNC' and sh == text)
    starts = [a for a, _, _ in funcs]

    def fn_at(a):
        i = bisect.bisect_right(starts, a) - 1
        if i < 0:
            return None
        ad, sz, n = funcs[i]
        return n if a < ad + max(sz, 1) else None

    byname = {n: a for a, sz, n in funcs}

    rels = []
    for k in range(rs['size'] // 8):
        o, info = struct.unpack('>II', data[rs['off'] + k * 8:rs['off'] + k * 8 + 8])
        rels.append((o, info >> 8, info & 0xff))

    def word(o):
        return struct.unpack('>I', data[tx['off'] + o:tx['off'] + o + 4])[0]

    def imm(o):
        return word(o) & 0xffff

    # conta quante volte ogni offset di addiu e' usato come parte bassa
    lo_use = {}
    pairs = []
    for i, (o, si, t) in enumerate(rels):
        if t != R_HI16:
            continue
        for j in range(i + 1, min(i + 12, len(rels))):
            o2, si2, t2 = rels[j]
            if t2 == R_LO16 and si2 == si:
                s = byidx.get(si)
                base = s[1] if s else 0
                a = (imm(o) << 16) + struct.unpack('>h', struct.pack('>H', imm(o2)))[0] + base
                pairs.append((o, o2, a))
                lo_use[o2] = lo_use.get(o2, 0) + 1
                break

    for tname in targets:
        ta = byname.get(tname)
        print(f"\n=== {tname}" + ("" if ta is not None else "  (simbolo assente)"))
        if ta is None:
            continue
        mine = [(o, o2, a) for o, o2, a in pairs if a == ta]
        print(f"  {len(mine)} coppie lui/addiu puntano qui\n")
        print(f"  {'lui':>10} {'addiu':>10} {'cond':>4} {'reg':>4} {'salto':>10} {'tipo':>9}  chiamante")
        ok = 0
        for o, o2, a in sorted(mine):
            rt = (word(o) >> 16) & 31          # registro caricato dal lui
            # cerca un jalr sullo stesso registro entro 8 istruzioni dall'addiu
            jalr_at = None
            kind = '-'
            for k in range(0, 9):
                w = word(o2 + k * 4)
                if (w >> 26) != 0 or ((w >> 21) & 31) != rt:
                    continue
                f = w & 0x3f
                if f == SPECIAL_JALR:
                    jalr_at, kind = o2 + k * 4, 'jalr'
                    break
                if f == SPECIAL_JR:
                    jalr_at, kind = o2 + k * 4, 'jr(tail)'
                    break
            shared = lo_use.get(o2, 1)
            ok += jalr_at is not None and shared == 1
            print(f"  0x{o:08x} 0x{o2:08x} {shared:4} ${rt:<3} "
                  f"{('0x%08x' % jalr_at) if jalr_at else '-':>10} {kind:>9}  {fn_at(o)}")
        print(f"\n  patchabili senza effetti collaterali: {ok}/{len(mine)}")
        print("  (serve un salto sullo stesso registro E addiu non condiviso)")


if __name__ == '__main__':
    main()
