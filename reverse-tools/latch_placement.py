#!/usr/bin/env python3
"""Dove cade il latch della finestra statistiche rispetto alla fase probe.

Il latch e il blocco E stanno fra la cella a 0xffff e la fase probe, oppure
dentro il primo giro della fase. Quale dei due non e' una proprieta' del canale
ne' dello stato del driver: e' se un tick del periodico e' caduto in mezzo
mentre l'attach finiva. Due corse identiche del driver danno risposte diverse a
seconda di quanto ha impiegato l'hardware, quindi il dato esce dalla cattura
come AC_BEACON_RELOADS e la timeline di timeline.py.

Il riconoscitore e' posizionale e usa tre marcatori che il port emette
identici:

  cella   OBJ.WR 0x0026 = 0xffff, una volta sola in tutto il segmento e in
          posizione invariante -- e' l'ancora da cui si conta.
  tick    la testa di un giro del periodico: MAC.MCTRL seguito da
          PHY.RD 0x07af, 0x07b3, 0x07ab.
  latch   OBJ.RD 0x008c, la prima op del blocco contatori che apre il latch.

Se il latch viene prima del tick, il tick non e' caduto in mezzo e i pezzi
stanno prima della fase; se viene dopo, ci e' caduto e stanno dentro il primo
giro. Sullo sweep a freddo la discriminazione e' totale: cold01 ha il latch a
+96 dalla cella e il tick a +5, tutti gli altri segmenti che arrivano alla fase
hanno il latch a +1 e il tick molto piu' avanti; a caldo il latch e' a +1 e il
tick a +31 su ogni segmento.

Sui segmenti che alla fase non arrivano -- i DFS fermi nell'attesa del
controllo di disponibilita' -- non c'e' nessun tick e la domanda non si pone:
il tool non stampa niente, che non e' un errore.

  ./latch_placement.py <cattura> [--sh]

--sh stampa `AC_LATCH_IN_PHASE=1` quando il latch cade dentro la fase, e niente
quando cade prima, che e' il caso comune e il default dell'harness.
"""

import argparse
import re

OP = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+(.*)$")
CELLA = "OBJ.WR   addr=0x0026 val=0xffff"
LATCH = "OBJ.RD   addr=0x008c"


def leggi(path):
    ops = []
    with open(path, errors="replace") as f:
        for line in f:
            m = OP.match(line)
            if m:
                ops.append(m.group(1).rstrip())
    return ops


def testa_di_tick(ops, i):
    return (ops[i].startswith("MAC.MCTRL")
            and "addr=0x07af" in ops[i + 1]
            and "addr=0x07b3" in ops[i + 2]
            and "addr=0x07ab" in ops[i + 3])


def posizioni(path):
    """(cella, tick, latch) come indici, None dove il marcatore non c'e'."""
    ops = leggi(path)
    cella = next((i for i, o in enumerate(ops) if o.startswith(CELLA)), None)
    if cella is None:
        return None, None, None
    tick = next((i for i in range(cella, len(ops) - 3)
                 if testa_di_tick(ops, i)), None)
    latch = next((i for i in range(cella, len(ops))
                  if ops[i].startswith(LATCH)), None)
    return cella, tick, latch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cattura")
    ap.add_argument("--sh", action="store_true",
                    help="stampa l'assegnamento pronto da mettere in testa a un comando")
    args = ap.parse_args()

    cella, tick, latch = posizioni(args.cattura)
    if cella is None or tick is None or latch is None:
        # Nessuna fase probe nel segmento: la domanda non si pone.
        if not args.sh:
            print("nessuna fase probe nel segmento")
        return

    dentro = latch > tick
    if args.sh:
        if dentro:
            print("AC_LATCH_IN_PHASE=1")
    else:
        print(f"cella   @{cella}")
        print(f"tick    +{tick - cella}")
        print(f"latch   +{latch - cella}")
        print("il latch cade " + ("dentro la fase" if dentro
                                  else "prima della fase"))


main()
