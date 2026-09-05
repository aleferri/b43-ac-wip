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

  plan    Emit a C read plan for one register from the values a capture
          recorded. The harness serves reads from three sources in order: the
          read oracle (the whole capture), a scripted plan, and finally the
          mirror of the writes. A flow that runs without an oracle needs a
          plan for any register whose read value steers control flow --
          otherwise the mirror answers with the last value written, which for
          a self-clearing status bit never advances.

Usage:
  reads.py risk   <dir>                      <dir>/<ch>.vendor captures
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

    p = sub.add_parser("plan", help="emit a C read plan for one register")
    p.add_argument("addr")
    p.add_argument("captures", nargs="+", metavar="NAME=CAPTURE")
    p.set_defaults(fn=mode_plan)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
