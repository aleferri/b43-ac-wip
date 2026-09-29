#!/usr/bin/env python3
"""What the stock driver writes differently on 2.4 GHz than on 5 GHz.

Takes four segments of the same capture, cut at the same point of the flow
(split_trace.py --on chanspec, then ops_fold.py fold):

    A5 B5   two 5 GHz channels
    A2 B2   two 2.4 GHz channels

and aligns A5 with A2 on (class, register), table cells by (id, offset).
Aligned operations whose value differs are band candidates; those that also
differ between A5 and B5, or between A2 and B2, follow the channel and are
dropped. What is left moves with the band and nothing else in the pair.
Operations present on one band only are listed as structural differences.

With --also, a second capture's four segments are run the same way and every
line says whether that capture shows the same delta, so a band fact can be
told from board data. With --port, four traces of the unit harness on the
same channels say whether the port has it; the last line counts, over the
deltas both captures share, how many the port reproduces.

    band_delta.py A5 B5 A2 B2 [--also A5 B5 A2 B2] [--port A5 B5 A2 B2]
"""
import argparse
import difflib


def load(path):
    """(key, value, text) per op; table data words keyed by (id, offset)."""
    ops = []
    table = None
    for line in open(path):
        f = line.split()
        if f and f[0].startswith("cpu"):     # unit harness: no ts, no #n
            f = ["0", "#0"] + f
        if len(f) < 5:
            continue
        cls = f[3]
        kv = dict(x.split("=", 1) for x in f[4:] if "=" in x)
        text = " ".join(f[3:])
        if cls.startswith("TBL."):
            table = [kv["id"], int(kv["off"], 16)]
            ops.append((f"{cls} {kv['id']}", "", text))
            continue
        addr = kv.get("addr") or kv.get("off") or ""
        if cls == "PHY.WR" and addr == "0x000f" and table:
            ops.append((f"TBL {table[0]}[{table[1]:#x}]", kv.get("val", ""), text))
            table[1] += 1
            continue
        ops.append((f"{cls} {addr}", kv.get("val", ""), text))
    return ops


def align(a, b):
    return difflib.SequenceMatcher(None, [k for k, _, _ in a], [k for k, _, _ in b],
                                   autojunk=False).get_opcodes()


def changed(a, b):
    out = {}
    for tag, i1, i2, j1, j2 in align(a, b):
        if tag == "equal":
            for x, y in zip(a[i1:i2], b[j1:j2]):
                if x[1] != y[1]:
                    out.setdefault(x[0], []).append((x[1], y[1]))
    return out


def structural(a, b):
    out = []
    for tag, i1, i2, j1, j2 in align(a, b):
        if tag != "equal":
            out.append((tag, [x[2] for x in a[i1:i2]], [y[2] for y in b[j1:j2]]))
    return out


def band_delta(a5, b5, a2, b2):
    a5, b5, a2, b2 = (load(p) for p in (a5, b5, a2, b2))
    follows_channel = set(changed(a5, b5)) | set(changed(a2, b2))
    delta = {k: v for k, v in changed(a5, a2).items() if k not in follows_channel}
    return delta, structural(a5, a2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("segments", nargs=4, metavar="SEG")
    ap.add_argument("--also", nargs=4, metavar="SEG")
    ap.add_argument("--port", nargs=4, metavar="TRACE")
    args = ap.parse_args()

    delta, struct = band_delta(*args.segments)
    other = band_delta(*args.also) if args.also else None
    port = band_delta(*args.port) if args.port else None
    shared = reproduced = 0

    print(f"{len(delta)} registers or cells change with the band")
    for key, pairs in delta.items():
        first = f"{pairs[0][0]} -> {pairs[0][1]}"
        more = f" (x{len(pairs)})" if len(pairs) > 1 else ""
        mark = ""
        if other:
            o = other[0].get(key)
            mark = ("  same" if o == pairs else
                    "  other values " + f"{o[0][0]} -> {o[0][1]}" if o else "  absent")
        if port:
            p = port[0].get(key)
            got = bool(p) and p[0] == pairs[0]
            mark += "  port " + ("yes" if got else
                                 f"{p[0][0]} -> {p[0][1]}" if p else "no")
            if other and other[0].get(key) == pairs:
                shared += 1
                reproduced += got
        print(f"  {key:24s} {first}{more}{mark}")

    if port and other:
        print(f"\nport reproduces {reproduced} of the {shared} deltas both captures share")

    print(f"\n{len(struct)} structural differences (5 GHz | 2.4 GHz)")
    for tag, a, b in struct:
        print(f"  {tag}: " + "; ".join(a[:4]) + (" ..." if len(a) > 4 else "") +
              " | " + "; ".join(b[:4]) + (" ..." if len(b) > 4 else ""))


if __name__ == "__main__":
    main()
