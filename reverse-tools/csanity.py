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
import sys

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
            # la riga dopo la chiusura contiene ancora prosa?
            eol = src.find('\n', j + 2)
            rest = src[j + 2:eol if eol > 0 else n].strip()
            if rest and rest[0] not in '{};,)' and not rest.startswith('//'):
                errs.append(f"{path}:{line + src.count(chr(10), i, j)}: "
                            f"commento chiuso da un '*/' nella prosa, poi segue "
                            f"codice: {rest[:48]!r}")
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


def main():
    bad = 0
    for p in sys.argv[1:]:
        e = check(p)
        if e:
            bad += 1
            for x in e:
                print(x)
        else:
            print(f"{p}: ok")
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
