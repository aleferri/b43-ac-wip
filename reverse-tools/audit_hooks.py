#!/usr/bin/env python3
"""Per ogni hook di wl_diag: il simbolo c'e' in questo .ko, e il prologo e'
agganciabile? Risponde su un blob PRIMA di bruciare una corsa di cattura.

Replica is_branch() del modulo, niente disassemblatore: bastano i primi 4 opcode
(2 con .shortj). Le colonne:

  ASSENTE       il simbolo non c'e' in questa build: l'hook non si armera' mai e
                dmesg dira' "non trovato".
  SALTATO       branch nella finestra: il detour non regge. NON significa nessun
                hook -- il modulo prova prima i siti di chiamata e poi il
                percorso a break, ed e' per questo che si conta anche `siti`.
  siti          coppie di rilocazioni HI16/LO16 verso il simbolo, cioe' i punti
                dove l'indirizzo viene materializzato per una chiamata
                indiretta. Questi driver chiamano quasi tutto cosi', non con
                jal, ed e' la via che find_sites patcha.

  ./audit_hooks.py wl.ko wl-diag/wl_diag.c
"""
import re
import struct
import subprocess
import sys


def is_branch(insn):
    op = insn >> 26
    if op == 0:
        f = insn & 0x3f
        return f in (0x08, 0x09)          # jr / jalr
    if op == 0x01:
        return True                        # REGIMM
    if op in (0x02, 0x03):
        return True                        # j / jal
    if 0x04 <= op <= 0x07:
        return True                        # beq/bne/blez/bgtz
    if op in (0x14, 0x15, 0x16, 0x17):
        return True                        # beql/bnel/...
    return False


def simboli(ko):
    out = {}
    for l in subprocess.run(['readelf', '-sW', ko], capture_output=True,
                            text=True).stdout.split('\n'):
        p = l.split()
        if len(p) >= 8 and p[3] == 'FUNC':
            out[p[7]] = (int(p[1], 16), int(p[2]), int(p[6]))
    return out


def siti(ko):
    """coppie HI16/LO16 per simbolo: i punti di chiamata indiretta"""
    fuori = {}
    sec = None
    for l in subprocess.run(['readelf', '-rW', ko], capture_output=True,
                            text=True).stdout.split('\n'):
        m = re.match(r"Relocation section '(\S+)'", l)
        if m:
            sec = m.group(1)
            continue
        p = l.split()
        if sec == '.rel.text' and len(p) >= 5 and re.fullmatch(r'[0-9a-f]{8}', p[0]):
            if p[2] in ('R_MIPS_HI16', 'R_MIPS_LO16'):
                fuori[p[4]] = fuori.get(p[4], 0) + 1
    return {k: v // 2 for k, v in fuori.items()}


def sezioni(ko):
    out = {}
    for l in subprocess.run(['readelf', '-SW', ko], capture_output=True,
                            text=True).stdout.split('\n'):
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)', l)
        if m:
            out[int(m.group(1))] = (m.group(2), int(m.group(4), 16),
                                    int(m.group(5), 16))
    return out


def main(ko, sorgente):
    sy = simboli(ko)
    st = siti(ko)
    se = sezioni(ko)
    blob = open(ko, 'rb').read()
    tabella = re.findall(r'\{\s*"([^"]+)",\s*(OP_\w+)([^}]*)\}',
                         open(sorgente).read())

    print(f"{'simbolo':<32} {'esito':<11} {'dim':>5} {'siti':>5}  dettaglio")
    for nome, op, resto in tabella:
        shortj = '.shortj' in resto
        if nome not in sy:
            print(f"{nome:<32} {'ASSENTE':<11} {'-':>5} {'-':>5}")
            continue
        addr, dim, sec = sy[nome]
        _, sh_addr, sh_off = se[sec]
        off = sh_off + (addr - sh_addr)
        n = 2 if shortj else 4
        w = struct.unpack('>%dI' % n, blob[off:off + 4 * n])
        br = next((j for j in range(n) if is_branch(w[j])), None)
        if dim < 4 * n:
            nota = f"funzione di {dim} B: piu' corta della finestra di {4*n} B"
            print(f"{nome:<32} {'TROPPO CORTA':<11} {dim:>5} "
                  f"{st.get(nome, 0):>5}  {nota}")
            continue
        if br is None:
            print(f"{nome:<32} {'ok':<11} {dim:>5} {st.get(nome, 0):>5}  "
                  f"{'short-j' if shortj else 'detour 4 parole'}")
        else:
            print(f"{nome:<32} {'SALTATO':<11} {dim:>5} {st.get(nome, 0):>5}  "
                  f"branch alla parola {br} (0x{w[br]:08x})")


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
