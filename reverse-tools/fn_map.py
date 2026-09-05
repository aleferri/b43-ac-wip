#!/usr/bin/env python3
"""Where each port function lands inside a vendor capture.

One question, three ways to answer it, chosen by what is at hand and by how
faithfully the port reproduces the capture:

  coverage      How many of the ops a function emits are found again, in
                order, in the capture -- and which capture ops no function
                covers (the GAPs). This is the completeness measure per
                phase, and it does not require the port to reproduce the
                whole capture: it looks for each function independently of
                the others.

  span          The episode interval each function accounts for, aligning the
                two sequences as a whole. Stronger than coverage, but only
                valid if the port reproduces that capture: it is the global
                alignment that gives the correspondence.

  fingerprints  Like `coverage`, but the per-function sequences are read from
                the sources instead of from the harness markers. Needed when
                the harness cannot reproduce the capture -- another board, or
                another build of the blob -- and there it is the only way.
                Elsewhere it is the weakest of the three: a computed address
                or a loop leaves no literal to search for, so the fingerprint
                is partial.

The two marker-based modes want a harness trace produced with
AC_FN_MARKERS=1; passing `flow:board` instead runs it.

WARNING: pass the capture **raw**, not collapsed by `trace_filter.py
--collapse`. The
collapse drops the ops on the data port, so the functions that write tables
would wrongly come out missing.

Note on the table functions: `tables_init` and `tables_zero_cal` emit hundreds
of data-port ops that the vendor interleaves with the TBL markers in a
different order. They do not align by sequence and must be compared by table
*content*, not by order; here they show a low coverage that is not a defect of
the port.

Usage:
  fn_map.py coverage     <trace|flow:board> <raw-capture>
  fn_map.py span         <trace|flow:board:capture> [<raw-capture>]
  fn_map.py fingerprints <capture> [--src DIR]
"""
import argparse
import bisect
import collections
import glob
import os
import re
import sys

import tracelib as T

# The fingerprint of an op is what makes it recognizable in another trace: a
# write with its value, or a modify with its mask of touched bits. Reads do not
# enter: their value is not a property of the code.
FINGERPRINT_OP = re.compile(r"\b(PHY|RAD)\.(WR|MOD)\b")


def fingerprint(op):
    m = FINGERPRINT_OP.search(op)
    if not m:
        return None
    fam, cls = m.group(1), m.group(2)
    d = dict(kv.split("=", 1) for kv in op.split() if kv.count("=") == 1)
    try:
        a = int(d["addr"], 16)
        if cls == "WR":
            return ("WR", fam, a, int(d["val"], 16))
        return ("MOD", fam, a, int(d.get("mask", "0x0"), 16))
    except (KeyError, ValueError):
        return None


def port_trace(arg):
    """Path of a trace with markers, running the harness if needed."""
    if ":" in arg and not os.path.exists(arg):
        flow, board = arg.split(":", 1)
        return T.write_temp(T.run_port(flow, board))
    return arg


def by_function(path):
    """{function: [fingerprint, ...]} in the order the markers open them.

    A function enters even if it emits no op with a fingerprint: that a phase
    leaves no recognizable literal is an attribute of it, and dropping it from
    the report would make it look uninstrumented.
    """
    ops, events, owner = T.segment(path)
    out = collections.OrderedDict()
    for _, delta, name in events:
        if delta > 0:
            out.setdefault(name, [])
    for op, fn in zip(ops, owner):
        if fn is None:
            continue
        k = fingerprint(op)
        if k:
            out.setdefault(fn, []).append(k)
    return out


def vendor_index(path):
    """([(episode, fingerprint)], {fingerprint: [positions]}).

    The ops go through the normalization in tracelib, so the vendor's PHY.AND
    and PHY.OR enter as PHY.MOD: they are the same access the harness emits in
    that form, and leaving them out makes every phase that only sets and
    clears single bits unlocatable.
    """
    v = []
    for op in T.read_vendor(path):
        k = fingerprint(op.norm)
        if k:
            v.append((op.ep, k))
    idx = collections.defaultdict(list)
    for i, (_, k) in enumerate(v):
        idx[k].append(i)
    return v, idx


# ---------------------------------------------------------------------------
# coverage
# ---------------------------------------------------------------------------

def locate(seq, v, idx, gap=400, seeds=60):
    """Where `seq` lands best in `v`. (found, total, i0, i1).

    Every position of the first op is tried as a seed, then the search moves
    forward looking for the next ones within `gap` positions. The skipping is
    deliberate: between two ops of a function the vendor may have others, its
    own or a callee's, and demanding adjacency would miss almost everything.

    `gap` is the parameter that decides what the result means, and it fails in
    two opposite ways. Too tight: a phase the vendor interleaves with other
    work never closes and the score drops without anything being missing. Too
    loose: the search picks up ops scattered across the whole capture and the
    score rises until it means nothing -- afe_gain_regs_reemit scores 10/10
    over a 19000-episode interval with `gap` unbounded, which is to say it is
    not located at all. The count must always be read together with the
    interval next to it: that is what says whether the phase was found or
    merely collected.

    It is measured in indexed op positions, not in episodes, so its useful
    value depends on how many ops the capture carries per episode.
    """
    if not seq:
        return 0, 0, None, None
    best, span = 0, None
    for st in idx.get(seq[0], [])[:seeds]:
        pos, found, last = st, 1, st
        for e in seq[1:]:
            lst = idx.get(e)
            if not lst:
                continue
            j = bisect.bisect_right(lst, pos)
            if j < len(lst) and lst[j] <= pos + gap:
                pos = last = lst[j]
                found += 1
        if found > best:
            best, span = found, (st, last)
    if span is None:
        return 0, len(seq), None, None
    return best, len(seq), span[0], span[1]


def mode_coverage(args):
    fp = by_function(port_trace(args.port))
    v, idx = vendor_index(args.capture)
    placed = []
    for fn, seq in fp.items():
        found, total, i0, i1 = locate(seq, v, idx, args.gap, args.seeds)
        pct = 100.0 * found / total if total else 0.0
        where = f"#{v[i0][0]}..#{v[i1][0]}" if i0 is not None else "not found"
        print(f"  {fn:36s} {found:4d}/{total:<4d} {pct:5.1f}%  {where}")
        if i0 is not None:
            placed.append((i0, i1))

    if not placed:
        return 0

    lo = min(a for a, _ in placed)
    hi = max(b for _, b in placed)
    covered = [False] * len(v)
    for a, b in placed:
        for i in range(a, b + 1):
            covered[i] = True

    print("  --- GAPs (vendor ops in the span no function covers) ---")
    i, total = lo, 0
    while i <= hi:
        if covered[i]:
            i += 1
            continue
        j = i
        while j <= hi and not covered[j]:
            j += 1
        run = v[i:j]
        total += len(run)
        fam = collections.Counter(f"{k[1]}.{k[0]}" for _, k in run)
        if len(run) >= 5:
            top = ", ".join(f"{n}:{c}" for n, c in fam.most_common(3))
            print(f"      #{v[i][0]}..#{v[j-1][0]}  {len(run):4d} ops  [{top}]")
        i = j
    print(f"      => {total} ops in gaps out of ~{hi - lo + 1} "
          f"({100.0 * total / (hi - lo + 1):.1f}% uncovered)")
    return 0


# ---------------------------------------------------------------------------
# span
# ---------------------------------------------------------------------------

def mode_span(args):
    if args.capture:
        trace, capture = port_trace(args.port), args.capture
    else:
        # a full run: tracelib prepares the read oracle, because without it
        # the port would diverge for a reason that is not a defect of its own
        flow, board, cap, env = T.parse_run(args.port)
        trace, capture = T.run_against(flow, board, cap, env)

    spans, aligned, total = T.spans_by_function(trace, capture)
    print(f"{aligned}/{total} port ops aligned")
    rows = [(min(s[0] for s in ivs), fn, ivs) for fn, ivs in spans.items()]
    for _, fn, ivs in sorted(rows):
        text = ", ".join(f"#{lo}-#{hi}" for lo, hi in ivs)
        print(f"  {fn:44s} {len(ivs):3d}x  {text}")
    return 0


# ---------------------------------------------------------------------------
# fingerprints -- sequences deduced from the sources, without the harness
# ---------------------------------------------------------------------------

CALL = re.compile(r"\bb43_(phy|radio)_(write|set|mask|maskset)\s*\(")
SIGNATURE = re.compile(r"^[A-Za-z].*\b(b43_[a-z0-9_]+)\s*\([^;]*$")


def _symbols(srcdir):
    sym = {}
    for h in glob.glob(srcdir + "/*.h") + glob.glob(srcdir + "/*.c"):
        for l in open(h, errors="ignore"):
            m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\b", l)
            if m:
                sym.setdefault(m.group(1), int(m.group(2), 16))
    return sym


def _value(t, sym):
    t = t.strip()
    m = re.match(r"^\(u16\)\s*~\s*(.+)$", t)
    if m:
        v = _value(m.group(1), sym)
        return None if v is None else (~v) & 0xffff
    if t.startswith("~"):
        v = _value(t[1:], sym)
        return None if v is None else (~v) & 0xffff
    if re.fullmatch(r"0x[0-9a-fA-F]+", t):
        return int(t, 16)
    if re.fullmatch(r"\d+", t):
        return int(t)
    return sym.get(t)


def _args(s):
    out, d, c = [], 0, ""
    for ch in s:
        if ch == "(":
            d += 1
            c += ch
        elif ch == ")":
            if d == 0:
                out.append(c)
                return out
            d -= 1
            c += ch
        elif ch == "," and d == 0:
            out.append(c)
            c = ""
        else:
            c += ch
    out.append(c)
    return out


def fingerprints_from_source(srcdir):
    """{function: [fingerprint, ...]} from the literals in the sources.

    The mask of touched bits comes from the shape of the call: `set` carries it
    directly, `mask` and `maskset` carry it negated (their argument is what is
    kept).
    """
    sym = _symbols(srcdir)
    fp = collections.defaultdict(list)
    for cf in sorted(glob.glob(srcdir + "/*.c")):
        depth, cur = 0, None
        for l in open(cf, errors="ignore").read().splitlines():
            if depth == 0:
                m = SIGNATURE.match(l)
                if m:
                    cur = m.group(1)
            for m in CALL.finditer(l):
                fam = "PHY" if m.group(1) == "phy" else "RAD"
                kind = m.group(2)
                a = [x.strip() for x in _args(l[m.end():])]
                if len(a) < 2 or not cur:
                    continue
                addr = _value(a[1], sym)
                if addr is None:
                    continue
                if kind == "write" and len(a) >= 3:
                    v = _value(a[2], sym)
                    if v is not None:
                        fp[cur].append(("WR", fam, addr, v))
                elif kind == "set" and len(a) >= 3:
                    mk = _value(a[2], sym)
                    if mk is not None:
                        fp[cur].append(("MOD", fam, addr, mk & 0xffff))
                elif kind == "mask" and len(a) >= 3:
                    mk = _value(a[2], sym)
                    if mk is not None:
                        fp[cur].append(("MOD", fam, addr, (~mk) & 0xffff))
                elif kind == "maskset" and len(a) >= 4:
                    keep = _value(a[2], sym)
                    if keep is not None:
                        fp[cur].append(("MOD", fam, addr, (~keep) & 0xffff))
            depth += l.count("{") - l.count("}")
            depth = max(depth, 0)
    return fp


def mode_fingerprints(args):
    fp = fingerprints_from_source(args.src)
    v, idx = vendor_index(args.capture)

    # A fingerprint of fewer than three ops identifies nothing: three literal
    # registers in a row are rare by chance, one is not.
    short = sorted(fn for fn, seq in fp.items() if len(seq) < 3)
    res = []
    for fn, seq in fp.items():
        if len(seq) < 3:
            continue
        used = seq[:10]
        found, total, i0, i1 = locate(used, v, idx, args.gap, args.seeds)
        if i0 is not None and found == total:
            res.append((v[i0][0], v[i1][0], fn, total, len(seq)))
    res.sort()

    print("REAL CALL ORDER (fingerprint = literal WR + mask-MOD)")
    print("start   end     function                               used/tot")
    for s0, s1, fn, u, t in res:
        print(f"#{s0:<6d} #{s1:<6d} {fn:38s} {u}/{t}")

    print("\nSEGMENTATION (each match to the next):")
    for i, (s0, s1, fn, u, t) in enumerate(res):
        end = res[i + 1][0] if i + 1 < len(res) else s1
        print(f"  #{s0:<6d} .. #{end:<6d}  {fn:36s}")

    print("\nUn-fingerprintable (fewer than 3 literal ops):")
    print("  " + ", ".join(short))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("coverage", help="ops found per function, plus the gaps")
    p.add_argument("port", metavar="trace|flow:board")
    p.add_argument("capture")
    p.add_argument("--gap", type=int, default=400,
                   help="how many op positions the search may skip between "
                        "two ops of the same function (default 400); see "
                        "locate() for the two ways it goes wrong")
    p.add_argument("--seeds", type=int, default=60,
                   help="how many positions of the first op to try as a seed "
                        "(default 60)")
    p.set_defaults(fn=mode_coverage)

    p = sub.add_parser("span", help="episode interval per function")
    p.add_argument("port", metavar="trace|flow:board:capture")
    p.add_argument("capture", nargs="?",
                   help="omit it when the first argument is a full run "
                        "flow:board:capture, which also prepares the oracle")
    p.set_defaults(fn=mode_span)

    p = sub.add_parser("fingerprints",
                       help="no harness, literals from the sources")
    p.add_argument("capture")
    p.add_argument("--src", default=T.SRC)
    p.add_argument("--gap", type=int, default=400)
    p.add_argument("--seeds", type=int, default=60)
    p.set_defaults(fn=mode_fingerprints)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
