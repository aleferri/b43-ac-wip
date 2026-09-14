#!/usr/bin/env python3
"""I giri della fase probe su cui il watchdog del vendor non ha latchato.

Il problema che risolve: l'harness emetteva un latch della finestra statistiche
per ogni tick della fase, e i tick li conta `probe_schedule.py` dalla cadenza
dei gruppi probe. Le due cose coincidono solo se il timer non ha perso colpi, e
su cinque dei ventisei segmenti a freddo li ha persi -- divari fino a 2.6 s fra
un latch e il successivo dove la cadenza e' 1.004, e nella stessa finestra i
gruppi probe a 1.31 s invece di 1.004: la macchina carica, non una scelta del
driver. Il port emetteva percio' letture della finestra che il vendor non ha.

Manca il solo latch e non il giro. Il conto lo dice: sul segmento peggiore le
op di troppo del port erano sette letture per ogni cella di 0x0308-0x0314 piu'
0x008c, e nient'altro -- niente spazzata dei contatori, niente blocco di
misura, niente cambio di modo. Un giro saltato porterebbe via anche quelli.

Cosa si conta. Il latch e' la lettura di `OBJ.RD 0x0314`: e' l'ultima cella
della finestra e la spazzata di clear non la tocca, che si ferma a 0x0312. Si
guardano i latch dal primo gruppo probe in avanti -- quelli prima sono il latch
a se' che `stats_latch_and_crs()` emette sotto i 5250 MHz, e non sono un giro
della fase.

Come si passa da tempi a ordinali di giro. Ogni latch va sullo slot
`round((t - t0) / tick)`, con due vincoli: gli slot crescono, e quelli rimasti
bastano per i latch rimasti. Senza il primo, due latch a 2 ms l'uno dall'altro
-- un risveglio in ritardo seguito dal recupero -- finirebbero sullo stesso
slot e conterebbero un salto che non c'e'. Senza il secondo, un ritardo in coda
spingerebbe l'ultimo latch oltre la scadenza. Gli slot rimasti vuoti sono i
giri senza latch.

Il controllo che dice se si legge bene sta nei dati: sui ventuno segmenti dove
il timer e' regolare il numero di latch viene uguale alla scadenza e la lista
viene vuota.

Il latch e la spazzata non si perdono sugli stessi giri, e lo strumento li
misura tutti e due. Gatato e' solo il latch: la spazzata la si e' provata e non
paga -- su cold17 le due liste sono sette giri e sei, e togliere anche la
spazzata fa scendere le op di troppo di dodici e salire le mancanti di trenta,
perche' quelle scritture il vendor le emette da un'altra parte. La lista della
spazzata resta percio' in `--check`, come misura del residuo, e fuori da
`--sh`.

  ./watchdog_turns.py <cattura> [--sh] [--check]

--sh emette `AC_WD_NOLATCH=..`, pronto da mettere davanti a un comando; niente
se la lista e' vuota.
--check stampa scadenza e le due liste.
Lo stato di uscita e' 1 se la cattura non ha una fase probe, cosi' il chiamante
puo' ricadere sul default dell'harness.
"""

import argparse
import re
import statistics
import sys

GROUP = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+PHY\.RD\s+addr=0x0*7af\b")
LATCH = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+OBJ\.RD\s+addr=0x0*314\b")
CLEAR = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+OBJ\.WR\s+addr=0x0*312\b")


def collect(path):
    groups, latches, clears = [], [], []
    for line in open(path, errors="replace"):
        m = GROUP.match(line)
        if m:
            groups.append((float(m.group(1)), int(m.group(2))))
            continue
        m = LATCH.match(line)
        if m:
            latches.append((float(m.group(1)), int(m.group(2))))
            continue
        m = CLEAR.match(line)
        if m:
            clears.append((float(m.group(1)), int(m.group(2))))
    return groups, latches, clears


def missing_slots(events, groups, tick, deadline):
    """Gli ordinali di giro su cui @events non ha un'occorrenza.

    Ogni occorrenza va sullo slot round((t - t0) / tick), con due vincoli: gli
    slot crescono, e quelli rimasti bastano per le occorrenze rimaste. Senza il
    primo, due occorrenze a 2 ms l'una dall'altra -- un risveglio in ritardo
    seguito dal recupero -- finirebbero sullo stesso slot e conterebbero un
    salto che non c'e'. Senza il secondo, un ritardo in coda spingerebbe
    l'ultima oltre la scadenza.
    """
    inside = [t for t, n in events if n >= groups[0][1]]
    if not inside:
        return None

    t0 = inside[0]
    used = set()
    prev = -1
    for i, t in enumerate(inside):
        slot = round((t - t0) / tick)
        slot = max(slot, prev + 1)
        slot = min(slot, deadline - 1 - (len(inside) - 1 - i))
        used.add(slot)
        prev = slot

    return [k for k in range(deadline) if k not in used]


def schedule(groups, latches, clears):
    """(scadenza, giri senza latch, giri senza spazzata) o None."""
    if len(groups) < 3:
        return None

    times = [t for t, _ in groups]
    # Stessa lunghezza del tick e stesso costo del primo gruppo di
    # probe_schedule.py: la mediana assorbe i risvegli in ritardo.
    tick = statistics.median(b - a for a, b in zip(times[1:], times[2:]))
    first_cost = max(1, round((times[1] - times[0]) / tick))
    deadline = first_cost + len(groups) - 2 + 1

    nolatch = missing_slots(latches, groups, tick, deadline)
    noclear = missing_slots(clears, groups, tick, deadline)
    if nolatch is None:
        return None

    return deadline, nolatch, noclear or []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--sh", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    groups, latches, clears = collect(args.capture)
    got = schedule(groups, latches, clears)
    if got is None:
        return 1
    deadline, nolatch, noclear = got

    def join(v):
        return ",".join(str(k) for k in v)

    if args.sh:
        # Solo il latch: vedi in testa perche' la spazzata resta misurata e
        # non gatata.
        if nolatch:
            print("AC_WD_NOLATCH=" + join(nolatch))
    elif args.check:
        print(f"scadenza {deadline}, senza latch {len(nolatch)}: "
              f"{nolatch or '-'}, senza spazzata {len(noclear)}: "
              f"{noclear or '-'}")
    else:
        print(f"{join(nolatch)} / {join(noclear)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
