#!/usr/bin/env python3
"""Matrice di similarita' fra build di wl, sui corpi delle funzioni acphy.

Per ogni coppia: sovrapposizione dei nomi (jaccard) e, sulle funzioni di pari
dimensione, quante sono identiche byte per byte e quante a meno delle costanti.

Uso: similarity.py etichetta=file [etichetta=file ...]
"""
import itertools
import re
import subprocess
import sys

I_TYPE = set(range(0x04, 0x10)) | set(range(0x20, 0x30)) | {
    0x30, 0x31, 0x32, 0x33, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3d, 0x3e, 0x3f,
}
J_TYPE = {0x02, 0x03}


def load(path):
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    sec = {}
    for m in re.finditer(r'\[\s*(\d+)\]\s+\S+\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)', out):
        sec[int(m.group(1))] = (int(m.group(2), 16), int(m.group(3), 16))
    data = open(path, 'rb').read()
    fn = {}
    for l in subprocess.run(['readelf', '-sW', path], capture_output=True, text=True).stdout.splitlines():
        c = l.split()
        if len(c) < 8 or c[3] != 'FUNC' or 'acphy' not in c[7]:
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
    builds = {}
    for arg in sys.argv[1:]:
        label, path = arg.split('=', 1)
        builds[label] = load(path)
        print(f"  {label:22} {len(builds[label])} funzioni acphy")

    print(f"\n{'coppia':46} {'jaccard':>8} {'pari dim':>9} {'identiche':>10} {'a-costanti':>11}")
    for a, b in itertools.combinations(builds, 2):
        A, B = builds[a], builds[b]
        common = set(A) & set(B)
        jac = len(common) / len(set(A) | set(B))
        same = [k for k in common if len(A[k]) == len(B[k])]
        ident = sum(1 for k in same if A[k] == B[k])
        modk = sum(1 for k in same if A[k] != B[k] and shapes(A[k]) == shapes(B[k]))
        pc = 100.0 * (ident + modk) / len(same) if same else 0
        print(f"{a + ' <-> ' + b:46} {jac:8.3f} {len(same):9} {ident:10} "
              f"{ident + modk:6} {pc:4.0f}%")


if __name__ == '__main__':
    main()
