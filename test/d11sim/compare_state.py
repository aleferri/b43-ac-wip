#!/usr/bin/env python3
"""Compare two d11sim state dumps and report whether the final core state and
RAM match, by cell, without any op-by-op alignment.

The verdict is taken over the *host-configuration surface*: the register file,
shared memory, scratch, internal-hw and RCMTA, minus cells that neither host
owns. Two kinds of cell are bucketed out of the verdict and reported on their
own, because they do not reflect what the driver's init did:

  - the ucode revinfo words (shared memory 0x0000..0x0007), written by the
    running ucode, not the host;
  - the UCODE image, which is the microcode build and differs between wl
    releases even on the same corerev.

A couple of register cells are volatile rather than configuration (the IRQ
reason latch the handshake sets, the self-clearing MAC command) and are
reported separately too.

Usage:  compare_state.py A.dump B.dump [--label-a NAME --label-b NAME]
                         [--show N] [--include-ucode]
Exit status is 0 when the host-configuration surface is identical.
"""
import argparse
import sys

REVINFO_SHM = {0x0000, 0x0002, 0x0004, 0x0006}
VOLATILE_REG = {0x0128, 0x0124}  # GEN_IRQ_REASON, MACCMD


def load(path):
    cells = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:          # a 4th provenance column is allowed
                continue
            region, key, val = parts[0], parts[1], parts[2]
            cells[(region, int(key, 16))] = int(val, 16)
    return cells


def bucket(region, key, ucode_keys):
    if region == "UCODE":
        return "ucode_image"
    if (region, key) in ucode_keys:
        return "ucode_revinfo"
    if region == "REG" and key in VOLATILE_REG:
        return "volatile_reg"
    return "host"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--show", type=int, default=25,
                    help="max differing cells to list per category")
    ap.add_argument("--include-ucode", action="store_true",
                    help="fold the ucode image into the verdict (off by default)")
    ap.add_argument("--ucode-a", help="interpreter ucode-init dump to overlay on A")
    ap.add_argument("--ucode-b", help="interpreter ucode-init dump to overlay on B")
    args = ap.parse_args()

    A, B = load(args.a), load(args.b)

    # Overlay the real ucode-init output onto each side and mark those cells as
    # ucode-authored by provenance. Without a dump, fall back to the documented
    # revinfo offsets so the tool still classifies them out of the host verdict.
    ucode_keys = set()
    for path, side in ((args.ucode_a, A), (args.ucode_b, B)):
        if not path:
            continue
        for k, v in load(path).items():
            side[k] = v
            ucode_keys.add(k)
    if not ucode_keys:
        ucode_keys = {("SHM", off) for off in REVINFO_SHM}

    keys = sorted(set(A) | set(B))

    cat = {}  # bucket -> dict of lists
    for k in keys:
        bk = bucket(k[0], k[1], ucode_keys)
        if bk == "ucode_image" and args.include_ucode:
            bk = "host"
        d = cat.setdefault(bk, {"equal": [], "differ": [], "only_a": [], "only_b": []})
        va, vb = A.get(k), B.get(k)
        if va is not None and vb is not None:
            (d["equal"] if va == vb else d["differ"]).append((k, va, vb))
        elif va is not None:
            d["only_a"].append((k, va, None))
        else:
            d["only_b"].append((k, None, vb))

    la, lb = args.label_a, args.label_b
    print(f"A = {la}: {args.a}")
    print(f"B = {lb}: {args.b}")
    print()

    def line(k, va, vb):
        region, key = k
        sa = "--------" if va is None else f"{va:08x}"
        sb = "--------" if vb is None else f"{vb:08x}"
        return f"    {region:5s} 0x{key:04x}  {la}={sa}  {lb}={sb}"

    order = ["host", "volatile_reg", "ucode_revinfo", "ucode_image"]
    titles = {
        "host": "HOST CONFIGURATION SURFACE  (REG/SHM/SCR/HW/RCMTA)",
        "volatile_reg": "volatile registers (IRQ latch, MAC command) [informational]",
        "ucode_revinfo": "shared memory written by the ucode [real execution]",
        "ucode_image": "ucode image [build provenance, not host init]",
    }

    host_ok = True
    for bk in order:
        if bk not in cat:
            continue
        d = cat[bk]
        eq, diff = len(d["equal"]), len(d["differ"])
        oa, ob = len(d["only_a"]), len(d["only_b"])
        total = eq + diff + oa + ob
        pct = (100.0 * eq / total) if total else 100.0
        print(f"== {titles[bk]} ==")
        print(f"   cells touched by either: {total}")
        print(f"   equal={eq}  differ={diff}  only-{la}={oa}  only-{lb}={ob}  "
              f"({pct:.2f}% equal)")

        if bk == "host" and (diff or oa or ob):
            host_ok = False

        for name, rows in (("differ", d["differ"]),
                           ("only_" + la, d["only_a"]),
                           ("only_" + lb, d["only_b"])):
            if not rows:
                continue
            print(f"   {name}: {len(rows)}")
            for k, va, vb in rows[:args.show]:
                print(line(k, va, vb))
            if len(rows) > args.show:
                print(f"      ... +{len(rows) - args.show} more")
        print()

    print("VERDICT: host-configuration surface is",
          "IDENTICAL" if host_ok else "DIFFERENT",
          f"between {la} and {lb}")
    return 0 if host_ok else 1


if __name__ == "__main__":
    sys.exit(main())
