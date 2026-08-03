#!/usr/bin/env python3
"""Controllo lessicale minimo su un file C, senza compilatore.

Prende la classe di errori che si fanno editando commenti a mano e che il
compilatore segnala in modo confuso, tipicamente decine di righe dopo la causa:

  - `*/` dentro la prosa di un commento, che lo chiude in anticipo
    (`hndotp_*/ipxotp_*` e' il caso reale che ha rotto la build)
  - commento aperto e non chiuso
  - letterale di stringa o carattere non terminato sulla riga
  - graffe, tonde, quadre non bilanciate

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


def main():
    bad = 0
    for p in sys.argv[1:]:
        e = check(p) + check_c90(p)
        if e:
            bad += 1
            for x in e:
                print(x)
        else:
            print(f"{p}: ok")
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
