#!/usr/bin/env python3
"""Resolve the #NNNNN trace references that appear in source comments.

Every reference names an episode index in a wl-diag capture, but the index
spaces of the captures overlap, so a bare "#34529" is ambiguous by
construction. This walks the sources, collects the references, and reports
for each one which of the captures under router-data/ actually holds that
index and what op sits there.

Exit status is 1 when at least one reference resolves nowhere, which makes
this usable as a pre-submission gate.

  ./check_trace_refs.py                  summary + unresolvable references
  ./check_trace_refs.py --all            every reference with its candidates
  ./check_trace_refs.py --ref 34529      one reference across all captures
"""

import argparse
import os
import re
import sys
import zipfile
from collections import defaultdict

REF_RE = re.compile(r"#(\d{3,6})\b")
EP_RE = re.compile(r"^\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(.*?)\s*$")
COMMENT_RE = re.compile(r"^\s*(\*/|\*|/\*|//)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
DATA = os.path.join(ROOT, "router-data")


def source_refs():
    """(index, file, line, text) for every reference inside a comment."""
    out = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith((".c", ".h")):
            continue
        path = os.path.join(SRC, name)
        for lineno, line in enumerate(open(path), 1):
            body = line
            if not COMMENT_RE.match(line):
                # trailing comment on a code line
                cut = min((line.find(t) for t in ("/*", "//") if t in line),
                          default=-1)
                if cut < 0:
                    continue
                body = line[cut:]
            for m in REF_RE.finditer(body):
                out.append((int(m.group(1)), name, lineno, line.rstrip()))
    return out


def capture_paths():
    """Every wl-diag capture, including the ones inside the sweep zips."""
    plain, zipped = [], []
    for board in sorted(os.listdir(DATA)):
        bdir = os.path.join(DATA, board)
        if not os.path.isdir(bdir):
            continue
        for name in sorted(os.listdir(bdir)):
            path = os.path.join(bdir, name)
            if name.endswith(".txt") and ("wl-diag" in name or "wl1-" in name
                                          or "-wl1-" in name):
                plain.append((f"{board}/{name}", path))
            elif name.endswith(".zip"):
                zipped.append((board, path))
    return plain, zipped


def scan(stream, wanted, label, hits):
    for raw in stream:
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", "replace")
        m = EP_RE.match(raw)
        if m and int(m.group(1)) in wanted:
            hits[int(m.group(1))].append((label, " ".join(m.group(2).split())))


def resolve(wanted, include_zips=True):
    hits = defaultdict(list)
    plain, zipped = capture_paths()
    for label, path in plain:
        with open(path, errors="replace") as fh:
            scan(fh, wanted, label, hits)
    if include_zips:
        for board, zpath in zipped:
            with zipfile.ZipFile(zpath) as zf:
                for info in zf.infolist():
                    if not info.filename.endswith(".txt"):
                        continue
                    if "LEGGIMI" in info.filename or "README" in info.filename:
                        continue
                    with zf.open(info) as fh:
                        scan(fh, wanted,
                             f"{board}/{os.path.basename(zpath)}:{info.filename}",
                             hits)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="list every reference, not just the broken ones")
    ap.add_argument("--ref", type=int,
                    help="resolve a single index and exit")
    ap.add_argument("--no-zips", action="store_true",
                    help="skip the sweep archives (much faster)")
    args = ap.parse_args()

    if args.ref is not None:
        hits = resolve({args.ref}, not args.no_zips)
        for label, op in hits.get(args.ref, []):
            print(f"  {label:64s} {op}")
        if not hits.get(args.ref):
            print(f"  #{args.ref}: no capture under router-data/ holds it")
        return 0

    refs = source_refs()
    wanted = {idx for idx, *_ in refs}
    print(f"{len(refs)} references, {len(wanted)} distinct, "
          f"scanning captures...", file=sys.stderr)
    hits = resolve(wanted, not args.no_zips)

    unresolved, unique, ambiguous = [], [], []
    for idx, name, lineno, text in refs:
        cand = hits.get(idx, [])
        if not cand:
            unresolved.append((idx, name, lineno, text))
        elif len({c[1] for c in cand}) == 1:
            unique.append((idx, name, lineno, cand))
        else:
            ambiguous.append((idx, name, lineno, cand))

    print(f"resolve to one op   : {len(unique)}")
    print(f"resolve to several  : {len(ambiguous)}")
    print(f"resolve to nothing  : {len(unresolved)}")

    if unresolved:
        print("\nreferences no capture in this repo can resolve:")
        for idx, name, lineno, text in unresolved:
            print(f"  {name}:{lineno}  #{idx}")
            print(f"      {text.strip()[:96]}")

    if args.all:
        print("\nambiguous references and their candidates:")
        for idx, name, lineno, cand in ambiguous:
            print(f"  {name}:{lineno}  #{idx}")
            for label, op in cand:
                print(f"      {label:60s} {op}")

    return 1 if unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
