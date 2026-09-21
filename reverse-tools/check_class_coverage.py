#!/usr/bin/env python3
"""Which op classes does a capture actually trace?

The wl-diag tracer gained hooks over time, so captures differ in which classes
they contain at all. An op absent from a capture that never traced its class
says nothing about the driver, and treating the two cases alike has already
produced wrong conclusions -- see router-data/CLASS-COVERAGE.md.

  ./check_class_coverage.py                 audit every capture under router-data
  ./check_class_coverage.py <file>...        report the given captures
  ./check_class_coverage.py --require <f>   exit non-zero if f is incomplete
  ./check_class_coverage.py --conta A,B     per-class counts, one row per capture

The sweeps live in .zip and .tar.gz, and an archive is read as ONE capture:
counting only the loose .txt leaves out almost everything there is, which is
how the table in CLASS-COVERAGE.md came to say that classes the d6220 sweeps
carry by the thousand were absent from them.

The reference set is the classes the sweep segments carry, which is the most
recent tracer.
"""

import collections
import glob
import os
import re
import sys
import tarfile
import zipfile

# La classe e' la parte prima del punto. OBJ.BULKR/W contano anche come
# "OBJ.BULK", perche' l'accessor bulk e' un hook a se' e la sua assenza
# non si deve confondere con quella della coppia a 16 bit.
OP = re.compile(r"cpu\d+\s+([A-Z]+)\.([A-Z]+)")

# Classes a complete capture has, for the purpose of judging it as an oracle
# for the PHY. PHY/RAD/TBL/MAC/SI/PMU/GPIO are in every capture ever taken;
# these three came later and their absence has already produced wrong
# conclusions.
LATE = ("OBJ", "TPL", "CAL")

# Classes the current tracer emits that no capture in the repo has yet: the
# address match path and the bulk object-memory accessors. They are NOT in LATE
# on purpose -- requiring them would fail every existing capture and block the
# PHY gates, and they are not needed to compare the PHY. They are reported so
# that an absence is read as "this capture predates the hook" and not as "the
# driver does not do it", which is the mistake CLASS-COVERAGE.md is about.
NUOVE = ("AMT", "RCMTA", "ADDRM", "OBJ.BULK")

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "router-data")


def conta(righe, c):
    for line in righe:
        if isinstance(line, bytes):
            line = line.decode("utf-8", "replace")
        m = OP.search(line)
        if m:
            c[m.group(1)] += 1
            c[m.group(1) + "." + m.group(2)] += 1
            if m.group(1) == "OBJ" and m.group(2).startswith("BULK"):
                c["OBJ.BULK"] += 1
    return c


def classes(path):
    c = collections.Counter()
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                if n.endswith(".txt"):
                    with z.open(n) as fh:
                        conta(fh, c)
    elif path.endswith((".tar.gz", ".tgz")):
        with tarfile.open(path) as t:
            for m in t.getmembers():
                if m.isfile() and m.name.endswith(".txt"):
                    conta(t.extractfile(m), c)
    else:
        with open(path, errors="replace") as fh:
            conta(fh, c)
    return c


def report(paths):
    rows = []
    for p in paths:
        c = classes(p)
        total = sum(v for k, v in c.items() if "." not in k)
        if total < 500:
            continue
        missing = [k for k in LATE if not c[k]]
        nuove = [k for k in NUOVE if c[k]]
        rows.append((p, total, missing, nuove))

    rows.sort(key=lambda r: (len(r[2]), r[0]))
    for p, total, missing, nuove in rows:
        name = os.path.relpath(p, ROOT) if p.startswith(ROOT) else p
        tag = "complete" if not missing else "missing " + " ".join(missing)
        if nuove:
            tag += "  +" + " ".join(nuove)
        print(f"  {name:58s} {total:7d}  {tag}")
    return rows


def conteggio(paths, classi):
    """Conteggio per classe, una riga per cattura: e' la tabella di
    CLASS-COVERAGE.md, e va rigenerata da qui invece che aggiornata a mano."""
    print("  %-46s %9s %s" % ("cattura", "op", "".join(
        f"{k:>12s}" for k in classi)))
    for p in paths:
        c = classes(p)
        total = sum(v for k, v in c.items() if "." not in k)
        if total < 500:
            continue
        name = os.path.relpath(p, ROOT) if p.startswith(ROOT) else p
        print("  %-46s %9d %s" % (name, total, "".join(
            f"{c[k]:12d}" for k in classi)))


def main():
    args = sys.argv[1:]
    if args and args[0] == "--require":
        for p in args[1:]:
            c = classes(p)
            missing = [k for k in LATE if not c[k]]
            if missing:
                print(f"{p}: does not trace {' '.join(missing)}; conclusions "
                      f"about those classes are not supported by it "
                      f"(router-data/CLASS-COVERAGE.md)", file=sys.stderr)
                return 1
        return 0

    classi = []
    if args and args[0] == "--conta":
        classi = args[1].split(",")
        args = args[2:]
    paths = args or sorted(
        p for pat in ("*.txt", "*.zip", "*.tar.gz")
        for p in glob.glob(os.path.join(ROOT, "*", pat)))
    if classi:
        conteggio(paths, classi)
        return 0
    rows = report(paths)
    incomplete = [r for r in rows if r[2]]
    print()
    print(f"  {len(rows) - len(incomplete)} complete, {len(incomplete)} "
          f"incomplete of {len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
