#!/usr/bin/env python3
"""Beacon-reload schedule of a capture, read off its timestamps.

Every so often the stack above the driver republishes the beacon, and the
driver reloads the template: 0x00cc read and echoed, TIMBPOS, the template
word in template RAM, the length in BTL0 or BTL1, then a pass over the
probe-response PLCP. The length is not a constant: it is 0x012a at 20 MHz and
something else at 40 and 80, so only the cell is matched here and not its
value. `wl` alternates beacon0 and beacon1, starting on beacon0.

How many times it happens is not the driver's to decide, and the captures show
it: on the 26 cold segments of one board the count runs from 7 to 21, on the
same tree, with no relation to segment length -- they all sit between 33 and 39
seconds. It is the number of times hostapd pushed a beacon while the tracer was
running. So it goes the same way AC_PROBE_TICKS and AC_WATCHDOG_TICKS go: out of
the capture and in through the caller, because the harness has no stack above
it.

Which reloads are these is a structural question, not a positional one. The two
anchored to b43_op_config() and the three of the conf_tx passes carry no
mac_suspend between the length and the template; the later ones do, and that is
what this looks for.

Placement reuses probe_schedule.py's tick grid, so a reload lands on the tick of
the probe group that follows it. Reloads before the phase starts are counted
separately: they run before the loop, not on a tick of it.

  ./beacon_reloads.py <capture> [--sh]

--sh emits `AC_BEACON_RELOADS=<pre>:<tick>,<tick>,..`, ready to prefix a
command. Exit status is 1 when the capture has no probe phase,
since without the grid there is nothing to place the reloads on.
"""

import argparse
import importlib.util
import os
import re
import statistics
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "probe_schedule", os.path.join(_HERE, "probe_schedule.py"))
probe_schedule = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe_schedule)

OP = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(.*?)\s*$")
TIMBPOS = re.compile(r"^OBJ\.WR\s+addr=0x001e\s+val=0x[0-9a-f]+(?: |$)")
# Le quattro passate conf_tx nell'ordine in cui l'harness le emette, ognuna
# riconosciuta dall'ultima cella del suo blocco di coda.
EDCF_LAST = [0x027e, 0x025e, 0x029e, 0x02be]
BTL = re.compile(r"^OBJ\.WR\s+addr=0x00(18|1a)\s+val=0x[0-9a-f]+(?: |$)")
SUSPEND = "MAC.MCTRL val=0x00000000 mask=0x00000001"


def collect(path):
    """Timestamps of the reloads that carry the internal mac_suspend."""
    ops = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if m:
            ops.append((float(m.group(1)), m.group(3)))
    out = []
    for i, (t, op) in enumerate(ops):
        m = BTL.match(op)
        if not m:
            continue
        if i + 1 < len(ops) and ops[i + 1][1].startswith(SUSPEND):
            out.append((t, m.group(1)))
    return out


def edcf_mask(path):
    """Su quali passate conf_tx cade una ricarica del beacon.

    Non e' strutturale: su cold01 solo la prima la porta, su cold05 tutte e
    quattro, e i quattro blocchi di coda ci sono in entrambi i casi -- nel
    vendor come nel port. Quindi e' hostapd che ha spinto un beacon li', ed e'
    un ingresso come il numero dei tick. Il criterio e' posizionale: una
    scrittura di TIMBPOS entro poche op dalla fine del blocco della passata.
    """
    ops = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if m:
            ops.append(m.group(3))
    mask = 0
    for n, last in enumerate(EDCF_LAST):
        pat = "OBJ.WR   addr=0x%04x" % last
        for i, op in enumerate(ops):
            if not op.startswith(pat):
                continue
            if any(TIMBPOS.match(x) for x in ops[i + 1:i + 8]):
                mask |= 1 << n
            break
    return mask


def pre_late(path):
    """Quante delle pre-fase cadono dopo la cella a 0xffff.

    Il confine e' la cella e non l'inizio della fase: su cold04 una delle tre
    sta dopo il latch e il blocco E, e contarla con le altre sfasa il flusso di
    una ricarica intera. Sui 26 segmenti a freddo e' zero ovunque tranne li'.
    """
    ops = []
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if m:
            ops.append(m.group(3))
    cell = [i for i, op in enumerate(ops)
            if op.startswith("OBJ.WR   addr=0x0026 val=0xffff")]
    group = [i for i, op in enumerate(ops)
             if op.startswith("PHY.RD   addr=0x0527")]
    if not cell:
        return 0
    lo = cell[0]
    hi = group[0] if group else len(ops)
    return sum(1 for i, op in enumerate(ops)
               if lo < i < hi and TIMBPOS.match(op))


MODE_CHANGE = re.compile(
    r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+PHY\.MOD\s+addr=0x0*520\b.*"
    r"mask=0x0*c\b")


def mode_changes(path):
    """Il tempo di ogni cambio di modo, uno per corpo di giro."""
    out = []
    for line in open(path):
        m = MODE_CHANGE.match(line)
        if m:
            out.append(float(m.group(1)))
    return out


def schedule(path):
    """[tick per reload] or None with no probe phase.

    Il tick di una ricarica e' quanti corpi di giro la precedono, contati; non
    quanto tempo e' passato, diviso per la durata di un tick.

    Contare e' esatto per costruzione e dividere no, perche' il tick del vendor
    arriva in ritardo -- e' la stessa cosa che watchdog_turns.py registra come
    @probe_nolatch_tick, e su cold01 sono quattro giri su ventisei. Ogni
    ritardo sposta l'attribuzione di uno e l'errore si accumula: la formula a
    tempo dava `2,6,7,13,14,19,21,26,26` dove i corpi contati danno
    `2,6,7,12,13,18,19,24,25`, e le prime tre coincidono perche' il ritardo non
    era ancora arrivato. Alimentate a mano, le seconde portano cold01 da 92.08%
    a 92.74%.

    Il marcatore e' il cambio di modo su 0x0520, non il gruppo di
    probe_schedule.py: quello e' `PHY.RD 0x07af`, che su cold01 compare 24
    volte per 26 giri, mentre i cambi di modo sono 26, esattamente
    @probe_ticks. Un marcatore che salta dei giri non puo' numerarli.

    L'inizio della fase resta quello dei gruppi: una ricarica prima del primo
    gruppo non sta su un tick del ciclo.
    """
    groups, _ = probe_schedule.collect(path)
    if len(groups) < 3:
        return None
    times = [t for t, _ in groups]
    bodies = mode_changes(path)
    deadline = len(bodies)

    pre, ticks = 0, []
    for t, _ in collect(path):
        if t < times[0]:
            pre += 1               # prima che la fase parta: non su un tick
            continue
        ticks.append(min(deadline, sum(1 for b in bodies if b < t)))
    return pre, ticks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--sh", action="store_true",
                    help="emit a shell assignment instead of a report")
    a = ap.parse_args()

    got = schedule(a.capture)
    if got is None:
        print("no probe phase in this capture", file=sys.stderr)
        return 1
    pre, ticks = got

    mask = edcf_mask(a.capture)
    late = pre_late(a.capture)
    pre = max(0, pre - late)

    if a.sh:
        print(f"AC_EDCF_RELOADS={mask:#x}")
        if late:
            print(f"AC_BEACON_PRE_LATE={late}")
        print(f"AC_BEACON_RELOADS={pre}:" + ",".join(str(t) for t in ticks))
    else:
        print(f"conf_tx  mask {mask:#x}")
        print(f"reloads  {pre + len(ticks)} da programmare")
        print(f"pre      {pre} before the phase starts, {late} after the cell")
        print(f"ticks    {ticks if ticks else 'none'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
