#!/usr/bin/env python3
"""Second pass over a trace decoded by mmio2ops.py.

`fold` rebuilds the accessor-level ops the vendor tracer logs and the bus
does not carry, from their bus footprint:

  PHY.RD a + PHY.WR a  (adjacent)  -> PHY.MOD addr val mask      mask = rd ^ wr
  RAD.RD a + RAD.WR a  (adjacent)  -> RAD.MOD + RAD.RD + RAD.WR  the vendor triple
  REG.RD 0x120 + MAC.MCTRL         -> MAC.MCTRL val mask
  PHY.WR 0xd, 0xe + data on 0xf/0x10/0x11 -> TBL.WR/TBL.RD id off len, before
                                      the 0xd write, data kept as the vendor does

The mask of a rebuilt MOD is a lower bound: a bit the driver masked but did
not change leaves no trace on the bus, so mask=0x0 means "same value written
back", not "no mask".

`report` lists what mmiotrace cannot record and what can be inferred about
it: window moves, polling loops, interrupt reasons, DMA ring activity per
chanspec segment, and the pauses the driver takes.
"""
import argparse
import collections
import re
import sys

RE = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(\S+)\s*(.*?)\s*$")
KV = re.compile(r"(\w+)=(\S+)")

TBL_ID, TBL_OFF = 0xd, 0xe
TBL_DATA = {0xf, 0x10, 0x11}

Op = collections.namedtuple("Op", "ts cls kv raw")


def parse(path):
    for line in open(path):
        m = RE.match(line)
        if not m:
            continue
        kv = {k: v for k, v in KV.findall(m.group(4))}
        yield Op(float(m.group(1)), m.group(3), kv, m.group(4))


def hx(kv, k):
    return int(kv[k], 16) if k in kv else None


# -- fold ---------------------------------------------------------------------

def fold(ops):
    ops = list(ops)
    out = []
    i = 0
    while i < len(ops):
        op = ops[i]
        nxt = ops[i + 1] if i + 1 < len(ops) else None
        if op.cls in ("PHY.RD", "RAD.RD") and nxt and nxt.cls == op.cls[:3] + ".WR" \
                and op.kv["addr"] == nxt.kv["addr"]:
            rd, wr = hx(op.kv, "val"), hx(nxt.kv, "val")
            mask = rd ^ wr
            space = op.cls[:3]
            out.append((op.ts, f"{space}.MOD addr={op.kv['addr']} val=0x{wr & mask:04x} mask=0x{mask:04x}"))
            if space == "RAD":
                out.append((op.ts, op.raw_line()))
                out.append((nxt.ts, nxt.raw_line()))
            i += 2
            continue
        if op.cls == "REG.RD" and op.kv.get("off") == "0x0120" and nxt and nxt.cls == "MAC.MCTRL":
            rd, wr = hx(op.kv, "val"), hx(nxt.kv, "val")
            mask = rd ^ wr
            out.append((nxt.ts, f"MAC.MCTRL val=0x{wr & mask:08x} mask=0x{mask:08x}"))
            i += 2
            continue
        if op.cls == "PHY.WR" and hx(op.kv, "addr") == TBL_ID and nxt \
                and nxt.cls == "PHY.WR" and hx(nxt.kv, "addr") == TBL_OFF:
            j = i + 2
            ports = collections.Counter()
            kind = None
            while j < len(ops) and ops[j].cls in ("PHY.WR", "PHY.RD") \
                    and hx(ops[j].kv, "addr") in TBL_DATA \
                    and kind in (None, ops[j].cls):
                kind = ops[j].cls
                ports[hx(ops[j].kv, "addr")] += 1
                j += 1
            if kind:
                n = ports[0xf] if ports[0xf] else ports[0x11]
                out.append((op.ts, f"TBL.{kind[-2:]} id={op.kv['val']} off={nxt.kv['val']} len={n}"))
        out.append((op.ts, op.raw_line()))
        i += 1
    return out


def _raw_line(self):
    return f"{self.cls} {self.raw}" if self.raw else self.cls


Op.raw_line = _raw_line


# -- report -------------------------------------------------------------------

MACINT = {
    0x1: "MACSSPNDD", 0x2: "BCNTPL", 0x4: "TBTT", 0x8: "BCNSUCCESS", 0x10: "BCNCANCLD",
    0x20: "ATIMWINEND", 0x40: "PMQ", 0x80: "NSPECGEN0", 0x100: "NSPECGEN1",
    0x200: "MACTXERR", 0x400: "NSPECGEN3", 0x800: "PHYTXERR", 0x1000: "PME",
    0x2000: "GP0", 0x4000: "GP1", 0x8000: "DMAINT", 0x10000: "TXSTOP", 0x20000: "CCA",
    0x40000: "BG_NOISE", 0x80000: "DTIM_TBTT", 0x100000: "PRQ", 0x200000: "PWRUP",
    0x400000: "RESET", 0x800000: "BT_RFACT_STUCK", 0x1000000: "TTTT", 0x2000000: "P2P",
    0x4000000: "DMATX", 0x8000000: "TSSI_LIMIT", 0x10000000: "RFDISABLE",
    0x20000000: "TFS", 0x40000000: "PHYCHANGED", 0x80000000: "TO",
}
TXPTR = {0x204 + 0x40 * n: n for n in range(6)}
RXPTR = 0x224
TXSTATUS = 0x170
CHANSPEC = 0x00a0


def bits(v, names):
    return "|".join(n for b, n in names.items() if v & b) or "0"


def report(ops, out):
    ops = list(ops)
    win = [(o.ts, o.kv["core"], o.kv["from"]) for o in ops if o.cls == "WIN"]
    out.write(f"== window moves inferred: {len(win)} (decode with --mark-windows to see them)\n")
    for ts, core, frm in win:
        out.write(f"  {ts:.6f}  {frm} -> {core}\n")

    out.write("\n== polling loops (>= 3 consecutive reads of one register)\n")
    i, polls = 0, collections.Counter()
    while i < len(ops):
        o = ops[i]
        if o.cls.endswith(".RD") and o.cls not in ("OBJ.RD", "SROM.RD", "EROM.RD"):
            key = (o.cls, o.kv.get("off") or o.kv.get("addr"))
            j = i
            while j < len(ops) and (ops[j].cls, ops[j].kv.get("off") or ops[j].kv.get("addr")) == key:
                j += 1
            if j - i >= 3:
                polls[key] += 1
                if polls[key] <= 2:
                    out.write(f"  {o.ts:.6f}  {key[0]} {key[1]} x{j - i}  first={o.kv.get('val')} last={ops[j - 1].kv.get('val')}  {(ops[j - 1].ts - o.ts) * 1e3:.2f} ms\n")
            i = j
        else:
            i += 1
    for key, n in polls.most_common():
        out.write(f"  total {key[0]} {key[1]}: {n} loops\n")

    out.write("\n== macintstatus values read (REG.RD 0x0128 != 0)\n")
    irq = collections.Counter(hx(o.kv, "val") for o in ops
                              if o.cls == "REG.RD" and o.kv.get("off") == "0x0128" and hx(o.kv, "val"))
    for v, n in irq.most_common(12):
        out.write(f"  0x{v:08x} x{n:<5} {bits(v, MACINT)}\n")

    out.write("\n== DMA activity per chanspec segment (pointer writes = descriptors handed over)\n")
    seg, rows = "attach", []
    cur = collections.Counter()
    t0 = ops[0].ts if ops else 0
    for o in ops:
        if o.cls == "OBJ.WR" and hx(o.kv, "addr") == CHANSPEC and o.kv.get("sel") == "0x10000":
            rows.append((seg, t0, cur))
            seg, t0, cur = f"chanspec {o.kv['val']}", o.ts, collections.Counter()
        elif o.cls == "REG.WR" and hx(o.kv, "off") in TXPTR:
            cur[f"tx{TXPTR[hx(o.kv, 'off')]}"] += 1
        elif o.cls == "REG.WR" and hx(o.kv, "off") == RXPTR:
            cur["rxptr"] += 1
        elif o.cls == "REG.RD" and hx(o.kv, "off") == TXSTATUS:
            cur["txstatus"] += 1
    rows.append((seg, t0, cur))
    for seg, t0, cur in rows:
        out.write(f"  {t0:.3f}  {seg:<18} " + " ".join(f"{k}={v}" for k, v in sorted(cur.items())) + "\n")

    out.write("\n== pauses > 1 ms (driver sleeping or waiting on the bus)\n")
    gaps = collections.Counter()
    for a, b in zip(ops, ops[1:]):
        dt = b.ts - a.ts
        if dt > 1e-3:
            gaps[(round(dt * 1e3), a.raw_line()[:40], b.raw_line()[:40])] += 1
    for (ms, before, after), n in sorted(gaps.items(), key=lambda kv: -kv[1])[:20]:
        out.write(f"  ~{ms:>4} ms x{n:<4} after [{before}] before [{after}]\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["fold", "report"])
    ap.add_argument("trace", help="output of mmio2ops.py")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()
    out = open(args.out, "w") if args.out else sys.stdout
    ops = parse(args.trace)
    if args.mode == "report":
        report(ops, out)
        return
    for n, (ts, line) in enumerate(fold(ops), 1):
        out.write(f"{ts:15.6f} #{n:<8} cpu0 {line}\n")


if __name__ == "__main__":
    main()
