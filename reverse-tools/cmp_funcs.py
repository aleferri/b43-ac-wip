#!/usr/bin/env python3
"""Confronta le funzioni omonime di due ELF MIPS32 a due livelli.

  byte       : identiche byte per byte
  a-costanti : stesse istruzioni con costanti diverse. Si azzera l'immediato a
               16 bit delle I-type e il target a 26 delle J-type. In un file REL
               l'addendo delle rilocazioni sta dentro l'istruzione, quindi senza
               questo passaggio ogni differenza di layout conta come differenza
               di codice.

Confronta solo le funzioni comuni E di pari dimensione: dimensione diversa vuol
dire codice diverso e non c'e' niente da confrontare.

Uso: cmp_funcs.py A.o B.o [--filtro sottostringa]
"""
import argparse
import re
import subprocess

I_TYPE = set(range(0x04, 0x10)) | set(range(0x20, 0x30)) | {
    0x30, 0x31, 0x32, 0x33, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3d, 0x3e, 0x3f,
}
J_TYPE = {0x02, 0x03}


def load(path, filtro):
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    sec = {}
    for m in re.finditer(r'\[\s*(\d+)\]\s+\S+\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)', out):
        sec[int(m.group(1))] = (int(m.group(2), 16), int(m.group(3), 16))
    data = open(path, 'rb').read()
    fn = {}
    for l in subprocess.run(['readelf', '-sW', path], capture_output=True, text=True).stdout.splitlines():
        c = l.split()
        if len(c) < 8 or c[3] != 'FUNC' or filtro not in c[7]:
            continue
        try:
            addr, size, shndx = int(c[1], 16), int(c[2]), int(c[6])
        except ValueError:
            continue
        if shndx not in sec or size == 0:
            continue
        va, off = sec[shndx]
        b = data[off + (addr - va):off + (addr - va) + size]
        if len(b) == size:
            fn[c[7]] = b
    return fn


def shapes(b):
    out = []
    for i in range(0, len(b) - 3, 4):
        w = int.from_bytes(b[i:i + 4], 'big')
        op = w >> 26
        out.append(op << 26 if op in J_TYPE else
                   w & 0xFFFF0000 if op in I_TYPE else w)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--filtro', default='acphy')
    args = ap.parse_args()

    A, B = load(args.a, args.filtro), load(args.b, args.filtro)
    common = sorted(set(A) & set(B))
    same = [k for k in common if len(A[k]) == len(B[k])]
    ident = [k for k in same if A[k] == B[k]]
    modk = [k for k in same if A[k] != B[k] and shapes(A[k]) == shapes(B[k])]
    other = [k for k in same if k not in ident and k not in modk]

    print(f"comuni {len(common)}, di pari dimensione {len(same)}")
    print(f"  byte identiche               {len(ident)}")
    print(f"  identiche a meno di costanti {len(modk)}")
    print(f"  istruzioni diverse           {len(other)}")
    for k in other:
        n = sum(1 for x, y in zip(shapes(A[k]), shapes(B[k])) if x != y)
        print(f"      {k}  ({n} istruzioni diverse su {len(A[k]) // 4})")


if __name__ == '__main__':
    main()
