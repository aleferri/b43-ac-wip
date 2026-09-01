#!/usr/bin/env python3
"""Which op classes does a capture actually trace?

The wl-diag tracer gained hooks over time, so captures differ in which classes
they contain at all. An op absent from a capture that never traced its class
says nothing about the driver, and treating the two cases alike has already
produced wrong conclusions -- see router-data/CLASS-COVERAGE.md.

  ./check_class_coverage.py                 audit every capture under router-data
  ./check_class_coverage.py <file>...        report the given captures
  ./check_class_coverage.py --require <f>   exit non-zero if f is incomplete

The reference set is the classes the sweep segments carry, which is the most
recent tracer.
"""

import collections
import glob
import os
import re
import sys

OP = re.compile(r"cpu\d+\s+([A-Z]+)\.[A-Z]+")

# Classes a complete capture has. PHY/RAD/TBL/MAC/SI/PMU/GPIO are in every
# capture ever taken; these three are the ones that came later.
LATE = ("OBJ", "TPL", "CAL")

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "router-data")


def classes(path):
    c = collections.Counter()
    with open(path, errors="replace") as fh:
        for line in fh:
            m = OP.search(line)
            if m:
                c[m.group(1)] += 1
    return c


def report(paths):
    rows = []
    for p in paths:
        c = classes(p)
        total = sum(c.values())
        if total < 500:
            continue
        missing = [k for k in LATE if not c[k]]
        rows.append((p, total, missing))

    rows.sort(key=lambda r: (len(r[2]), r[0]))
    for p, total, missing in rows:
        name = os.path.relpath(p, ROOT) if p.startswith(ROOT) else p
        tag = "complete" if not missing else "missing " + " ".join(missing)
        print(f"  {name:58s} {total:7d}  {tag}")
    return rows


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

    paths = args or sorted(glob.glob(os.path.join(ROOT, "*", "*.txt")))
    rows = report(paths)
    incomplete = [r for r in rows if r[2]]
    print()
    print(f"  {len(rows) - len(incomplete)} complete, {len(incomplete)} "
          f"incomplete of {len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
