#!/usr/bin/env python3
"""The BCM4352/BCM4360 PMU and PCIe2 init writes in a trace.

Input: mmio2ops.py output or a b43-trace output. One line per PLL word,
PLL_UPD, resource table, resource mask and PCIe2 HT request write; repeats
in a row are merged, as the stock driver writes max_res_mask twice.

--stop gpiotimer: up to the stock driver's first gpiotimerval write.
--stop bcma: up to the harness's first access outside ChipCommon and PCIe2.
--from LABEL: from the first line starting with LABEL.
"""
import argparse
import re
import sys

OP = re.compile(r'\b(CC|PCIE)\.(RD|WR)\s+off=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)')
ANY_CLASS = re.compile(r'\s([A-Z0-9]+)\.[A-Z]+\s')

PMU_CTL_PLL_UPD = 0x400
CLKCTLST_HAVEHTREQ = 0x10
LABELS = {0x620: 'RES_TABSEL', 0x628: 'RES_UPDNTM',
          0x618: 'MINRES', 0x61c: 'MAXRES'}


def family_writes(lines, stop):
    pll_addr = None
    for line in lines:
        m = OP.search(line)
        if not m:
            c = ANY_CLASS.search(line)
            if stop == 'bcma' and c and c.group(1) not in ('CC', 'PCIE'):
                return
            continue
        core, rw, off, val = m.group(1), m.group(2), int(m.group(3), 16), \
            int(m.group(4), 16)
        if rw != 'WR':
            continue
        if core == 'PCIE':
            if off == 0x1e0 and val & CLKCTLST_HAVEHTREQ:
                yield 'HAVEHTREQ'
            continue
        if off == 0x088 and stop == 'gpiotimer':
            return
        if off == 0x660:
            pll_addr = val
        elif off == 0x664:
            yield 'PLL%d=0x%x' % (pll_addr, val)
        elif off == 0x600 and val & PMU_CTL_PLL_UPD:
            yield 'PLL_UPD'
        elif off in LABELS:
            yield '%s=0x%x' % (LABELS[off], val)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('trace')
    ap.add_argument('--stop', choices=('gpiotimer', 'bcma'), required=True)
    ap.add_argument('--from', dest='start')
    args = ap.parse_args()

    with open(args.trace, errors='replace') as f:
        ops = list(family_writes(f, args.stop))
    if args.start:
        first = [i for i, o in enumerate(ops) if o.startswith(args.start)]
        if not first:
            sys.exit('%s: no %s' % (args.trace, args.start))
        ops = ops[first[0]:]
    prev = None
    for o in ops:
        if o != prev:
            print(o)
        prev = o


if __name__ == '__main__':
    main()
