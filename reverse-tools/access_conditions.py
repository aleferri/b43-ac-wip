#!/usr/bin/env python3
"""Compare the core state in which two traces touch the PHY and radio, and the
waits they make.

A PHY or radio access is only safe in some states of the core: the stock
driver keeps the MAC suspended, the PHY clock on and out of reset, and forces
the clock where it needs to. This annotates every PHY/RAD access of two traces
(a wl bus capture and a b43 trace, both in the mmio2ops/trace_out vocabulary)
with the core state it happens in:

    reset    BCMA resetctrl bit 0                 (wrapper 0x800)
    clk      ioctrl BCMA_IOCTL_CLK                (wrapper 0x408 bit 0)
    fgc      ioctrl BCMA_IOCTL_FGC, forced clock  (bit 1)
    phyclk   ioctrl B43_BCMA_IOCTL_PHY_CLKEN      (bit 2)
    phyrst   ioctrl B43_BCMA_IOCTL_PHY_RESET      (bit 3)
    mac      MACCONTROL ENABLED                   (bit 0)
    psm      MACCONTROL PSM_RUN                   (bit 1)
    forceht  clk_ctl_st BCMA_CLKCTLST_FORCEHT     (0x1e0 bit 1)

and lists, per state, how many accesses each trace makes. A state in which
b43 touches the PHY and wl never does is the first thing to look at.

It also lists the waits: runs of two or more consecutive reads of the same
register, with the write that precedes the run, and whether the other trace
waits on that register at all.

    access_conditions.py WL B43 [--wl-from PAT] [--wl-to PAT]
                                [--b43-from PAT] [--b43-to PAT]

The windows are given by the first line containing PAT (default: the whole
file); --*-to excludes the line it matches. The core state is followed from
the top of the file, so a window that starts late still knows it.
"""

import argparse
import collections
import re

OP = re.compile(r"\b([A-Z]+\.[A-Z.]+)\s+(.*)$")
FIELD = re.compile(r"(\w+)=0x([0-9a-fA-F]+)")

WRAP_IOCTL, WRAP_RESETCTRL, REG_CLKCTLST = 0x408, 0x800, 0x1E0
PHY_PORTS = {0x3FC, 0x3FE, 0x3FA, 0x3F6, 0x3F8, 0x3E8, 0x3EA}  # address/data
FLAGS = ("reset", "clk", "fgc", "phyclk", "phyrst", "mac", "psm", "forceht")


class State:
    def __init__(self):
        self.ioctl = None
        self.resetctrl = None
        self.macctl = None
        self.clkctlst = None

    def key(self):
        def bit(v, b):
            return "?" if v is None else int(bool(v & b))
        return (bit(self.resetctrl, 1), bit(self.ioctl, 1), bit(self.ioctl, 2),
                bit(self.ioctl, 4), bit(self.ioctl, 8), bit(self.macctl, 1),
                bit(self.macctl, 2), bit(self.clkctlst, 2))

    def update(self, cls, f):
        if cls == "WRAP.WR" and "off" in f:
            if f["off"] == WRAP_IOCTL:
                self.ioctl = f["val"]
            elif f["off"] == WRAP_RESETCTRL:
                self.resetctrl = f["val"]
        elif cls == "MAC.MCTRL":
            self.macctl = f["val"]
        elif cls == "REG.WR" and f.get("off") == REG_CLKCTLST:
            self.clkctlst = f["val"]


def window(path, start, stop):
    lines = open(path).read().splitlines()
    a = next((i for i, l in enumerate(lines) if start and start in l), 0)
    b = next((i for i, l in enumerate(lines) if stop and stop in l and i > a), len(lines))
    return lines, a, b


def analyse(lines, a, b):
    st = State()
    access = collections.Counter()
    examples = {}
    waits = collections.Counter()
    wait_ctx = {}
    last_write = None
    run = None                      # (cls, off, count, first, last)

    def close():
        if run and run[2] >= 2:
            k = (run[0], run[1])
            waits[k] += 1
            wait_ctx.setdefault(k, (run[3], run[4], run[2], last_write_at_run))

    last_write_at_run = None
    for n, line in enumerate(lines):
        m = OP.search(line)
        if not m:
            continue
        cls = m.group(1)
        f = {k: int(v, 16) for k, v in FIELD.findall(m.group(2))}
        st.update(cls, f)
        if not a <= n < b:
            continue
        if cls.startswith(("PHY.", "RAD.")):
            k = st.key()
            access[k] += 1
            examples.setdefault(k, (n, line.strip()))
        rd = cls.endswith(".RD") and "off" in f and f["off"] not in PHY_PORTS
        if rd and run and run[0] == cls and run[1] == f["off"]:
            run = (run[0], run[1], run[2] + 1, run[3], f.get("val"))
            continue
        close()
        run = (cls, f["off"], 1, f.get("val"), f.get("val")) if rd else None
        last_write_at_run = last_write
        if cls.endswith((".WR", ".MCTRL", ".MCMD")):
            last_write = line.strip()
    close()
    return access, examples, waits, wait_ctx


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wl")
    ap.add_argument("b43")
    ap.add_argument("--wl-from")
    ap.add_argument("--wl-to")
    ap.add_argument("--b43-from")
    ap.add_argument("--b43-to")
    args = ap.parse_args()

    wa, we, ww, wc = analyse(*window(args.wl, args.wl_from, args.wl_to))
    ba, be, bw, bc = analyse(*window(args.b43, args.b43_from, args.b43_to))

    print("PHY/RAD accesses per core state (" + " ".join(FLAGS) + ")")
    print(f"  {'state':24s} {'wl':>7s} {'b43':>7s}")
    for k in sorted(set(wa) | set(ba), key=str):
        mark = "   <-- b43 only" if ba.get(k) and not wa.get(k) else ""
        print(f"  {' '.join(map(str, k)):24s} {wa.get(k, 0):7d} {ba.get(k, 0):7d}{mark}")
    for k in sorted(set(ba) - set(wa), key=str):
        n, line = be[k]
        print(f"  first b43 access in {' '.join(map(str, k))}: line {n + 1}: {line}")

    print("\nwaits (two or more consecutive reads of one register)")
    print(f"  {'register':18s} {'wl':>4s} {'b43':>4s}  wl: values, length, preceded by")
    for k in sorted(set(ww) | set(bw)):
        ctx = wc.get(k) or bc.get(k)
        first, last, length, prev = ctx
        fmt = lambda v: "?" if v is None else f"{v:#x}"
        side = "" if k in ww else "  (b43 only)"
        print(f"  {k[0]:8s} {k[1]:#06x}   {ww.get(k, 0):4d} {bw.get(k, 0):4d}  "
              f"{fmt(first)}->{fmt(last)} x{length}; {prev or '-'}{side}")


if __name__ == "__main__":
    main()
