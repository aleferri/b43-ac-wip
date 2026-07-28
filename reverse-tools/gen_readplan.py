#!/usr/bin/env python3
"""Emit a C read plan for one PHY register from the values a capture recorded.

The harness serves register reads from three sources, in order: the read oracle
(`AC_READ_ORACLE`, the whole capture), a scripted plan, and finally the mirror
of the writes. Flows that run without an oracle need a plan for any register
whose read value steers control flow -- otherwise the mirror answers with the
last value written, which for a self-clearing status bit never advances.

Captures are folded through merge_retvals.py here, so the raw capture path is
what gets passed in and what gets recorded as provenance. Only captures logged
with "capture ret val" qualify; without the RETVAL lines the values are
UNDEFINED and there is nothing to extract.

Usage:
    gen_readplan.py ADDR NAME=CAPTURE [NAME=CAPTURE ...]

ADDR is the register, hex or decimal. NAME becomes the C identifier suffix.
Output goes to stdout.

Example:
    python3 reverse-tools/gen_readplan.py 0x0270 \
        d6220_first=router-data/d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt \
        > test/readplan_0270.h
"""
import os
import re
import subprocess
import sys
import tempfile

VALS_PER_LINE = 12


def read_values(path, addr):
    """Fold the RETVALs, then pick the values read at `addr`, in order."""
    merger = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          'merge_retvals.py')
    with tempfile.NamedTemporaryFile(suffix='.merged.txt') as tmp:
        subprocess.run([sys.executable, merger, path, tmp.name],
                       check=True, stdout=subprocess.DEVNULL)
        rx = re.compile(r'PHY\.RD\s+addr=0x0*%x\s+val=0x([0-9a-fA-F]+)\b' % addr)
        out = []
        for line in open(tmp.name, errors='replace'):
            m = rx.search(line)
            if m:
                out.append(int(m.group(1), 16) & 0xffff)
    return out


def emit(addr, entries):
    guard = 'B43_TEST_READPLAN_%04X_H' % addr
    print('/*')
    print(' * Read plan per PHY 0x%04x, generato da reverse-tools/gen_readplan.py.' % addr)
    print(' * Non modificare a mano: rigenerare dalle catture (vedi test/README.md).')
    print(' *')
    print(' * I valori sono quelli che il tracer vendor ha registrato, nell\'ordine in')
    print(' * cui il driver stock li ha letti. Servono ai flow che girano senza')
    print(' * AC_READ_ORACLE: senza di essi il mirror delle write risponderebbe con il')
    print(' * bit di start ancora alto e il poll del driver girerebbe fino al timeout.')
    print(' */')
    print('#ifndef %s' % guard)
    print('#define %s' % guard)
    for name, path, vals in entries:
        print()
        print('/* %d letture -- %s */' % (len(vals), path))
        print('static const u16 readplan_%04x_%s[] = {' % (addr, name))
        for i in range(0, len(vals), VALS_PER_LINE):
            chunk = vals[i:i + VALS_PER_LINE]
            print('\t' + ' '.join('0x%04x,' % v for v in chunk))
        print('};')
    print()
    print('#endif /* %s */' % guard)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    addr = int(sys.argv[1], 0)
    entries = []
    for spec in sys.argv[2:]:
        if '=' not in spec:
            sys.exit('atteso NAME=CAPTURE, ricevuto %r' % spec)
        name, path = spec.split('=', 1)
        vals = read_values(path, addr)
        if not vals:
            sys.exit('%s: nessuna lettura di 0x%04x con un valore. '
                     'La cattura e\' stata registrata con "capture ret val"?'
                     % (path, addr))
        entries.append((name, path, vals))
    emit(addr, entries)


if __name__ == '__main__':
    main()
