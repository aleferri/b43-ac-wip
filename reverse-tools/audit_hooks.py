#!/usr/bin/env python3
"""Per ogni hook di wl_diag: il simbolo c'e' in questo .ko, e il prologo e'
agganciabile? Risponde su un blob PRIMA di bruciare una corsa di cattura.

Replica is_branch() del modulo, niente disassemblatore: bastano i primi 4 opcode
(2 con .shortj). Le colonne:

  ASSENTE       il simbolo non c'e' in questa build: l'hook non si armera' mai e
                dmesg dira' "non trovato".
  SALTATO       branch nella finestra: il detour non regge. NON significa nessun
                hook -- il modulo prova prima la deviazione della tail call, poi
                i siti di chiamata, poi il percorso a break.
  tail-call     la parola che ferma il detour e' una `j` assoluta: sta sul
                percorso d'ingresso per costruzione, quindi il modulo devia
                quella sola parola. E' il caso dei thunk.
  siti          punti in cui l'indirizzo del simbolo compare in un chiamante:
                coppie HI16/LO16, che e' la via che find_sites patcha, piu' le
                `jal`/`j` (R_MIPS_26). Le due forme si contano separate perche'
                **solo le prime sono patchabili** da find_sites com'e' oggi.
                **Zero siti non e' un dettaglio**: GCC emette la copia
                out-of-line di una funzione GLOBAL anche quando la inlinea in
                tutti i chiamanti, quindi il simbolo si risolve, il prologo si
                aggancia, e la classe di op resta a zero per sempre. La riga
                viene marcata e la classe non entra fra quelle coperte.
  t8/t9         lo stub ha bisogno di un registro per il salto di rientro: se le
                parole spiazzate scrivono sia t8 sia t9, pianifica() scarta
                l'hook e qui si legge prima di andare sul router.
  BERSAGLIO     una parola dentro la finestra e' il bersaglio di un branch che
                arriva da dentro la funzione: spiazzarla la fa saltare a chi ci
                arriva da li'. Vale per il detour a 4 parole quanto per lo
                short-j, e nessuno dei due lo verifica a runtime.

Prima del piano controlla anche la forma della tabella -- che ogni `.campo =`
esista in struct hook, che nessuna voce abbia piu' inizializzatori posizionali
dei campi che li accettano, che il codice non legga campi non dichiarati. Sono
errori che da questa parte non li trova nessuno, perche' il tracer si compila
con gli header del kernel del router: si scoprono a build fallita in fondo a un
ciclo di flash. E' successo due volte.

Il verdetto su prologo e registro esce identico su un oggetto pre-link e sul
.ko estratto dal firmware. Il conteggio dei siti e la presenza dei simboli no:
in un .ko i riferimenti ai simboli locali possono passare per il simbolo di
sezione, e la symtab puo' essere molto piu' povera di quella del pre-link.

Un simbolo LOCAL (static) di TESTO si risolve sul device anche senza
CONFIG_KALLSYMS_ALL: `is_core_symbol()` in kernel/module.c filtra sui flag di
sezione -- SHF_ALLOC piu' SHF_EXECINSTR quando l'opzione e' spenta -- e non sul
binding. Quello che serve KALLSYMS_ALL e' un simbolo DATO.

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


def siti(ko, blob, se, sy):
    """Siti di chiamata per simbolo: (coppie HI16/LO16, jal/j).

    Le due forme non sono intercambiabili: find_sites patcha solo le coppie.
    In un .ko un riferimento a un simbolo LOCAL puo' passare per il simbolo di
    sezione, con il bersaglio nell'addendo dentro l'istruzione, quindi non
    basta guardare il nome nella colonna della rilocazione.
    """
    idx_txt = next((i for i, (n, _, _) in se.items() if n == '.text'), None)
    per_val = {}
    for nome, (val, _dim, sec) in sy.items():
        if sec == idx_txt:
            per_val.setdefault(val, nome)
    pair = {}
    jal = {}
    sec = None
    for l in subprocess.run(['readelf', '-rW', ko], capture_output=True,
                            text=True).stdout.split('\n'):
        m = re.match(r"Relocation section '(\S+)'", l)
        if m:
            sec = m.group(1)
            continue
        if not (sec and sec.startswith('.rel.text')):
            continue
        p = l.split()
        if len(p) < 5 or not re.fullmatch(r'[0-9a-f]{8}', p[0]):
            continue
        if p[2] in ('R_MIPS_HI16', 'R_MIPS_LO16'):
            pair[p[4]] = pair.get(p[4], 0) + 1
        elif p[2] == 'R_MIPS_26':
            nome = p[4]
            if nome == '.text':
                ospite = se_per_nome(se, sec[4:])
                if ospite is None:
                    continue
                _, sh_addr, sh_off = ospite
                o = sh_off + (int(p[0], 16) - sh_addr)
                tgt = (struct.unpack('>I', blob[o:o + 4])[0] & 0x03ffffff) << 2
                nome = per_val.get(tgt)
                if nome is None:
                    continue
            jal[nome] = jal.get(nome, 0) + 1
    return {n: (pair.get(n, 0) // 2, jal.get(n, 0))
            for n in set(pair) | set(jal)}


def se_per_nome(se, nome):
    for _, v in se.items():
        if v[0] == nome:
            return v
    return None


def bersaglio_in_finestra(blob, off, addr, dim, n):
    """Indice di una parola della finestra raggiunta da un branch interno.

    Spiazzarla la farebbe sparire per chi ci arriva da li'. Il controllo vale
    per il detour a 4 parole quanto per lo short-j, e nessuno dei due lo fa a
    runtime: qui si vede da fermi.
    """
    for i in range(dim // 4):
        w = struct.unpack('>I', blob[off + 4 * i:off + 4 * i + 4])[0]
        op = w >> 26
        if op == 0x01 or 0x04 <= op <= 0x07 or 0x14 <= op <= 0x17:
            imm = w & 0xffff
            if imm >= 0x8000:
                imm -= 0x10000
            tgt = addr + 4 * i + 4 + 4 * imm
        elif op == 0x02:
            tgt = (w & 0x03ffffff) << 2
        else:
            continue
        if addr < tgt < addr + 4 * n:
            return (tgt - addr) // 4
    return None


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
    se = sezioni(ko)
    blob = open(ko, 'rb').read()
    st = siti(ko, blob, se, sy)
    ops = leggi_enum(testo)
    classi = leggi_classi()
    tabella = re.findall(r'\{\s*"([^"]+)",\s*(OP_\w+)([^}]*)\}', testo)

    preso = set()
    print(f"{'simbolo':<32} {'esito':<11} {'dim':>5} {'siti':>7}  dettaglio")
    for nome, op, resto in tabella:
        shortj = '.shortj' in resto
        opn = ops.get(op)
        if opn in preso:
            continue                       # un op, un hook: vedi op_preso
        if nome not in sy:
            print(f"{nome:<32} {'ASSENTE':<11} {'-':>5} {'-':>7}")
            continue
        addr, dim, sec = sy[nome]
        _, sh_addr, sh_off = se[sec]
        off = sh_off + (addr - sh_addr)
        n = 2 if shortj else 4
        np, nj = st.get(nome, (0, 0))
        ns = f"{np}+{nj}"
        if dim < 4 * n:
            print(f"{nome:<32} {'TROPPO CORTA':<11} {dim:>5} {ns:>7}  "
                  f"funzione di {dim} B: piu' corta della finestra di {4*n} B")
            continue
        w = struct.unpack('>%dI' % n, blob[off:off + 4 * n])
        br = next((j for j in range(n) if is_branch(w[j])), None)
        note = []
        if np + nj == 0:
            note.append("NESSUN chiamante: inlineata ovunque o morta, "
                        "l'hook si pianifica e non emettera' mai")
        tail = br is not None and (w[br] >> 26) == 0x02
        if tail:
            esito = 'ok'
            come = f"tail-call alla parola {br}: si devia quella sola parola"
            preso.add(opn)
        elif br is not None:
            esito, come = ('siti', f"branch alla parola {br} (0x{w[br]:08x}), "
                                   f"{np} coppie patchabili")
            if np:
                preso.add(opn)
            elif not is_branch(w[0]):
                esito, come = ('ok', f"branch alla parola {br}, nessuna coppia "
                                     f"HI16/LO16: resta il percorso a break")
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
            if np + nj:
                preso.add(opn)
        if not tail and esito == 'ok' and 'break' not in come:
            k = bersaglio_in_finestra(blob, off, addr, dim, n)
            if k is not None:
                esito = 'BERSAGLIO'
                come = (f"la parola {k} della finestra e' bersaglio di un "
                        f"branch interno: spiazzarla la fa sparire")
                preso.discard(opn)
        print(f"{nome:<32} {esito:<11} {dim:>5} {ns:>7}  {come}")
        for x in note:
            print(f"{'':<32} {'':<11} {'':>5} {'':>7}  ({x})")

    vive = sorted(classi[n] for n in preso if n in classi)
    print('\nclassi che la cattura conterrebbe:\n  ' + ', '.join(vive))
    fuori = sorted({classi[ops[op]] for _, op, _ in tabella
                    if ops.get(op) not in preso and ops.get(op) in classi})
    if fuori:
        print('\nclassi che resterebbero fuori:\n  ' + ', '.join(fuori))
    return 1 if errori else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))
