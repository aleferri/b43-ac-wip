#!/usr/bin/env python3
"""Annota una cattura wl-diag riga per riga con lo stato corrente degli
enable, ognuno marcato con l'#op che l'ha impostato per ultimo:

  MAC   MAC.MCTRL bit 0x00000001 (EN_MAC)                 -> MAC=0/1
  RX    PHY 0x140 (CLASSCTL) bits CCK/OFDM/WAITED         -> RX=cow / RX=w / RX=-
  CLIP  PHY 0x6d4/0x8d4/0xad4 bit 0x4000 (clip det/core)  -> CLIP=111 (core2,1,0)
  PHY   PHY 0x001 (BBCFG) bit 0x8000 (quiesce)            -> PHY=0/1
  CCA   PHY 0x001 (BBCFG) bit 0x4000 (RSTCCA)             -> CCA=0/1
  AFE   PHY 0x1720 bit 0x0200 (front-end armed vs parked) -> AFE=ON/DOWN
  RCAL  RAD 0x8ea (RCCAL_EN1) bit 0x0080 (gate cal)       -> RCAL=0/1
  GPIO  GPIO.OUT pin2 (0x4) e pin10 (0x400)               -> GPIO=2A / 2- / -A / --

Uso: annotate_enables.py <cattura.txt> [out.txt]
Il sommario delle transizioni va su stderr. '?' = mai toccato finora.
"""
import re
import sys

OP = re.compile(r'(?:#(\d+)\s+)?cpu\d\s+(\S+)\s+(?:addr=0x([0-9a-f]+)\s+)?'
                r'val=(0x[0-9a-f]+|UNDEFINED)(?:\s+\(.*\))?(?:\s+mask=(0x[0-9a-f]+))?')

# nome -> lista di sorgenti (op_prefix, addr|None, bitmask, shift_out)
DOMAINS = {
    'MAC':  [('MAC.MCTRL', None, 0x1, 0)],
    'RX':   [('PHY.', 0x140, 0x7, 0)],
    'CLIP': [('PHY.', 0x6d4, 0x4000, 0), ('PHY.', 0x8d4, 0x4000, 1),
             ('PHY.', 0xad4, 0x4000, 2)],
    'PHY':  [('PHY.', 0x001, 0x8000, 0)],
    'CCA':  [('PHY.', 0x001, 0x4000, 0)],
    'AFE':  [('PHY.', 0x1720, 0x0200, 0)],
    'RCAL': [('RAD.', 0x8ea, 0x0080, 0)],
    'GPIO': [('GPIO.OUT', None, 0x404, 0)],
}
RENDER = {
    'RX':   lambda b: (('c' if b & 1 else '') + ('o' if b & 2 else '') +
                       ('w' if b & 4 else '')) or '-',
    'CLIP': lambda b: format(b, '03b'),
    'AFE':  lambda b: 'ON' if b else 'DOWN',
    'GPIO': lambda b: ('2' if b & 0x4 else '-') + ('A' if b & 0x400 else '-'),
}


def render(name, bits):
    if bits is None:
        return '?'
    if name in RENDER:
        return RENDER[name](bits)
    return '1' if bits else '0'


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.replace('.txt', '-enables.txt')
    raw = {}    # (name, idx_sorgente) -> valore raw dei bit tracciati (o None)
    agg = {n: (None, '?') for n in DOMAINS}   # name -> (bits aggregati, #op)
    for n, srcs in DOMAINS.items():
        for i, _ in enumerate(srcs):
            raw[(n, i)] = None

    out = open(dst, 'w')
    transitions = []
    for lineno, line in enumerate(open(src), 1):
        m = OP.search(line)
        if m:
            opno, op, addr_s, val_s, mask_s = m.groups()
            tag = f'#{opno}' if opno else f'L{lineno}'
            addr = int(addr_s, 16) if addr_s else None
            val = None if val_s == 'UNDEFINED' else int(val_s, 16)
            mask = int(mask_s, 16) if mask_s else None
            if val is not None:
                for name, srcs in DOMAINS.items():
                    for i, (pfx, daddr, dmask, shift) in enumerate(srcs):
                        if not op.startswith(pfx) or daddr != addr:
                            continue
                        cur = raw[(name, i)] or 0
                        if op.endswith('.WR'):
                            new = val
                        elif op.endswith('.OR'):
                            new = cur | val
                        elif op.endswith('.AND'):
                            new = cur & val
                        else:  # MOD / MCTRL / GPIO.OUT: coppia val+mask
                            # mask=0 in old traces = OR/AND captured without
                            # dedicated hooks → treat as full write (safe for
                            # single-bit domain tracking).
                            if mask:
                                new = (cur & ~mask) | (val & mask)
                            else:
                                new = val
                        raw[(name, i)] = new & dmask
                        bits = 0
                        known = True
                        for j, (_, _, dm, sh) in enumerate(srcs):
                            v = raw[(name, j)]
                            if v is None:
                                known = False
                                continue
                            bits |= (1 << sh) if (v & dm) else 0
                        # per RX/GPIO i bit sono quelli raw, non 1-per-sorgente
                        if len(srcs) == 1:
                            bits = raw[(name, 0)]
                        old_bits, _ = agg[name]
                        if old_bits != bits:
                            agg[name] = (bits, tag)
                            transitions.append((line.rstrip(), name, bits,
                                                tag))
                        break
        col = ' '.join(f'{n}={render(n, agg[n][0])}@{agg[n][1]}'
                       for n in DOMAINS)
        out.write(f'{line.rstrip():<98} | {col}\n')
    out.close()

    print(f'annotato -> {dst}', file=sys.stderr)
    print('--- transizioni ---', file=sys.stderr)
    for src_line, name, bits, opno in transitions:
        print(f'{name}={render(name, bits):<5} {opno:<8} {src_line}',
              file=sys.stderr)


if __name__ == '__main__':
    main()
