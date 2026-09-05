#!/usr/bin/env python3
"""Disassemblatore MIPS32 big-endian minimo, per ricavare le FIRME degli hook.

Serve a una cosa sola, ed e' la cosa per cui un hook si puo' scrivere: sapere
quale argomento porta l'offset, quale il valore, quale la lunghezza, e se il
prologo e' agganciabile. Senza questo un hook si scrive a indovinare, i campi
del record finiscono sbagliati e la cattura va rifatta.

Non serve a ricavare la SEMANTICA di cio' che una funzione fa. Quella si legge
dalle catture: un oggetto pre-link non e' necessariamente il codice che gira,
e le conclusioni tratte da qui su "cosa fa il driver" sono speculazione anche
quando il disassemblato e' corretto.

Il binutils di sistema non ha il target MIPS -- `objdump -d` su questi oggetti
risponde "can't disassemble for architecture UNKNOWN" -- da cui questo.

Due cose da sapere per leggere l'uscita:

  In un oggetto REL i target delle chiamate stanno nelle RILOCAZIONI, non
  nell'istruzione: una `jal` ha campo zero e le coppie `lui`/`addiu` hanno
  immediati zero. Lo strumento annota i target dalle `.rel<sezione>`, e per le
  coppie HI16/LO16 il valore si ricompone come `(hi << 16) + lo_con_segno`.
  Leggere il disassemblato senza le rilocazioni fa concludere che una funzione
  non chiami nessuno.

  Cio' che non riconosce lo stampa come `.word`, cosi' un'istruzione non
  coperta si vede invece di essere decodificata male.

Uso:
    mipsdis.py <oggetto> <indirizzo> <byte> [sezione]
    mipsdis.py <oggetto> --prologo <simbolo>[,<simbolo>...]

`--prologo` stampa le prime otto istruzioni di ciascun simbolo e dice se c'e'
un branch nella finestra delle prime quattro parole, che e' cio' che decide fra
detour classico, short-j e patch dei siti di chiamata.
"""

import re
import struct
import subprocess
import sys

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
       't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
       't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']

# op-code dei salti e dei branch: servono per la diagnosi del prologo, non solo
# per stampare il mnemonico giusto.
BRANCH_OPS = {1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23}
BRANCH_FN = {0x08, 0x09}          # jr, jalr (special, op == 0)


def sezioni(path):
    out = {}
    for l in subprocess.run(['readelf', '-SW', path],
                            capture_output=True, text=True).stdout.splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)'
                     r'\s+([0-9a-f]+)', l)
        if m:
            out[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16),
                               int(m.group(4), 16))
    return out


def simboli(path):
    out = {}
    for l in subprocess.run(['readelf', '-sW', path],
                            capture_output=True, text=True).stdout.splitlines():
        m = re.match(r'\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+'
                     r'(\d+)\s+(\S+)$', l)
        if m:
            out.setdefault(m.group(4),
                           (int(m.group(1), 16), int(m.group(2)),
                            int(m.group(3))))
    return out


def rilocazioni(path, secname):
    """offset -> (tipo, simbolo) per la sezione .rel<secname>"""
    out = {}
    cur = None
    for l in subprocess.run(['readelf', '-rW', path],
                            capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"Relocation section '(\S+)'", l)
        if m:
            cur = m.group(1)
            continue
        if cur != '.rel' + secname:
            continue
        m = re.match(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+(\S+)\s*'
                     r'(?:[0-9a-f]+\s+)?(\S*)', l)
        if m:
            out[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return out


def istruzione(word, pc, rel):
    op = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    sa, fn, imm = (word >> 6) & 31, word & 63, word & 0xffff
    simm = imm - 0x10000 if imm & 0x8000 else imm
    R = lambda i: '$' + REG[i]

    if word == 0:
        return 'nop'
    if op == 0:
        if fn == 0x08:
            return f'jr {R(rs)}'
        if fn == 0x09:
            return f'jalr {R(rs)}'
        f = {0x21: 'addu', 0x20: 'add', 0x23: 'subu', 0x25: 'or', 0x24: 'and',
             0x26: 'xor', 0x27: 'nor', 0x2b: 'sltu', 0x2a: 'slt',
             0x00: 'sll', 0x02: 'srl', 0x03: 'sra',
             0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav'}.get(fn)
        if f in ('sll', 'srl', 'sra'):
            return f'{f} {R(rd)}, {R(rt)}, {sa}'
        if f:
            return f'{f} {R(rd)}, {R(rs)}, {R(rt)}'
        return f'.word 0x{word:08x}'
    if op in (2, 3):
        r = rel.get(pc)
        tag = f'  <{r[1]}>' if r and r[1] else ''
        return f"{'j' if op == 2 else 'jal'} 0x{(word & 0x3ffffff) << 2:x}{tag}"
    f = {0x08: 'addi', 0x09: 'addiu', 0x0c: 'andi', 0x0d: 'ori', 0x0e: 'xori',
         0x0a: 'slti', 0x0b: 'sltiu'}.get(op)
    if f:
        r = rel.get(pc)
        tag = f'  <{r[1]}>' if r and r[1] else ''
        return f'{f} {R(rt)}, {R(rs)}, 0x{imm:x}{tag}'
    if op == 0x0f:
        r = rel.get(pc)
        tag = f'  <{r[1]}>' if r and r[1] else ''
        return f'lui {R(rt)}, 0x{imm:x}{tag}'
    ld = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu',
          0x28: 'sb', 0x29: 'sh', 0x2b: 'sw'}.get(op)
    if ld:
        return f'{ld} {R(rt)}, {simm}({R(rs)})'
    br = {4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz'}.get(op)
    if br:
        return f'{br} {R(rs)}, {R(rt)}, 0x{pc + 4 + simm * 4:x}'
    if op == 1:
        return f"{'bltz' if rt == 0 else 'bgez'} {R(rs)}, 0x{pc + 4 + simm * 4:x}"
    return f'.word 0x{word:08x}'


def e_branch(word):
    op = word >> 26
    if op == 0:
        return (word & 63) in BRANCH_FN
    return op in BRANCH_OPS


def leggi(path, sec, start, n):
    base, off, _ = sec
    with open(path, 'rb') as f:
        f.seek(off + start)
        return f.read(n)


def dump(path, secname, start, size, rel):
    sec = sezioni(path)[secname]
    data = leggi(path, sec, start, size)
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack('>I', data[i:i + 4])[0]
        print(f"  {start + i:08x}  {w:08x}   {istruzione(w, start + i, rel)}")


def prologhi(path, nomi):
    sy = simboli(path)
    S = sezioni(path)
    for nome in nomi:
        if nome not in sy:
            print(f"--- {nome}: ASSENTE")
            continue
        addr, size, shndx = sy[nome]
        secname = next(k for k, v in S.items()
                       if v[0] == 0 and k.startswith('.text')) \
            if False else '.text'
        rel = rilocazioni(path, secname)
        print(f"--- {nome} @{addr:#x} ({size} B)")
        data = leggi(path, S[secname], addr, min(size, 32))
        branch = None
        for i in range(0, len(data) - 3, 4):
            w = struct.unpack('>I', data[i:i + 4])[0]
            k = i // 4
            if k < 4 and e_branch(w) and branch is None:
                branch = k
            print(f"    [{k}] {w:08x}  {istruzione(w, addr + i, rel)}")
        if branch is None:
            print("    prologo: 4 parole libere -> detour classico")
        else:
            print(f"    prologo: branch a parola {branch} -> serve short-j "
                  f"o patch dei siti")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path = sys.argv[1]
    if sys.argv[2] == '--prologo':
        prologhi(path, sys.argv[3].split(','))
        return 0
    start, size = int(sys.argv[2], 0), int(sys.argv[3], 0)
    secname = sys.argv[4] if len(sys.argv) > 4 else '.text'
    dump(path, secname, start, size, rilocazioni(path, secname))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
