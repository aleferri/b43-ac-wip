#!/usr/bin/env python3
"""Controllo lessicale minimo su un file C, senza compilatore.

Prende la classe di errori che si fanno editando commenti a mano e che il
compilatore segnala in modo confuso, tipicamente decine di righe dopo la causa:

  - `*/` dentro la prosa di un commento, che lo chiude in anticipo
    (`hndotp_*/ipxotp_*` e' il caso reale che ha rotto la build)
  - commento aperto e non chiuso
  - letterale di stringa o carattere non terminato sulla riga
  - graffe, tonde, quadre non bilanciate
  - uso di un simbolo `static` di file prima della sua dichiarazione: in C non
    c'e' forward reference implicito, quindi spostare un blocco piu' in basso
    durante un refactor rompe la build ('undeclared identifier')

Non e' un parser: e' un tokenizzatore che salta commenti e letterali
correttamente e conta il resto.

Uso: csanity.py file.c [file.c ...]      exit 1 se trova qualcosa
"""
import re
import signal
import sys

# senza questo un `| head` fa uscire un BrokenPipeError
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

PAIRS = {'{': '}', '(': ')', '[': ']'}
CLOSE = {v: k for k, v in PAIRS.items()}


def check(path):
    src = open(path, errors='replace').read()
    errs = []
    stack = []
    i, line = 0, 1
    n = len(src)

    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        # commento di blocco
        if src.startswith('/*', i):
            start = line
            j = src.find('*/', i + 2)
            if j < 0:
                errs.append(f"{path}:{start}: commento aperto e non chiuso")
                break
            # Un commento INLINE seguito da codice e' normale: /* x */ 0x1,
            # Il caso da prendere e' la prosa MULTIRIGA chiusa per sbaglio da un
            # '*/' finito nel testo, dopo cui segue altra prosa -- non codice.
            blocco = src[i:j + 2]
            eol = src.find('\n', j + 2)
            rest = src[j + 2:eol if eol > 0 else n].strip()
            multiriga = '\n' in blocco
            pare_prosa = rest and not any(c in rest for c in ';,)=({')
            if multiriga and pare_prosa and not rest.startswith('//'):
                errs.append(f"{path}:{line + src.count(chr(10), i, j)}: "
                            f"commento multiriga chiuso da un '*/' nella prosa, "
                            f"poi segue: {rest[:48]!r}")
            line += src.count('\n', i, j + 2)
            i = j + 2
            continue
        if src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        # letterali
        if c in '"\'':
            q, j = c, i + 1
            while j < n and src[j] != q:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == '\n':
                    errs.append(f"{path}:{line}: letterale {q} non terminato")
                    break
                j += 1
            if j >= n:
                errs.append(f"{path}:{line}: letterale {q} non terminato a fine file")
                break
            i = j + 1
            continue
        # parentesi
        if c in PAIRS:
            stack.append((c, line))
        elif c in CLOSE:
            if not stack:
                errs.append(f"{path}:{line}: '{c}' senza apertura")
            elif stack[-1][0] != CLOSE[c]:
                o, ol = stack.pop()
                errs.append(f"{path}:{line}: '{c}' chiude un '{o}' aperto a riga {ol}")
            else:
                stack.pop()
        i += 1

    for o, ol in stack:
        errs.append(f"{path}:{ol}: '{o}' mai chiusa")
    return errs


TIPI = (r'int|unsigned|char|bool|void|long|short|float|double|'
        r'u8|u16|u32|u64|s8|s16|s32|s64|size_t|struct|union|enum|const|static')
RX_DECL = re.compile(r'^\t(?:' + TIPI + r')[\s*]+\**\w+\s*(?:[;,=\[]|$)')
RX_STMT = re.compile(r'^\t(?:if|for|while|switch|return|do|goto|\w+\s*\(|\*?\w+\s*=|\w+\+\+|\w+--)')


def check_c90(path):
    """dichiarazione dopo statement: su kernel C90 e' un warning, e in un
    modulo compilato con -Werror diventa un errore. Euristica su indentazione a
    un tab, cioe' il primo livello del corpo di funzione."""
    errs = []
    in_fn = False
    visto_stmt = False
    for n, l in enumerate(open(path, errors='replace'), 1):
        if re.match(r'^[a-zA-Z_].*\)\s*$', l) or re.match(r'^\{', l):
            pass
        if l.startswith('{'):
            in_fn, visto_stmt = True, False
            continue
        if l.startswith('}'):
            in_fn = False
            continue
        if not in_fn or not l.startswith('\t'):
            continue
        spoglia = l.rstrip()
        if not spoglia or spoglia.lstrip().startswith(('/*', '*', '//', '#')):
            continue
        if RX_DECL.match(spoglia) and not spoglia.lstrip().startswith('static const'):
            if visto_stmt:
                errs.append(f"{path}:{n}: dichiarazione dopo statement (C90): "
                            f"{spoglia.strip()[:52]!r}")
        elif RX_STMT.match(spoglia):
            visto_stmt = True
    return errs


def spoglia_commenti_e_letterali(src):
    """sostituisce commenti e letterali con spazi, tenendo i newline: cosi' i
    numeri di riga restano quelli veri e un identificatore citato nella prosa
    di un commento non conta come uso."""
    out = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
            continue
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
            continue
        if src[i] in '"\'':
            q, j = src[i], i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
            continue
        out.append(src[i])
        i += 1
    return ''.join(out)


# Dichiarazione a livello di file: si prende la parte di riga PRIMA del
# terminatore e l'ultimo identificatore che c'e' dentro. Tokenizzare invece di
# scrivere una regex per il nome evita di sbagliare su
# `static struct module *target_mod;`, dove una regex ingenua cattura 'module'.
RX_STATIC = re.compile(r'^static\b(.*)$', re.M)
RX_ID = re.compile(r'\b[A-Za-z_]\w*\b')
TIPO_TOKEN = re.compile(r'^(?:' + TIPI + r'|\w+_t|__\w+)$')


def _nome_dichiarato(testo):
    """l'ultimo identificatore prima di ; = ( [ , cioe' il nome dichiarato"""
    taglio = len(testo)
    for c in ';=([,':
        k = testo.find(c)
        if 0 <= k < taglio:
            taglio = k
    ids = RX_ID.findall(testo[:taglio])
    if not ids:
        return None
    nome = ids[-1]
    # `static DEFINE_RAW_SPINLOCK(x)` e simili: il nome vero lo fa la macro, e
    # non si sa quale sia. Meglio nessuna dichiarazione che una sbagliata.
    if TIPO_TOKEN.match(nome) or nome.isupper():
        return None
    return nome


def check_ordine(path):
    """uso di un simbolo di file prima della sua dichiarazione.

    In C non c'e' forward reference implicito: una funzione che nomina una
    variabile o una funzione statica dichiarata piu' in basso non compila
    ('undeclared identifier'), ed e' facilissimo farlo spostando codice in
    blocchi durante un refactor. Il compilatore lo dice chiaro, ma solo dopo un
    ciclo di cross-build."""
    src = spoglia_commenti_e_letterali(open(path, errors='replace').read())
    righe = src.split('\n')
    off = []
    k = 0
    for r in righe:
        off.append(k)
        k += len(r) + 1

    def riga_di(pos):
        lo, hi = 0, len(off) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if off[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    decl = {}
    for m in RX_STATIC.finditer(src):
        nome = _nome_dichiarato(m.group(1))
        if nome and nome not in decl:
            decl[nome] = m.start() + m.group(0).index(nome)

    errs = []
    for nome, pos_decl in decl.items():
        rx = re.compile(r'\b' + re.escape(nome) + r'\b')
        for m in rx.finditer(src):
            if m.start() >= pos_decl:
                break
            riga = righe[riga_di(m.start()) - 1]
            col = m.start() - off[riga_di(m.start()) - 1]
            prefisso = riga[:col]
            prima = prefisso.rstrip()
            # accesso a membro: e' un campo omonimo, non il simbolo di file
            if prima.endswith('.') or prima.endswith('->'):
                continue
            # dichiarazione locale, campo di struct o parametro con lo stesso
            # nome: il token precedente e' un tipo. Anche questo e' shadowing,
            # non un uso anticipato.
            tok = RX_ID.findall(prima)
            separato = prefisso != prima or prima.endswith('*')
            if separato and tok and TIPO_TOKEN.match(tok[-1]):
                continue
            errs.append(f"{path}:{riga_di(m.start())}: '{nome}' usato prima "
                        f"della dichiarazione (riga {riga_di(pos_decl)})")
            break
    return sorted(errs)


def main():
    bad = 0
    for p in sys.argv[1:]:
        e = check(p) + check_c90(p) + check_ordine(p)
        if e:
            bad += 1
            for x in e:
                print(x)
        else:
            print(f"{p}: ok")
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
