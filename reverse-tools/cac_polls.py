#!/usr/bin/env python3
"""CAC poll schedule of a capture.

On a channel whose availability check has not completed the stock driver runs
a poll of PHY 0x0251 and 0x0252, from the host-flag clear that closes the
bring-up to the end of the segment. The discrimination is total: 134 turns on
eighteen of the nineteen cold segments above 5250 MHz and 108 on cold15, zero
on the seven below, and zero on every hot segment whatever its channel --
there the check had long completed.

Il poll e' il flusso, non un contesto a parte: su cold05, cold06, cold07 e
cold08 i turni cadono alle stesse posizioni rispetto all'arming -- +2, +134,
+210, +214, +218, +222, +226 -- e 112 dei 133 intervalli distano quattro
operazioni, cioe' fra un turno e l'altro non succede nient'altro. I 151.5 ms
che separano i turni sono quel ciclo che dorme. Quanti turni ci stiano dipende
pero' da quanto dura la finestra catturata, che il driver non sa, quindi il
conto esce dalla cattura come AC_PROBE_TICKS e AC_BEACON_RELOADS. La
collocazione e' posizionale e non temporale: i turni si contano fra
consecutive reads of SHM 0x07da, the statistics poll that opens every turn of
the watchdog. That marker is used because the port emits it identically --
twenty-one of them on every segment of the sweep, whatever the channel -- so
the buckets line up with the port's emission points without a tick grid. A
grid built from probe_schedule.py's timestamps does not: its tick boundaries
are not where the port's bodies fall, and the turns land a tick off.

The first marker opens the bring-up and not a watchdog turn, so there are no
turns before it; those between it and the second are the pre-phase ones. The
first of all is not reported, because the driver emits it with the arming MOD
of 0x02e4 that precedes it.

  ./cac_polls.py <capture> [--sh]

--sh emits `AC_CAC_POLLS=<pre>:<n>,<n>,..`, ready to prefix a command, and
nothing at all when the capture has no turns -- which is the common case and
not an error.
"""

import argparse
import re

OP = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+(\S+)\s+addr=(0x[0-9a-f]+)")
POLL = ("PHY.RD", 0x251)
MARK = ("OBJ.RD", 0x7da)


def buckets(path):
    """Turns between consecutive markers: one entry per marker, plus a tail."""
    out, cur = [], 0
    for line in open(path, errors="replace"):
        m = OP.match(line)
        if not m:
            continue
        k = (m.group(1), int(m.group(2), 16))
        if k == POLL:
            cur += 1
        elif k == MARK:
            out.append(cur)
            cur = 0
    out.append(cur)
    return out


def schedule(path):
    """(pre, [turns per tick]) -- empty when the capture has no turns."""
    b = buckets(path)
    if sum(b) == 0:
        return 0, []
    # b[0] precede il primo marcatore, che apre il bring-up e non un giro di
    # watchdog: la' non ci sono turni. b[1] sono quelli prima della fase, meno
    # quello che emette l'arming. Il resto e' un bucket per corpo di tick.
    pre = max(0, b[1] - 1) if len(b) > 1 else 0
    per = b[2:]
    while per and per[-1] == 0:
        per.pop()
    return pre, per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--sh", action="store_true",
                    help="emit a shell assignment instead of a report")
    a = ap.parse_args()

    pre, per = schedule(a.capture)

    if a.sh:
        if pre or per:
            print(f"AC_CAC_POLLS={pre}:" + ",".join(str(n) for n in per))
    else:
        n = 1 + pre + sum(per) if (pre or per) else 0
        print(f"turns    {n}")
        print(f"pre      {pre} before the phase starts, plus the armed one")
        print(f"ticks    {per if per else 'none'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
