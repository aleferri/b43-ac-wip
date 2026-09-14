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
                **Zero siti non e' un dettaglio**: GCC emette la copia
                out-of-line di una funzione GLOBAL anche quando la inlinea in
                tutti i chiamanti, quindi il simbolo si risolve, il prologo si
                aggancia, e la classe di op resta a zero per sempre. La riga
                viene marcata e la classe non entra fra quelle coperte.
  t8/t9         lo stub ha bisogno di un registro per il salto di rientro: se le
                parole spiazzate scrivono sia t8 sia t9, pianifica() scarta
                l'hook e qui si legge prima di andare sul router.

Prima del piano controlla anche la forma della tabella -- che ogni `.campo =`
esista in struct hook, che nessuna voce abbia piu' inizializzatori posizionali
dei campi che li accettano, che il codice non legga campi non dichiarati. Sono
errori che da questa parte non li trova nessuno, perche' il tracer si compila
con gli header del kernel del router: si scoprono a build fallita in fondo a un
ciclo di flash. E' successo due volte.

Un oggetto **pre-link** basta: `.text` e le rilocazioni sono gia' quelle
definitive, e il piano esce identico a quello sul .ko estratto dal firmware.
Un simbolo LOCAL (static) qui si vede sempre, sul device solo con
CONFIG_KALLSYMS_ALL: viene segnalato.

  ./audit_hooks.py wl.ko [wl-diag/wl_diag.c]
"""
import os
import re
import struct
import subprocess
import sys


HERE = os.path.dirname(os.path.abspath(__file__))


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


def membri_hook(src):
    """I campi dichiarati in struct hook, nell'ordine."""
    m = re.search(r'struct hook\s*\{(.*?)\n\};', src, re.S)
    if not m:
        sys.exit('hook_plan: struct hook non trovata')
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    campi = []
    for riga in body.split(';'):
        riga = riga.strip()
        if not riga:
            continue
        for nome in re.findall(r'(\w+)\s*(?:\[[^\]]*\])?\s*(?:,|$)',
                               riga.split(None, 1)[-1] if ' ' in riga else ''):
            campi.append(nome)
    return campi

def lint(src):
    """Gli errori di forma che il compilatore trova solo sul device.

    Il tracer si compila con gli header del kernel del router, quindi da questa
    parte non c'e' nessun gate: un campo usato e non dichiarato, o un
    inizializzatore posizionale di troppo, si scoprono a build fallita in fondo
    a un ciclo di flash. E' successo due volte -- un `true` destinato a retcap
    finito in use_bp, e `.ripiego_di` usato senza essere mai stato dichiarato.
    """
    campi = set(membri_hook(src))
    errori = []

    m = re.search(r'static struct hook hooks\[\]\s*=\s*\{(.*?)\n\};',
                  src, re.S)
    corpo = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)

    for nome in sorted(set(re.findall(r'\.(\w+)\s*=', corpo))):
        if nome not in campi:
            errori.append("la tabella inizializza .%s, che struct hook non "
                          "dichiara" % nome)

    for nome in sorted(set(re.findall(r'\bh->(\w+)', src)
                           + re.findall(r'\bhooks\[[^\]]+\]\.(\w+)', src))):
        if nome not in campi:
            errori.append("il codice legge ->%s, che struct hook non dichiara"
                          % nome)

    # I campi posizionali sono quelli prima del primo che la tabella imposta
    # solo per nome; una voce non puo' averne di piu'.
    per_nome = set(re.findall(r'\.(\w+)\s*=', corpo))
    pos_max = next((i for i, c in enumerate(membri_hook(src))
                    if c in per_nome), len(campi))
    for voce in re.finditer(r'\{([^{}]*)\}', corpo):
        pezzi = [x.strip() for x in voce.group(1).split(',') if x.strip()]
        n = len([x for x in pezzi if not x.startswith('.')])
        if n > pos_max:
            errori.append("la voce %s ha %d inizializzatori posizionali, "
                          "il massimo e' %d" % (pezzi[0], n, pos_max))
    return errori

def leggi_enum(src):
    """OP_* -> numero, dall'enum wldiag_op."""
    m = re.search(r'enum wldiag_op\s*\{(.*?)\}', src, re.S)
    if not m:
        return {}
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    ops, nxt = {}, 0
    for tok in body.split(','):
        tok = tok.strip()
        if not tok:
            continue
        if '=' in tok:
            nome, val = (x.strip() for x in tok.split('='))
            nxt = int(val, 0)
        else:
            nome = tok
        ops[nome] = nxt
        nxt += 1
    return ops

def leggi_classi():
    """numero -> nome di classe, dal decoder: e' l'unico posto dove stanno."""
    path = os.path.join(HERE, 'decode-wl-diag.py')
    try:
        src = open(path).read()
    except OSError:
        return {}
    cl = {}
    for m in re.finditer(r'(\d+):\s*"([A-Z][A-Z0-9._]*)"', src):
        cl[int(m.group(1))] = m.group(2)
    return cl


# ---------------------------------------------------------------- l'oggetto

def reg_dest(w):
    """Registro di destinazione, o None. Serve per il registro di rientro."""
    op = w >> 26
    if op == 0:
        return None if (w & 0x3f) in (0x08, 0x09) else (w >> 11) & 31
    if op in (0x0f, 0x09, 0x0c, 0x0d, 0x0a, 0x0b, 0x08) or 0x20 <= op <= 0x27:
        return (w >> 16) & 31
    return None


def main(ko, sorgente=None):
    sorgente = sorgente or os.path.join(HERE, 'wl-diag', 'wl_diag.c')
    testo = open(sorgente).read()

    errori = lint(testo)
    for e in errori:
        print('ERRORE: ' + e)
    if errori:
        print('\nIl tracer non compilerebbe. Il piano qui sotto non vale.\n')

    sy = simboli(ko)
    st = siti(ko)
    se = sezioni(ko)
    ops = leggi_enum(testo)
    classi = leggi_classi()
    blob = open(ko, 'rb').read()
    tabella = re.findall(r'\{\s*"([^"]+)",\s*(OP_\w+)([^}]*)\}', testo)

    preso = set()
    print(f"{'simbolo':<32} {'esito':<11} {'dim':>5} {'siti':>5}  dettaglio")
    for nome, op, resto in tabella:
        shortj = '.shortj' in resto
        opn = ops.get(op)
        if opn in preso:
            continue                       # un op, un hook: vedi op_preso
        if nome not in sy:
            print(f"{nome:<32} {'ASSENTE':<11} {'-':>5} {'-':>5}")
            continue
        addr, dim, sec = sy[nome]
        _, sh_addr, sh_off = se[sec]
        off = sh_off + (addr - sh_addr)
        n = 2 if shortj else 4
        ns = st.get(nome, 0)
        if dim < 4 * n:
            print(f"{nome:<32} {'TROPPO CORTA':<11} {dim:>5} {ns:>5}  "
                  f"funzione di {dim} B: piu' corta della finestra di {4*n} B")
            continue
        w = struct.unpack('>%dI' % n, blob[off:off + 4 * n])
        br = next((j for j in range(n) if is_branch(w[j])), None)
        note = []
        if ns == 0:
            note.append("NESSUN chiamante: inlineata ovunque o morta, "
                        "l'hook si pianifica e non emettera' mai")
        if br is not None:
            esito, come = ('siti', f"branch alla parola {br} (0x{w[br]:08x}), "
                                   f"{ns} siti da patchare")
            if ns:
                preso.add(opn)
            else:
                esito = 'SALTATO'
        elif (any(reg_dest(x) == 24 for x in w)
              and any(reg_dest(x) == 25 for x in w)):
            esito, come = 'SALTATO', 'le parole spiazzate scrivono sia t8 sia t9'
        else:
            esito = 'ok'
            rientro = 8 if any(reg_dest(x) == 25 for x in w) else 9
            come = (f"{'short-j' if shortj else 'detour 4 parole'}, "
                    f"rientro su $t{rientro}")
            if ns:
                preso.add(opn)
        print(f"{nome:<32} {esito:<11} {dim:>5} {ns:>5}  {come}")
        for x in note:
            print(f"{'':<32} {'':<11} {'':>5} {'':>5}  ({x})")

    vive = sorted(classi[n] for n in preso if n in classi)
    print('\nclassi che la cattura conterrebbe:\n  ' + ', '.join(vive))
    fuori = sorted({classi[ops[op]] for _, op, _ in tabella
                    if ops.get(op) not in preso and ops.get(op) in classi})
    if fuori:
        print('\nclassi che resterebbero fuori:\n  ' + ', '.join(fuori))
    return 1 if errori else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))
