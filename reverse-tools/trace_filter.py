#!/usr/bin/env python3
"""Strip the instrumentation artifacts from a wl-diag trace.

Three filters, each removing one class of thing the tracer records that the
driver did not do. They compose, and the order they are applied in is fixed
because it is the only one that works: values first, then the ops that carry
them.

  --retvals     Fold the RETVAL and ARGX lines into their antecedent op.
                The "capture ret val" format logs a read as `val=UNDEFINED`
                and then, on one or more *later* lines, the value actually
                returned and any extra call arguments, tied to the source op
                by `for=#N`:

                    #16 SI.COREREG core=0x0 off=0x660 val=UNDEFINED
                    #17 ARGX     for=#16 a5=0x00000002 a6=0x00000000
                    #18 RETVAL   for=#16 val=0x00000002

                A read gets UNDEFINED replaced; an op that already carries a
                written val= keeps it and gains ` ret=<v>` (write-then-
                readback, e.g. PMU.PLL); ARGX operands are appended as
                ` a5=.. a6=..`. Without this every read has val=UNDEFINED and
                the value is not comparable -- forgetting it is the easiest
                mistake to make, because a grep for `val=0x...` on an unfolded
                file finds no reads at all and the op looks absent.

  --mod-reads   Drop the RMW read of each MOD. The tracer logs a
                read-modify-write as two ops: the `PHY.RD`/`RAD.RD` that
                `phy_reg_mod` does internally to read the current value, then
                the `PHY.MOD`/`RAD.MOD` that writes it back. That RD is pure
                instrumentation: it has no effect, and the ported driver may
                or may not emit it depending on how it is written (explicit
                peek vs mask helper). In a positional comparison it is a false
                mismatch. Only a `<fam>.RD addr=A` immediately followed by a
                `<fam>.MOD addr=A` goes: standalone peeks stay.

  --collapse    Drop the raw ops that implement each TBL.WR/TBL.RD -- the PHY
                table ports 0x00d (id), 0x00e (offset), 0x00f/0x010/0x011
                (data) and the 0x019e write gate -- leaving the TBL header
                alone. The count and order of those mechanism ops depend on
                HOW the port loads a table, not on the effect, so a positional
                comparison trips over them. Removing them from BOTH sides
                leaves the skeleton of the real register writes.

                Unlike --mod-reads this DOES hide the table mechanism and its
                values, so a real divergence (a table write the vendor does
                not do, a board-specific value) disappears with it. For a
                comparison, prefer --mod-reads alone and keep --collapse for
                the macro-order tools that need the skeleton.

Both trace formats are accepted -- the vendor capture
(`<time> #<ep> cpu<n> OP ...`) and the harness's bare output
(`cpu<n> OP ...`) -- and the `#ep` is preserved, because the comparison window
is expressed in episodes.

Usage:
  trace_filter.py --retvals IN [OUT]        (OUT omitted -> stdout)
  trace_filter.py --mod-reads --collapse IN OUT
"""
import argparse
import re
import sys

VAL_RE = re.compile(r"val=(0x[0-9a-fA-F]+|UNDEFINED)")
A5_RE = re.compile(r"\ba5=(0x[0-9a-fA-F]+)")
A6_RE = re.compile(r"\ba6=(0x[0-9a-fA-F]+)")
FOR_RE = re.compile(r"\bfor=#(\d+)\b")

OP_LINE = re.compile(r"^(\s*(?:[\d.]+\s+#\d+\s+)?cpu\d+\s+)(\S+)\s+(.*)$")

# The PHY table ports and the write gate: the mechanism of a table access.
TABLE_PORTS = {0x00d, 0x00e, 0x00f, 0x010, 0x011, 0x19e}


def ep_of(line):
    """Episode number from the `#NNN` token, or None."""
    parts = line.split()
    if len(parts) > 1 and parts[1].startswith("#"):
        return parts[1][1:]
    return None


def parse_op(line):
    """(family, kind, addr) for an op line, or None if it is not one."""
    m = OP_LINE.match(line.rstrip("\n"))
    if not m:
        return None
    op = m.group(2)
    if "." not in op:
        return (op, "", None)
    fam, kind = op.split(".", 1)
    kv = dict(re.findall(r"(\w+)=(\S+)", m.group(3)))
    addr = None
    if "addr" in kv:
        try:
            addr = int(kv["addr"], 16)
        except ValueError:
            addr = None
    return (fam, kind, addr)


def fold_retvals(lines):
    """RETVAL/ARGX into their antecedent op. Returns (lines, folded, args).

    The ops are indexed first and the attachments applied after, so a RETVAL
    that precedes its op works as well as one that follows it.
    """
    aux = [False] * len(lines)
    ret, args = {}, {}
    for i, ln in enumerate(lines):
        parts = ln.split()
        op = parts[3] if len(parts) > 3 else ""
        if op not in ("RETVAL", "ARGX"):
            continue
        aux[i] = True
        m = FOR_RE.search(ln)
        if not m:
            continue
        tgt = m.group(1)
        if op == "RETVAL":
            v = VAL_RE.search(ln)
            if v:
                ret[tgt] = v.group(1)
        else:
            a5, a6 = A5_RE.search(ln), A6_RE.search(ln)
            args[tgt] = (a5.group(1) if a5 else None,
                         a6.group(1) if a6 else None)

    n_ret = n_arg = 0
    out = []
    for i, ln in enumerate(lines):
        if aux[i]:
            continue
        ep = ep_of(ln)
        if ep is not None and ep in ret and ret[ep] != "UNDEFINED":
            if "val=UNDEFINED" in ln:
                ln = ln.replace("val=UNDEFINED", "val=" + ret[ep])
            else:
                ln = ln + " ret=" + ret[ep]
            n_ret += 1
        if ep is not None and ep in args:
            a5, a6 = args[ep]
            if a5:
                ln += " a5=" + a5
            if a6:
                ln += " a6=" + a6
            n_arg += 1
        out.append(ln)
    return out, n_ret, n_arg


def fold_mod_reads(lines):
    """Drop each MOD's internal RMW read. Returns (lines, dropped)."""
    kept, dropped, i = [], 0, 0
    while i < len(lines):
        cur = parse_op(lines[i])
        nxt = parse_op(lines[i + 1]) if i + 1 < len(lines) else None
        if (cur and nxt and cur[1] == "RD" and nxt[1] == "MOD"
                and cur[0] == nxt[0] and cur[2] is not None
                and cur[2] == nxt[2]):
            dropped += 1
            i += 1
            continue
        kept.append(lines[i])
        i += 1
    return kept, dropped


def collapse_tables(lines):
    """Drop the table-port ops. Returns (lines, removed, total).

    Lines that are not ops are dropped as well: what is left is meant to be a
    comparable skeleton, and a header in the middle of it is not one.
    """
    kept, removed, total = [], 0, 0
    for ln in lines:
        m = OP_LINE.match(ln)
        if not m:
            continue
        total += 1
        kv = dict(re.findall(r"(\w+)=(\S+)", m.group(3)))
        if m.group(2).startswith("PHY.") and "addr" in kv:
            try:
                a = int(kv["addr"], 16)
            except ValueError:
                a = None
            if a in TABLE_PORTS:
                removed += 1
                continue
        kept.append(ln)
    return kept, removed, total


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--retvals", action="store_true",
                    help="fold the RETVAL/ARGX lines into their op")
    ap.add_argument("--mod-reads", action="store_true",
                    help="drop the RMW read of each MOD")
    ap.add_argument("--collapse", action="store_true",
                    help="drop the table-port ops, keep the TBL header")
    ap.add_argument("infile")
    ap.add_argument("outfile", nargs="?")
    a = ap.parse_args()

    if not (a.retvals or a.mod_reads or a.collapse):
        ap.error("pick at least one filter: --retvals, --mod-reads, --collapse")

    lines = open(a.infile, encoding="utf-8",
                 errors="replace").read().splitlines()
    notes = []

    if a.retvals:
        lines, n_ret, n_arg = fold_retvals(lines)
        notes.append(f"{n_ret} retval, {n_arg} argx merged")
    if a.mod_reads:
        lines, dropped = fold_mod_reads(lines)
        notes.append(f"{dropped} RMW reads folded")
    if a.collapse:
        lines, removed, total = collapse_tables(lines)
        notes.append(f"{removed}/{total} raw table ops removed")

    text = "\n".join(lines) + "\n"
    if a.outfile:
        open(a.outfile, "w", encoding="utf-8").write(text)
        sys.stderr.write(f"{a.outfile}: {len(lines)} ops, "
                         f"{', '.join(notes)}\n")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
