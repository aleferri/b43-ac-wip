#!/usr/bin/env python3
"""The reads the port discards: are they safe, what are they for, what to plan.

The port sends the reads it does not need through b43_phy_read_log() and
b43_radio_read_log() into a discarded variable, so that the bus order matches
the stock driver's. That is deliberate. It is also where a whole class of
latent bug lives: if the vendor reads a register and its later writes depend
on what came back, then discarding the value means the port is reproducing a
constant that happened to hold on the board the capture came from.

Three modes, and they are the three stages of the same investigation, in
order:

  risk    Which discarded reads have a value that actually VARIES across the
          captures. A discarded read whose value is the same everywhere is at
          worst fragile -- it could still differ on another board. One whose
          value varies is a bug waiting for a different board: the port is not
          learning anything, so any dependent write is transcribed rather than
          derived. Reads the port sources, so it knows which addresses are
          discarded, and needs no harness.

  intent  What the stock driver DOES with a read. "Read for bus order" is not
          an explanation, it is what is left when nobody worked it out. A
          driver reads a register for one of a few reasons and each leaves a
          signature in the capture:

            sample loop     the same address read many times in a row, values
                            varying: the driver is averaging or accumulating.
                            This is what PHY 0x0012 turned out to be, and
                            calling it bus order hid the bug.
            poll            long runs too, but the values converge to a fixed
                            one and the run ends there: waiting on a bit.
            read-modify     a write to the same address follows immediately;
                            the read supplies the bits the write preserves.
            read-to-clear   non-zero, then zero on the next read of the same
                            address with no write in between.
            probe           a lone read whose value never varies anywhere.
                            Nothing is being learnt, so either the read has a
                            side effect or it is genuinely only ordering.

  consumers
          Which write consumes each read's value: the same value written
          elsewhere (copy), the same address rewritten with some bits changed
          (read-modify-write), or a repeat of the read with no write between
          (poll). Reads with none are 'unused'. Stronger than `intent`, which
          classifies by signature: this one follows the VALUE, so it names the
          write that consumes it instead of guessing the shape. Use `intent`
          when the value is not traceable -- it also recognizes read-to-clear,
          which a value search cannot see.

  plan    Emit a C read plan for one register from the values a capture
          recorded. The harness serves reads from three sources in order: the
          read oracle (the whole capture), a scripted plan, and finally the
          mirror of the writes. A flow that runs without an oracle needs a
          plan for any register whose read value steers control flow --
          otherwise the mirror answers with the last value written, which for
          a self-clearing status bit never advances.

Usage:
  reads.py risk      <dir>                   <dir>/<ch>.vendor captures
  reads.py consumers FOLDED.txt [--window N] [--only copy|rmw|unused|poll]
  reads.py intent <dir> [discarded-list]     list from test/consumed_reads.sh
  reads.py plan   ADDR NAME=CAPTURE [...]    > test/readplan_NNNN.h

`risk` and `intent` want captures whose read values are folded in
(`trace_filter.py --retvals`); without the RETVAL lines every value is
UNDEFINED and there is nothing to classify. `plan` folds them itself, so it
takes the raw capture path -- which is also what it records as provenance.
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys
import tempfile

import tracelib as T

READ_LOG = re.compile(r"b43_(phy|radio)_read_log\(dev,\s*([^;)]+)\)")
LITERAL = re.compile(r"^0x([0-9a-fA-F]+)$")

VENDOR_RD = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+"
                       r"(PHY|RAD)\.RD\s+addr=0x([0-9a-fA-F]+)\s+"
                       r"val=0x([0-9a-fA-F]+)")

ACCESS = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+"
                    r"(PHY|RAD)\.(RD|WR|MOD|AND|OR)\s+addr=0x([0-9a-fA-F]+)"
                    r"(?:\s+val=0x([0-9a-fA-F]+))?")

KIND = {"PHY": "phy", "RAD": "radio"}


def captures_in(d):
    paths = sorted(glob.glob(os.path.join(d, "*.vendor")))
    if not paths:
        sys.exit(f"no <channel>.vendor file in {d}")
    return paths


# ---------------------------------------------------------------------------
# risk
# ---------------------------------------------------------------------------

def discarded_addrs():
    """(kind, addr) for every literal read_log site, plus the computed ones."""
    lit = collections.Counter()
    computed = []
    for name in sorted(os.listdir(T.SRC)):
        if not name.endswith(".c"):
            continue
        for lineno, line in enumerate(open(os.path.join(T.SRC, name)), 1):
            for kind, arg in READ_LOG.findall(line):
                arg = arg.strip()
                m = LITERAL.match(arg)
                if m:
                    lit[(kind, int(m.group(1), 16))] += 1
                else:
                    computed.append((name, lineno, kind, arg))
    return lit, computed


def capture_reads(paths):
    """(kind, addr) -> values seen, and the per-file value sets."""
    vals = collections.defaultdict(set)
    per_file = collections.defaultdict(dict)
    for p in paths:
        for line in open(p, errors="replace"):
            m = VENDOR_RD.match(line)
            if not m:
                continue
            key = (KIND[m.group(1)], int(m.group(2), 16))
            v = int(m.group(3), 16)
            vals[key].add(v)
            per_file[key].setdefault(p, set()).add(v)
    return vals, per_file


def mode_risk(args):
    paths = captures_in(args.dir)
    lit, computed = discarded_addrs()
    vals, per_file = capture_reads(paths)

    print(f"captures: {len(paths)}")
    print(f"discarded read sites with a literal address: {sum(lit.values())} "
          f"over {len(lit)} addresses")
    print(f"discarded read sites with a computed address: {len(computed)}")
    print()

    varying, constant, unseen = [], [], []
    for key, n in lit.items():
        seen = vals.get(key)
        if not seen:
            unseen.append((key, n))
        elif len(seen) > 1:
            # does it vary *between* configurations, or only within one?
            across = len({frozenset(s) for s in per_file[key].values()}) > 1
            varying.append((key, n, sorted(seen), across))
        else:
            constant.append((key, n, sorted(seen)[0]))

    varying.sort(key=lambda x: (-len(x[2]), x[0][1]))

    print("LATENT BUGS: value varies across the captures, and is discarded.")
    print("Any write that depends on one of these is transcribed, not derived.")
    print()
    print(f"  {'reg':10s} {'sites':>5s} {'values':>7s}  across configs  values")
    print("  " + "-" * 72)
    for (kind, addr), n, seen, across in varying:
        vs = ", ".join(f"{v:#x}" for v in seen[:6])
        if len(seen) > 6:
            vs += f", ... ({len(seen)} total)"
        print(f"  {kind:5s}{addr:#06x} {n:5d} {len(seen):7d}  "
              f"{'yes' if across else 'no ':^14s}  {vs}")

    print()
    print(f"CONSTANT across every capture: {len(constant)} addresses. Fragile "
          f"rather than wrong -- the value could still differ on another board.")
    print(f"NOT READ in any capture: {len(unseen)} addresses. Either the flow "
          f"is not exercised or the read is the port's own invention.")
    for (kind, addr), n in sorted(unseen, key=lambda x: x[0][1]):
        print(f"  {kind:5s}{addr:#06x}  {n} site(s)")

    if computed:
        print()
        print(f"COMPUTED addresses, not resolvable from the source alone "
              f"({len(computed)} sites); these need the trace to classify:")
        for name, lineno, kind, arg in computed[:15]:
            print(f"  {name}:{lineno}  {kind}  {arg}")
        if len(computed) > 15:
            print(f"  ... and {len(computed) - 15} more")
    return 0


# ---------------------------------------------------------------------------
# intent
# ---------------------------------------------------------------------------

def load_accesses(path):
    """Ordered (kind, addr, op, val) for every register access."""
    out = []
    for line in open(path, errors="replace"):
        m = ACCESS.match(line)
        if not m:
            continue
        val = int(m.group(4), 16) if m.group(4) else None
        out.append((KIND[m.group(1)], int(m.group(3), 16),
                    "rd" if m.group(2) == "RD" else "wr", val))
    return out


def classify(paths, restrict):
    runs = collections.defaultdict(int)        # longest consecutive run
    run_tail = collections.defaultdict(set)    # last value of each long run
    run_spread = collections.defaultdict(set)  # values seen inside long runs
    rmw = collections.Counter()                # read then write, same addr
    reads = collections.Counter()
    clears = collections.Counter()
    vals = collections.defaultdict(set)
    last_read = {}

    for p in paths:
        ops = load_accesses(p)
        i = 0
        while i < len(ops):
            kind, addr, op, val = ops[i]
            key = (kind, addr)
            if op != "rd":
                last_read.pop(key, None)
                i += 1
                continue

            reads[key] += 1
            if val is not None:
                vals[key].add(val)

            # read-to-clear: the previous read of this address was non-zero,
            # this one is zero, and nothing was written in between
            prev = last_read.get(key)
            if prev is not None and prev and val == 0:
                clears[key] += 1
            last_read[key] = val

            # consecutive run of reads of the same address
            j = i
            while (j + 1 < len(ops) and ops[j + 1][0] == kind
                   and ops[j + 1][1] == addr and ops[j + 1][2] == "rd"):
                j += 1
            n = j - i + 1
            if n > runs[key]:
                runs[key] = n
            if n >= 4:
                run_tail[key].add(ops[j][3])
                for k in range(i, j + 1):
                    run_spread[key].add(ops[k][3])

            if j + 1 < len(ops) and ops[j + 1][:2] == (kind, addr) \
                    and ops[j + 1][2] == "wr":
                rmw[key] += 1
            i = j + 1

    rows = []
    for key in reads:
        if restrict and key not in restrict:
            continue
        n = reads[key]
        run = runs[key]
        spread = len(run_spread.get(key, ()))
        tails = run_tail.get(key, set())
        nvals = len(vals[key])

        if run >= 4 and spread > 2:
            verdict = "sample loop" if len(tails) > 1 else "poll"
        elif rmw[key] >= max(1, n // 4):
            verdict = "read-modify-write"
        elif clears[key]:
            verdict = "read-to-clear?"
        elif nvals <= 1:
            verdict = "probe, value never varies"
        else:
            verdict = "UNEXPLAINED"
        rows.append((verdict, key, n, run, nvals, rmw[key], clears[key]))
    return rows


ORDER = ["sample loop", "poll", "read-modify-write", "read-to-clear?",
         "probe, value never varies", "UNEXPLAINED"]


def mode_intent(args):
    restrict = set()
    if args.discarded:
        for line in open(args.discarded):
            f = line.split()
            if len(f) >= 4 and f[3] == "DISCARDED":
                restrict.add((f[0], int(f[1], 16)))

    paths = captures_in(args.dir)
    rows = classify(paths, restrict)
    rows.sort(key=lambda r: (ORDER.index(r[0]), -r[2]))

    tot = collections.Counter(r[0] for r in rows)
    print(f"captures: {len(paths)}   addresses examined: {len(rows)}")
    for k in ORDER:
        if tot[k]:
            print(f"  {k:28s} {tot[k]}")
    print()
    print(f"{'verdict':28s}{'reg':11s}{'reads':>7s}{'run':>6s}"
          f"{'values':>8s}{'rmw':>6s}{'clr':>5s}")
    print("-" * 72)
    for verdict, (kind, addr), n, run, nvals, nrmw, nclr in rows:
        print(f"{verdict:28s}{kind:5s}{addr:#06x}{n:7d}{run:6d}"
              f"{nvals:8d}{nrmw:6d}{nclr:5d}")
    return 0


# ---------------------------------------------------------------------------
# plan
# ---------------------------------------------------------------------------

VALS_PER_LINE = 12


def read_values(path, addr):
    """Fold the RETVALs, then pick the values read at `addr`, in order."""
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "trace_filter.py")
    with tempfile.NamedTemporaryFile(suffix=".merged.txt") as tmp:
        subprocess.run([sys.executable, tool, "--retvals", path, tmp.name],
                       check=True, stdout=subprocess.DEVNULL)
        rx = re.compile(r"PHY\.RD\s+addr=0x0*%x\s+val=0x([0-9a-fA-F]+)\b"
                        % addr)
        out = []
        for line in open(tmp.name, errors="replace"):
            m = rx.search(line)
            if m:
                out.append(int(m.group(1), 16) & 0xffff)
    return out


def emit_plan(addr, entries):
    guard = "B43_TEST_READPLAN_%04X_H" % addr
    print("/*")
    print(" * Read plan for PHY 0x%04x, generated by "
          "reverse-tools/reads.py plan." % addr)
    print(" * Do not edit by hand: regenerate from the captures (see")
    print(" * test/README.md).")
    print(" *")
    print(" * The values are the ones the vendor tracer recorded, in the order")
    print(" * the stock driver read them. Flows that run without")
    print(" * AC_READ_ORACLE need them: the mirror of the writes would answer")
    print(" * with the start bit still high and the driver's poll would spin")
    print(" * to its timeout.")
    print(" */")
    print("#ifndef %s" % guard)
    print("#define %s" % guard)
    for name, path, vals in entries:
        print()
        print("/* %d reads -- %s */" % (len(vals), path))
        print("static const u16 readplan_%04x_%s[] = {" % (addr, name))
        for i in range(0, len(vals), VALS_PER_LINE):
            chunk = vals[i:i + VALS_PER_LINE]
            print("\t" + " ".join("0x%04x," % v for v in chunk))
        print("};")
    print()
    print("#endif /* %s */" % guard)


def mode_plan(args):
    addr = int(args.addr, 0)
    entries = []
    for spec in args.captures:
        if "=" not in spec:
            sys.exit("expected NAME=CAPTURE, got %r" % spec)
        name, path = spec.split("=", 1)
        vals = read_values(path, addr)
        if not vals:
            sys.exit("%s: no read of 0x%04x carries a value. Was the capture "
                     "recorded with \"capture ret val\"?" % (path, addr))
        entries.append((name, path, vals))
    emit_plan(addr, entries)
    return 0


# ---------------------------------------------------------------------------
# consumers -- which write consumes each read's value
# ---------------------------------------------------------------------------

CENSUS_OP = re.compile(r'#(\d+)\s+cpu\d\s+(\S+)\s+(.*)$')
CENSUS_KV = re.compile(r'(\w+)=0x([0-9a-f]+)')

# Values that match anything by chance, so they cannot be traced to a consumer.
TRIVIAL = {0x0000, 0xffff, 0x0001}


def _census_parse(path):
    ops = []
    for line in open(path):
        m = CENSUS_OP.search(line)
        if not m:
            continue
        kv = {k: int(v, 16) for k, v in CENSUS_KV.findall(m.group(3))}
        ops.append((int(m.group(1)), m.group(2), kv))
    # fold table accesses: TBL.RD/TBL.WR marker + data-port ops -> one op per cell
    out = []
    i = 0
    while i < len(ops):
        ep, op, kv = ops[i]
        if op in ('TBL.RD', 'TBL.WR'):
            n = kv.get('len', 1)
            vals = []
            j = i + 1
            while j < len(ops) and len(vals) < n:
                _, op2, kv2 = ops[j]
                if op2 in ('TBL.RD', 'TBL.WR'):
                    break
                if op2 in ('PHY.RD', 'PHY.WR') and kv2.get('addr') in (0xf, 0x10, 0x11):
                    vals.append(kv2['val'])
                j += 1
            if 'val' in kv and not vals:
                vals = [kv['val']]
            for k, v in enumerate(vals):
                out.append((ep, op, ('TBL', (kv['id'], kv['off'] + k)), v))
            i = j if j > i + 1 else i + 1
            # drop the raw data-port ops we consumed
            continue
        if op in ('PHY.RD', 'PHY.WR', 'RAD.RD', 'RAD.WR', 'OBJ.RD', 'OBJ.WR'):
            if kv.get('addr') in (0xd, 0xe, 0xf, 0x10, 0x11) and op.startswith('PHY'):
                i += 1
                continue
            cls = op.split('.')[0]
            out.append((ep, op, (cls, kv.get('addr')), kv.get('val')))
        elif op in ('PHY.MOD', 'RAD.MOD'):
            cls = op.split('.')[0]
            out.append((ep, op, (cls, kv.get('addr')), (kv.get('val'), kv.get('mask'))))
        i += 1
    return out


def _census(ops, window):
    res = collections.defaultdict(lambda: dict(n=0, copy=collections.Counter(), rmw=collections.Counter(), poll=0,
                                   unused=0, compare=0, examples=[]))
    for idx, (ep, op, key, val) in enumerate(ops):
        if not op.endswith('.RD'):
            continue
        r = res[key]
        r['n'] += 1
        found = None
        # is the value a candidate for tracing? trivial values match anything
        traceable = val not in TRIVIAL
        for j in range(idx + 1, min(idx + 1 + window, len(ops))):
            ep2, op2, key2, val2 = ops[j]
            if op2.endswith('.RD'):
                if key2 == key and found is None and j - idx <= 3:
                    found = ('poll', None)
                    break
                continue
            if op2.endswith('.WR'):
                if key2 == key:
                    if val2 == val:
                        found = ('rmw', ('=', 0))
                    else:
                        # additive relation first (gain steps), else bit flip
                        d = (val2 - val) & 0xffff
                        found = ('rmw', ('+', d) if d < 0x8000 and d & (d - 1) else
                                 ('xor', val ^ val2))
                    break
                if traceable and val2 == val:
                    found = ('copy', key2)
                    break
                if traceable and isinstance(val, int) and val2 in ((val + 1) & 0xffff, (val - 1) & 0xffff):
                    found = ('copy', key2 + ('+-1',))
                    break
            if op2.endswith('.MOD') and key2 == key:
                found = ('rmw', ('mod', val2[1]))
                break
        if found is None:
            r['unused'] += 1
        elif found[0] == 'poll':
            r['poll'] += 1
        elif found[0] == 'copy':
            r['copy'][found[1]] += 1
        else:
            r['rmw'][found[1]] += 1
        if len(r['examples']) < 2:
            r['examples'].append((ep, val, found))
    return res


def _census_key(key):
    if key[0] == 'TBL':
        return f"TBL 0x{key[1][0]:04x}[0x{key[1][1]:04x}]"
    return f"{key[0]} 0x{key[1]:04x}"


def mode_consumers(args):
    ops = _census_parse(args.trace)
    res = _census(ops, args.window)
    for key in sorted(res, key=lambda k: (k[0], k[1] if isinstance(k[1], int)
                                          else k[1][0] * 0x10000 + k[1][1])):
        r = res[key]
        kinds = []
        if r['copy']:
            kinds.append('copy->' + ','.join(
                f"{_census_key(k[:2])}{'~' if len(k) > 2 else ''}x{c}"
                for k, c in r['copy'].most_common(3)))
        if r['rmw']:
            kinds.append('rmw' + ','.join(
                f"({m[0]}{'' if m[0] == '=' else '0x%04x' % m[1]})x{c}"
                for m, c in r['rmw'].most_common(4)))
        if r['poll']:
            kinds.append(f"poll x{r['poll']}")
        if r['unused']:
            kinds.append(f"unused x{r['unused']}")
        dominant = ('copy' if r['copy'] else 'rmw' if r['rmw'] else
                    'unused' if r['unused'] else 'poll')
        if args.only != 'all' and dominant != args.only:
            continue
        ex = ' '.join(f"#{e} 0x{v:04x}" for e, v, _ in r['examples']
                      if isinstance(v, int))
        print(f"{_census_key(key):22s} n={r['n']:3d}  "
              f"{' | '.join(kinds):60s} e.g. {ex}")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("risk", help="which discarded reads actually vary")
    p.add_argument("dir", nargs="?", default=".")
    p.set_defaults(fn=mode_risk)

    p = sub.add_parser("intent", help="what the stock driver does with a read")
    p.add_argument("dir", nargs="?", default=".")
    p.add_argument("discarded", nargs="?",
                   help="output of test/consumed_reads.sh, to restrict the "
                        "report to the addresses the port discards")
    p.set_defaults(fn=mode_intent)

    p = sub.add_parser("consumers",
                       help="which write consumes each read's value")
    p.add_argument("trace", metavar="FOLDED.txt")
    p.add_argument("--window", type=int, default=600,
                   help="how many ops ahead to look for the consumer "
                        "(default 600)")
    p.add_argument("--only", choices=("copy", "rmw", "unused", "poll", "all"),
                   default="all",
                   help="keep only the addresses whose dominant verdict is this")
    p.set_defaults(fn=mode_consumers)

    p = sub.add_parser("plan", help="emit a C read plan for one register")
    p.add_argument("addr")
    p.add_argument("captures", nargs="+", metavar="NAME=CAPTURE")
    p.set_defaults(fn=mode_plan)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
