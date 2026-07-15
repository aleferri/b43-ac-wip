#!/usr/bin/env python3
"""Differential correlation: which written values track which NVRAM.

Exact-value matching is useless because every SROM value is transformed
before it is written. Instead we compare two traces and look at what
*changes*:

  * two boards, different NVRAM, same code path  -> changes are NVRAM-driven
    (plus chip/topology differences, which we flag)
  * one board, two channels/bandwidths           -> changes are driven by the
    band/bw-indexed NVRAM arrays (pa5ga[grp], pa5gbwXX, rxgains_5g{l,m,h})

Each PHY table is compared cell-by-cell; the set of differing cells per
table id is the influence footprint. Correlating that against the NVRAM
diff tells us which variable family drives which table, without ever
needing to know the transform.
"""
import re
import sys
from pathlib import Path


def tables(path):
    """tbl id -> list of data words (0x000f stream, concatenated in order)."""
    out = {}
    cur = None
    for line in Path(path).read_text(errors='replace').splitlines():
        m = re.search(r'TBL.WR   id=0x([0-9a-fA-F]+) off=0x([0-9a-fA-F]+)', line)
        if m:
            cur = int(m.group(1), 16)
            out.setdefault(cur, [])
            continue
        if cur is not None and 'addr=0x000f' in line:
            v = re.search(r'val=0x([0-9a-fA-F]+)', line)
            if v:
                out[cur].append(int(v.group(1), 16))
        elif re.search(r'(PHY|RAD)\.(WR|MOD)', line):
            a = re.search(r'addr=0x([0-9a-fA-F]+)', line)
            if a and int(a.group(1), 16) not in (0x000d, 0x000e, 0x000f, 0x0010, 0x0011):
                cur = None                         # real reg write ends the burst
    return out


def reg_config(path):
    """addr -> the single value written, for addrs written exactly one value."""
    from collections import defaultdict
    vals = defaultdict(set)
    for line in Path(path).read_text(errors='replace').splitlines():
        if 'PHY.MOD' not in line and 'PHY.WR' not in line and 'RAD.WR' not in line:
            continue
        a = re.search(r'addr=0x([0-9a-fA-F]+)', line)
        v = re.search(r'val=0x([0-9a-fA-F]+)', line)
        if not (a and v):
            continue
        ai = int(a.group(1), 16)
        if ai in (0x000d, 0x000e, 0x000f, 0x0010, 0x0011):
            continue
        vals[ai].add(int(v.group(1), 16) & 0xFFFF)
    return {a: next(iter(s)) for a, s in vals.items() if len(s) == 1}


def load_nvram(path):
    nv = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r'^([a-z0-9_]+)=(.+)$', line.strip())
        if m:
            nv[m.group(1)] = m.group(2)
    return nv


def diff_tables(A, B, label):
    print(f"\n-- table diff: {label}")
    common = sorted(set(A) & set(B))
    for tid in common:
        a, b = A[tid], B[tid]
        n = min(len(a), len(b))
        d = [i for i in range(n) if a[i] != b[i]]
        if d:
            span = f"{d[0]}..{d[-1]}" if len(d) > 1 else str(d[0])
            print(f"   tbl 0x{tid:02x}: {len(d)}/{n} cells differ (idx {span})"
                  f"   e.g. [{d[0]}] 0x{a[d[0]]:x} vs 0x{b[d[0]]:x}")
    only = (set(A) ^ set(B))
    if only:
        print(f"   tables present in only one: {sorted(hex(t) for t in only)}")


def diff_regs(A, B, label):
    print(f"\n-- config-register diff: {label}")
    common = sorted(set(A) & set(B))
    diffs = [(a, A[a], B[a]) for a in common if A[a] != B[a]]
    for a, va, vb in diffs[:40]:
        print(f"   reg 0x{a:04x}: 0x{va:04x} vs 0x{vb:04x}")
    print(f"   ({len(diffs)} differing single-value config regs)")


def diff_nvram(a, b, label):
    print(f"\n-- NVRAM diff: {label}")
    keys = sorted(set(a) & set(b))
    diffs = [(k, a[k], b[k]) for k in keys if a[k] != b[k]]
    for k, va, vb in diffs:
        print(f"   {k:<24} {va[:24]:<24} | {vb[:24]}")
    only = set(a) ^ set(b)
    print(f"   ({len(diffs)} differing shared keys; {len(only)} keys unique to one)")


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
    rd = root / 'router-data'
    d6220 = rd / 'd6220/wl-diag-wl1-down-to-bss-up.txt'
    ag36 = rd / 'agcombo/agcombo-wl1-4360-down-to-bss-ch36.txt'
    ag100 = rd / 'agcombo/agcombo-wl1-4360-down-to-bss-ch100-bw80.txt'

    print("=" * 74)
    print("A) SAME NVRAM, different channel/bw: agcombo ch36 vs ch100/bw80")
    print("   -> every diff is driven by band/bw-indexed NVRAM arrays")
    print("=" * 74)
    diff_tables(tables(ag36), tables(ag100), "agcombo ch36 vs ch100bw80")
    diff_regs(reg_config(ag36), reg_config(ag100), "agcombo ch36 vs ch100bw80")

    print("\n" + "=" * 74)
    print("B) DIFFERENT NVRAM, same channel: d6220 vs agcombo, both ch36")
    print("   -> diffs are NVRAM + chip/topology (3x3 vs 2x2) confounded")
    print("=" * 74)
    diff_tables(tables(d6220), tables(ag36), "d6220 vs agcombo ch36")
    diff_nvram(load_nvram(rd / 'd6220/wl1_nvram.txt'),
               load_nvram(rd / 'agcombo/agcombo_nvram.txt'),
               "d6220 vs agcombo")


if __name__ == '__main__':
    main()
