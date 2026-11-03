#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""agcombo.py TRACE: the radar FIFO reads of a wl-diag trace in replay's format.

Every read of PHY 0x0251 opens a poll; the values are on the RETVAL lines.
"""
import re
import sys

pend, out = {}, []
for line in open(sys.argv[1]):
    m = re.search(r'#(\d+)\s+cpu\d\s+PHY\.RD\s+addr=0x(025[1-4])', line)
    if m:
        pend[m.group(1)] = m.group(2)
        continue
    m = re.search(r'RETVAL\s+for=#(\d+) val=0x([0-9a-f]+)', line)
    if m and m.group(1) in pend:
        reg, v = pend.pop(m.group(1)), int(m.group(2), 16) & 0xffff
        if reg == '0251':
            out.append(f'poll {v:04x}')
        elif reg == '0252':
            out[-1] += f' {v:04x}'
        else:
            out.append(f"{0 if reg == '0253' else 1} {v:04x}")
print('\n'.join(out))
