#!/usr/bin/env python3
"""Which of the values the driver writes are board data, not code.

A value transcribed from a capture and a value derived from the SROM look
identical in the source: both are integer literals. The difference only shows
up on a different board, which is exactly when it is too late. Three methods,
each attacking that from a different side:

  literals    Every literal in the driver that equals a value the reference
              board's SROM declares is a candidate for "this is board data,
              not a magic number". Two stages, because stage one alone is
              noisy: small ubiquitous values (0x0001, 0x0002) collide with
              SROM fields by chance, so matches are ranked and the ones worth
              reading are the specific ones -- a value used a handful of times
              that belongs to a named power or gain field. With --canonical,
              Broadcom's own bcmsrom_tbl.h decides: if the field exists there
              the match is real, if it does not the value match was luck.

              Confirmed by this method so far: `tssifloor5g[grp]`, hardcoded
              as 0x03ff at PHY 0x0724. The field is unprogrammed on the
              reference boards and reads 0x3ff, i.e. exactly the constant, so
              the transcription was invisible.

  correlate   Exact-value matching is useless because every SROM value is
              transformed before it is written, so instead two captures are
              compared and what *changes* is read:

                two boards, different NVRAM, same path -> changes are
                    NVRAM-driven (plus chip and topology differences)
                one board, two channels or bandwidths  -> changes are driven
                    by the band/bw-indexed arrays (pa5ga[grp], pa5gbwXX,
                    rxgains_5g{l,m,h})

              Each PHY table is compared cell by cell; the set of differing
              cells per table id is the influence footprint. Correlating that
              against the NVRAM diff says which variable family drives which
              table, without ever needing to know the transform.

  verify      The other two suggest; this one checks. For the four SROM inputs
              the ported driver actually reads from bus_sprom, it applies that
              consumer's transform and looks for the result in the trace
              window of the function that consumes it:

                rxchain        -> coremask                 structural
                subband5gver   -> pa5g_group(freq)         slice selector
                rxgains_5gl    -> rxgain_init              transformed regs
                core_pwr_info  -> txpwrctrl_setup est_pwr  transfer function

              Anything else in the NVRAM is not consumed on the PHY path in
              this port and is deliberately out of scope.

The captures are passed as arguments: this used to be a set of hardcoded paths,
which stopped resolving when the attach captures were replaced by the sweep
archives. Any capture works, including a segment out of an archive.

Usage:
  srom.py literals  [--nvram FILE] [--min-uses N] [--canonical bcmsrom_tbl.h]
  srom.py correlate A.txt B.txt [--nvram-a F] [--nvram-b F] [--label TEXT]
  srom.py verify    NVRAM CAPTURE [CAPTURE ...] [--freq 5180]
"""
import argparse
import collections
import glob
import os
import re
import sys

import tracelib as T

NV_RE = re.compile(r"^([a-z0-9_]+)=(.+)$")

# The PHY table ports: an access to one of these is table mechanism, not a
# register write, and must not be read as configuration.
TABLE_PORTS = (0x000d, 0x000e, 0x000f, 0x0010, 0x0011)

DRIVER = os.path.join(T.SRC, "phy_ac.c")

# SROM fields describing power, gain or the front end: a match here is worth
# more than one on an administrative field (boardnum, ccode, ...).
RF_HINT = re.compile(
    r"maxp|pa[0-9]|pdgain|epagain|tssi|elna|triso|trelnabyp|rxgain|noiselvl|"
    r"femctrl|papdcap|tworange|pdoffset|measpower|gainctrl|edthresh|agbg|aga",
    re.I)


def load_nvram(path):
    """field -> raw value string, from an NVRAM dump."""
    nv = {}
    for line in open(path, errors="replace"):
        m = NV_RE.match(line.strip())
        if m:
            nv[m.group(1)] = m.group(2)
    return nv


def hexlist(s):
    return [int(x, 16) if x.lower().startswith("0x") else int(x)
            for x in s.split(",")]


def default_nvram():
    cand = sorted(glob.glob(os.path.join(T.DATA, "*", "*nvram*")))
    if not cand:
        sys.exit("no NVRAM dump under router-data/: pass one explicitly")
    return cand[0]


# ---------------------------------------------------------------------------
# literals
# ---------------------------------------------------------------------------

def driver_constants(path):
    src = open(path, errors="replace").read()
    # comments carry trace episode numbers: those are not code constants
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    c = collections.Counter()
    for m in re.finditer(r"\b0x([0-9a-fA-F]{2,4})\b", src):
        v = int(m.group(1), 16)
        if 0 < v <= 0xffff:
            c[v] += 1
    return c


def srom_values(path):
    """value -> the set of NVRAM fields that contain it."""
    out = collections.defaultdict(set)
    for line in open(path, errors="replace"):
        if "=" not in line:
            continue
        k, v = line.strip().split("=", 1)
        for tok in re.split(r"[,\s]+", v):
            tok = tok.strip()
            if not tok:
                continue
            try:
                n = int(tok, 16) if tok.lower().startswith("0x") else int(tok)
            except ValueError:
                continue
            if 0 < n <= 0xffff:
                out[n].add(k)
    return out


def canonical_fields(path):
    """field name -> (word symbol, mask), from bcmsrom_tbl.h."""
    out = {}
    for line in open(path, errors="replace"):
        m = re.match(r'\s*\{\s*"([a-z0-9_]+)"\s*,\s*\S+\s*,\s*[^,]+,\s*'
                     r"(SROM\w+)\s*,\s*(0x[0-9a-fA-F]+)", line)
        if m:
            out.setdefault(m.group(1), (m.group(2), m.group(3)))
    return out


def mode_literals(args):
    nvram = args.nvram or default_nvram()
    if not os.path.exists(DRIVER):
        sys.exit(f"{DRIVER} not found")

    consts = driver_constants(DRIVER)
    srom = srom_values(nvram)
    canon = canonical_fields(args.canonical) if args.canonical else None

    print(f"driver: {len(consts)} distinct constants")
    print(f"SROM:   {len(srom)} distinct values from {nvram}")
    if canon:
        print(f"canonical: {len(canon)} fields from {args.canonical}")
    print()

    rows = []
    for v, n in consts.items():
        if v not in srom:
            continue
        if not (args.min_uses <= n <= args.max_uses):
            continue
        fields = sorted(srom[v])
        rf = [f for f in fields if RF_HINT.search(f)]
        if not rf:
            continue
        conf = ""
        if canon:
            known = [f for f in rf
                     if re.sub(r"[0-9]$", "", f) in canon or f in canon]
            conf = "canonical" if known else "NOT canonical"
        rows.append((len(rf), n, v, rf, conf))

    rows.sort(key=lambda t: (t[1], -t[0]))
    print("candidates: constants matching an RF or power SROM field")
    print(f"{'value':>8} {'uses':>4} {'confirmation':>16}   fields")
    for _, n, v, rf, conf in rows:
        print(f"  0x{v:04x} {n:4} {conf:>16}   {', '.join(rf[:4])}"
              f"{' ...' if len(rf) > 4 else ''}")
    print()
    print("How to read it: few uses plus a power or gain field means check the")
    print("code. Many uses is almost certainly coincidence. The canonical")
    print("confirmation says whether the field really exists in Broadcom's")
    print("layout.")
    return 0


# ---------------------------------------------------------------------------
# correlate
# ---------------------------------------------------------------------------

def tables(path):
    """table id -> the data words of its writes, concatenated in order."""
    out, cur = {}, None
    with T.open_trace(path) as f:
        for line in f:
            m = re.search(r"TBL\.WR\s+id=0x([0-9a-fA-F]+) off=0x([0-9a-fA-F]+)",
                          line)
            if m:
                cur = int(m.group(1), 16)
                out.setdefault(cur, [])
                continue
            if cur is not None and "addr=0x000f" in line:
                v = re.search(r"val=0x([0-9a-fA-F]+)", line)
                if v:
                    out[cur].append(int(v.group(1), 16))
            elif re.search(r"(PHY|RAD)\.(WR|MOD)", line):
                a = re.search(r"addr=0x([0-9a-fA-F]+)", line)
                if a and int(a.group(1), 16) not in TABLE_PORTS:
                    cur = None            # a real register write ends the burst
    return out


def reg_config(path):
    """addr -> the value written, for addresses written exactly one value."""
    vals = collections.defaultdict(set)
    with T.open_trace(path) as f:
        for line in f:
            if not re.search(r"PHY\.(MOD|WR)|RAD\.WR", line):
                continue
            a = re.search(r"addr=0x([0-9a-fA-F]+)", line)
            v = re.search(r"val=0x([0-9a-fA-F]+)", line)
            if not (a and v):
                continue
            ai = int(a.group(1), 16)
            if ai in TABLE_PORTS:
                continue
            vals[ai].add(int(v.group(1), 16) & 0xFFFF)
    return {a: next(iter(s)) for a, s in vals.items() if len(s) == 1}


def diff_tables(A, B):
    print("\n-- table diff")
    for tid in sorted(set(A) & set(B)):
        a, b = A[tid], B[tid]
        n = min(len(a), len(b))
        d = [i for i in range(n) if a[i] != b[i]]
        if d:
            span = f"{d[0]}..{d[-1]}" if len(d) > 1 else str(d[0])
            print(f"   tbl 0x{tid:02x}: {len(d)}/{n} cells differ (idx {span})"
                  f"   e.g. [{d[0]}] 0x{a[d[0]]:x} vs 0x{b[d[0]]:x}")
    only = set(A) ^ set(B)
    if only:
        print(f"   tables present in only one: {sorted(hex(t) for t in only)}")


def diff_regs(A, B):
    print("\n-- config-register diff")
    diffs = [(a, A[a], B[a]) for a in sorted(set(A) & set(B)) if A[a] != B[a]]
    for a, va, vb in diffs[:40]:
        print(f"   reg 0x{a:04x}: 0x{va:04x} vs 0x{vb:04x}")
    print(f"   ({len(diffs)} differing single-value config regs)")


def diff_nvram(a, b):
    print("\n-- NVRAM diff")
    diffs = [(k, a[k], b[k]) for k in sorted(set(a) & set(b)) if a[k] != b[k]]
    for k, va, vb in diffs:
        print(f"   {k:<24} {va[:24]:<24} | {vb[:24]}")
    only = set(a) ^ set(b)
    print(f"   ({len(diffs)} differing shared keys; "
          f"{len(only)} keys unique to one)")


def mode_correlate(args):
    label = args.label or f"{os.path.basename(args.a)} vs " \
                          f"{os.path.basename(args.b)}"
    print("=" * 74)
    print(label)
    if args.nvram_a and args.nvram_b:
        print("   different NVRAM: the diffs are NVRAM plus chip and topology,")
        print("   confounded")
    else:
        print("   same NVRAM: every diff is driven by the band/bw-indexed")
        print("   NVRAM arrays")
    print("=" * 74)

    diff_tables(tables(args.a), tables(args.b))
    diff_regs(reg_config(args.a), reg_config(args.b))
    if args.nvram_a and args.nvram_b:
        diff_nvram(load_nvram(args.nvram_a), load_nvram(args.nvram_b))
    return 0


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------

def cdiv(a, b):
    """C integer division: truncates toward zero."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def est_pwr_lut(a1, b0, b1):
    def s16(x):
        return x - 0x10000 if x >= 0x8000 else x
    a1, b0, b1 = s16(a1), s16(b0), s16(b1)
    num, den, out = b0 << 9, 0x8000, []
    for _ in range(128):
        d = den or 1
        v = max(-8, min(0x7f, cdiv(d // 2 + num, d)))
        out.append(v & 0xff)
        num += b1 * 0x20
        den += a1
    return out


def pa5g_group(freq, subband5gver):
    if subband5gver == 4:
        return 0 if freq < 5250 else 1 if freq < 5500 else 2 if freq < 5700 \
            else 3
    return 0                          # other versions not exercised here


def read_lines(paths):
    out = []
    for p in paths:
        with T.open_trace(p) as f:
            out += [l.rstrip("\n") for l in f]
    return out


def find_reg(lines, addr, val, mask=None):
    pat = f"addr=0x{addr:04x} val=0x{val:04x}"
    if mask is not None:
        pat += f" mask=0x{mask:04x}"
    return [re.search(r"#(\d+)", l).group(1) for l in lines
            if pat in l and re.search(r"#(\d+)", l)]


def table_data(lines, tid):
    """First write of PHY table `tid`: the sequence of 0x000f data bytes."""
    key = f"TBL.WR   id=0x{tid:04x}"
    for i, l in enumerate(lines):
        if key in l:
            data, started = [], False
            for l2 in lines[i + 1:]:
                m = re.search(r"addr=0x000f val=0x([0-9a-fA-F]+)", l2)
                if m:
                    data.append(int(m.group(1), 16) & 0xff)
                    started = True
                elif started and ("TBL.WR" in l2 or "addr=0x000d" in l2):
                    break
            return data
    return None


def mode_verify(args):
    nv = load_nvram(args.nvram)
    lines = read_lines(args.captures)
    freq = args.freq
    print(f"\n{'='*70}\n{os.path.basename(args.nvram)}   "
          f"(channel freq {freq} MHz)\n{'='*70}")

    # 1. rxchain -> coremask -> number of active per-core est_pwr tables
    coremask = int(nv["rxchain"]) & 7 or 3
    n_cores = bin(coremask).count("1")
    written = [tid for tid in (0x40, 0x60, 0x80) if table_data(lines, tid)]
    print(f"[rxchain={nv['rxchain']}] coremask=0x{coremask:x} -> {n_cores} "
          f"cores; est_pwr tables written: {[hex(t) for t in written]}  "
          f"{'OK' if len(written) == n_cores else 'MISMATCH'}")

    # 2. subband5gver -> pa5g group slice for this freq
    sv = int(nv["subband5gver"], 16)
    grp = pa5g_group(freq, sv)
    print(f"[subband5gver=0x{sv:x}] pa5g group for {freq} MHz = {grp} "
          f"(uses pa5gaX[{grp*3}:{grp*3+3}])")

    # 3. rxgains_5gl -> rxgain_init transforms (per core)
    for core in range(n_cores):
        triso = int(nv[f"rxgains5gtrisoa{core}"])
        gainctx = ((triso + 4) << 1) + 2
        hits = find_reg(lines, 0x06f9 + core * 0x200,
                        (gainctx << 8) & 0x7f00, 0x7f00)
        print(f"[rxgains_5gl core{core}] triso={triso} -> "
              f"gainctx=0x{gainctx:02x} -> reg 0x{0x6f9+core*0x200:04x} "
              f"val=0x{(gainctx<<8)&0x7f00:04x}: "
              f"{'FOUND ' + hits[0] if hits else 'not found'}")

    # 4. core_pwr_info.pa5ga -> est_pwr transfer function -> tables
    tid_for = {0: 0x40, 1: 0x60, 2: 0x80}
    for core in range(n_cores):
        pa = hexlist(nv[f"pa5ga{core}"])[grp * 3: grp * 3 + 3]
        want = est_pwr_lut(*pa)
        got = table_data(lines, tid_for[core])
        n = sum(a == b for a, b in zip(got or [], want))
        print(f"[pa5ga{core}[grp{grp}]] est_pwr LUT -> "
              f"tbl 0x{tid_for[core]:02x}: {n}/128 bytes "
              f"{'EXACT' if got == want else 'DIFF'}")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("literals", help="driver constants equal to SROM values")
    p.add_argument("--nvram")
    p.add_argument("--min-uses", type=int, default=1)
    p.add_argument("--max-uses", type=int, default=20,
                   help="above this a match is almost certainly chance "
                        "(default 20)")
    p.add_argument("--canonical", help="path to bcmsrom_tbl.h")
    p.set_defaults(fn=mode_literals)

    p = sub.add_parser("correlate", help="what changes between two captures")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--nvram-a")
    p.add_argument("--nvram-b")
    p.add_argument("--label")
    p.set_defaults(fn=mode_correlate)

    p = sub.add_parser("verify", help="the four consumed inputs, at their "
                                      "consumption points")
    p.add_argument("nvram")
    p.add_argument("captures", nargs="+")
    p.add_argument("--freq", type=int, default=5180)
    p.set_defaults(fn=mode_verify)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
