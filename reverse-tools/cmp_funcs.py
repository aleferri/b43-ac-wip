#!/usr/bin/env python3
"""Compare the function bodies of two or more MIPS32 ELF builds of wl.

Two levels of sameness, and the second one is the point of the tool:

  byte        identical byte for byte
  constants   same instructions with different constants. The 16-bit
              immediate of the I-types and the 26-bit target of the J-types
              are zeroed. In a REL file the relocation addend lives inside the
              instruction, so without this step every difference of layout
              counts as a difference of code.

Only the functions present in both AND of equal size are compared: a
different size means different code, and there is nothing to compare.

With two builds it reports the differing functions one by one; with more, a
pairwise matrix with the name overlap (jaccard) as well, which is what says
whether two builds are the same driver at all before the bodies are worth
comparing.

Usage:
  cmp_funcs.py A.o B.o [--filter substring]
  cmp_funcs.py label=A.o label=B.o label=C.o [...] [--filter substring]
"""
import argparse
import itertools
import re
import subprocess
import sys

I_TYPE = set(range(0x04, 0x10)) | set(range(0x20, 0x30)) | {
    0x30, 0x31, 0x32, 0x33, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3d, 0x3e, 0x3f,
}
J_TYPE = {0x02, 0x03}


def load(path, filter_):
    """{function name: body bytes} for the FUNC symbols matching the filter.

    The body is read from the file at the section offset, so it works on a REL
    object that was never linked: the symbol address is relative to its
    section, and the section header gives both the virtual address and the
    file offset to translate it.
    """
    out = subprocess.run(['readelf', '-SW', path],
                         capture_output=True, text=True).stdout
    sec = {}
    for m in re.finditer(
            r'\[\s*(\d+)\]\s+\S+\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)', out):
        sec[int(m.group(1))] = (int(m.group(2), 16), int(m.group(3), 16))
    data = open(path, 'rb').read()
    fn = {}
    for l in subprocess.run(['readelf', '-sW', path],
                            capture_output=True, text=True).stdout.splitlines():
        c = l.split()
        if len(c) < 8 or c[3] != 'FUNC' or filter_ not in c[7]:
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
    """The instruction words with the constants masked out."""
    out = []
    for i in range(0, len(b) - 3, 4):
        w = int.from_bytes(b[i:i + 4], 'big')
        op = w >> 26
        out.append(op << 26 if op in J_TYPE else
                   w & 0xFFFF0000 if op in I_TYPE else w)
    return out


def classify(A, B):
    """(common, equal size, byte-identical, same-up-to-constants, other)."""
    common = sorted(set(A) & set(B))
    same = [k for k in common if len(A[k]) == len(B[k])]
    ident = [k for k in same if A[k] == B[k]]
    modk = [k for k in same
            if A[k] != B[k] and shapes(A[k]) == shapes(B[k])]
    other = [k for k in same if k not in ident and k not in modk]
    return common, same, ident, modk, other


def pair_report(A, B):
    common, same, ident, modk, other = classify(A, B)
    print(f"common {len(common)}, of equal size {len(same)}")
    print(f"  byte identical               {len(ident)}")
    print(f"  identical up to constants    {len(modk)}")
    print(f"  different instructions       {len(other)}")
    for k in other:
        n = sum(1 for x, y in zip(shapes(A[k]), shapes(B[k])) if x != y)
        print(f"      {k}  ({n} differing instructions of {len(A[k]) // 4})")


def matrix_report(builds):
    print(f"\n{'pair':46} {'jaccard':>8} {'eq size':>9} {'identical':>10} "
          f"{'up to const':>12}")
    for a, b in itertools.combinations(builds, 2):
        A, B = builds[a], builds[b]
        common, same, ident, modk, _ = classify(A, B)
        jac = len(common) / len(set(A) | set(B))
        pc = 100.0 * (len(ident) + len(modk)) / len(same) if same else 0
        print(f"{a + ' <-> ' + b:46} {jac:8.3f} {len(same):9} {len(ident):10} "
              f"{len(ident) + len(modk):6} {pc:4.0f}%")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+', metavar='[label=]file')
    ap.add_argument('--filter', dest='filter_', default='acphy',
                    help="only symbols whose name contains this (default "
                         "'acphy'); pass '' for every FUNC symbol")
    args = ap.parse_args()

    builds = {}
    for arg in args.files:
        label, _, path = arg.rpartition('=')
        path = path or arg
        label = label or path
        builds[label] = load(path, args.filter_)
        print(f"  {label:22} {len(builds[label])} functions")

    if len(builds) < 2:
        sys.exit("at least two builds are needed")
    if len(builds) == 2:
        (A, B) = builds.values()
        pair_report(A, B)
    else:
        matrix_report(builds)


if __name__ == '__main__':
    main()
