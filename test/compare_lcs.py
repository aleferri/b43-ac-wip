#!/usr/bin/env python3
"""Confronta una cattura col trace di ac_trace per SOTTOSEQUENZA COMUNE.

Complementare a `compare.py`, che confronta POSIZIONE per posizione. La
differenza non e' cosmetica e va capita prima di citare un numero:

  compare.py      il port deve emettere la stessa sequenza, nello stesso
                  ordine, senza op in piu' ne' in meno. Un'op inserita sfasa
                  tutto il resto: su flow1 conta 1017 disallineamenti dove
                  questo strumento ne conta 15.
  compare_lcs.py  quanto del vendor e' riprodotto in ordine, tollerando
                  inserimenti e cancellazioni. Dice "il 99.94% della sequenza
                  c'e'", non "e' identica".

Entrambe sono legittime, misurano cose diverse, e i numeri NON sono
confrontabili. Le percentuali citate nei documenti di questo repo vengono da
qui, salvo dove indicato.

Uso:
    compare_lcs.py <vendor.txt> <test.out> [--range LO:HI] [--regioni N]

Nota di provenienza: ricostruzione di uno script di lavoro non versionato,
verificata contro i numeri che quello produceva sui due gate del d6220. Torna:
flow2 da' 21000/21007 identico, flow1 25002/25013 contro 24998 -- quattro op di
differenza, che sono le quattro MAC.MCTRL aggiunte a phy_ac.c dopo che quei
numeri erano stati presi.

Le normalizzazioni sono tutte ricavate dai dati e non assunte; quella sul
rendering di AND/OR viene da 163 coppie allineate in cui il valore coincide
sempre e la maschera del port e' sempre zero.
"""
import argparse
import difflib
import re
import signal
import sys

signal.signal(signal.SIGPIPE, signal.SIG_DFL)

# riga della cattura: timestamp, #episodio, cpuN, poi l'op e i suoi argomenti
RX_VENDOR = re.compile(r'\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(.*)$')
# riga di ac_trace: prefisso cpuN e poi l'op. Il cpuN va tolto da ENTRAMBI i
# lati: l'harness non simula lo scheduling, quindi su quale core sia finita
# un'operazione non e' comportamento.
RX_TEST = re.compile(r'\s*(?:cpu\d+\s+)?(\S+\s+.*)$')


def norm(s):
    """Riduce una riga a cio' che e' comportamento.

    Tre cose vanno via, e ognuna per una ragione diversa:
      - i riferimenti #N ai numeri di sequenza, che differiscono fra due
        catture della stessa cosa;
      - la LARGHEZZA dei valori esadecimali: la cattura stampa val=0x00000000
        dove l'harness stampa val=0x0000, ed e' lo stesso valore;
      - i campi che solo la cattura ha (`ret=`, `a5=`, `a6=`): sono il valore di
        ritorno e gli argomenti su stack, che l'harness non emette.
    """
    s = re.sub(r'for=#\d+', 'for=#N', s.strip())
    # Lo stesso effetto con accessor diversi. Il vendor fa PHY.AND e PHY.OR
    # separate dove il port fa una PHY.MOD mascherata: nella cattura di flow1
    # sono 93+70 da un lato e 163 dall'altro, cioe' esattamente le stesse.
    # E GPIO.OE / GPIO.OUTEN sono due nomi per lo stesso registro.
    s = re.sub(r'^PHY\.(?:AND|OR|MOD)\b', 'PHY.RMW', s)
    # Il vendor rende AND e OR con una parentesi -- val=0x4000 (set 0x4000) --
    # dove l'harness scrive mask=0x0 con lo stesso valore. Ricavato dai dati:
    # su 163 coppie allineate il valore coincide sempre e la maschera del port
    # e' sempre 0, 93 volte con (clr) e 70 con (set). Le PHY.MOD vere hanno
    # gia' la stessa forma da entrambe le parti e non vengono toccate.
    s = re.sub(r' \((?:set|clr) 0x[0-9a-f]+\)$', ' mask=0x0', s)
    s = re.sub(r'^GPIO\.(?:OE|OUTEN)\b', 'GPIO.OE', s)
    s = re.sub(r'\s+(?:ret|a5|a6)=\S+', '', s)
    s = re.sub(r'0x0*([0-9a-f]+)', lambda m: '0x' + m.group(1), s)
    return re.sub(r'\s+', ' ', s)


# Classi che l'harness non simula affatto: l'accesso ai registri di core del bus
# e il PLL del PMU sono territorio bcma/ssb, fuori dal PHY. Confrontarle darebbe
# divergenze che non riguardano il codice sotto test.
ESCLUSE = ('SI.COREREG', 'PMU.PLL')


def leggi_vendor(path, lo, hi):
    out = []
    for line in open(path, errors='replace'):
        m = RX_VENDOR.match(line)
        if not m:
            continue
        ep = int(m.group(1))
        if lo is not None and not (lo <= ep <= hi):
            continue
        corpo = m.group(2)
        if corpo.split()[0] in ESCLUSE:
            continue
        out.append(norm(corpo))
    return out


def leggi_test(path):
    out = []
    for line in open(path, errors='replace'):
        m = RX_TEST.match(line)
        if not m:
            continue
        s = norm(m.group(1))
        if s and s.split()[0] not in ESCLUSE:
            out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vendor')
    ap.add_argument('test')
    ap.add_argument('--range', dest='rng')
    ap.add_argument('--regioni', type=int, default=0,
                    help='stampa le prime N regioni divergenti')
    args = ap.parse_args()

    lo = hi = None
    if args.rng:
        lo, hi = (int(x) for x in args.rng.split(':'))

    a = leggi_vendor(args.vendor, lo, hi)
    b = leggi_test(args.test)
    if not a:
        sys.exit("nessuna riga vendor nell'intervallo")

    sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
    comune = sum(n for _, _, n in sm.get_matching_blocks())

    regioni = [op for op in sm.get_opcodes() if op[0] != 'equal']
    divergenti = sum(max(op[2] - op[1], op[4] - op[3]) for op in regioni)

    print(f"{comune}/{len(a)} = {100.0 * comune / len(a):.2f}%   "
          f"{divergenti} op divergenti in {len(regioni)} regioni")

    for tag, i1, i2, j1, j2 in regioni[:args.regioni]:
        print(f"\n  {tag} vendor[{i1}:{i2}] test[{j1}:{j2}]")
        for x in a[i1:i2][:4]:
            print(f"     vendor: {x[:70]}")
        for x in b[j1:j2][:4]:
            print(f"     test:   {x[:70]}")


if __name__ == '__main__':
    main()
