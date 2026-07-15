#!/usr/bin/env python3
"""Pulizia minima di un trace wl-diag: ripiega la sola RMW-read di ogni MOD.

Il tracer logga un read-modify-write come due op: la `PHY.RD`/`RAD.RD` che
`phy_reg_mod`/`radio_reg_mod` fa internamente per leggere il valore corrente,
seguita dal `PHY.MOD`/`RAD.MOD` che riscrive. Quella RD e' puro artefatto di
strumentazione: non ha effetto, e il driver portato puo' emetterla o no a
seconda di come e' scritto (peek esplicito vs mask helper). In un confronto
posizionale (compare.py) diventa un falso mismatch.

Questo tool fa UNA cosa sola: rimuove ogni `<fam>.RD addr=A` immediatamente
seguita da `<fam>.MOD addr=A` (stessa famiglia PHY/RAD/MAC, stesso indirizzo),
cioe' la lettura interna di quel MOD. Tiene TUTTO il resto — table-data,
write-gate, WR piene, RD standalone (peek non seguito da un MOD sullo stesso
addr). A differenza di collapse_trace.py NON nasconde il meccanismo tabella ne'
i valori: cosi' le divergenze vere (una table-write che il vendor non fa, un
valore board-specific) restano visibili invece di sparire.

Flusso: ripiegare con questo script sia la cattura vendor sia l'output
dell'harness, poi darli a compare.py. Accetta i due formati — cattura vendor
("<time> #<ep> cpu<n> OP ...") e output bare dell'harness ("cpu<n> OP ...") —
preservando l'eventuale #ep per il --range.
Uso: python3 fold_mod_reads.py <trace.txt> <out.txt>
"""
import re, sys

SRC, OUT = sys.argv[1], sys.argv[2]
line = re.compile(r'^(\s*(?:[\d.]+\s+#\d+\s+)?cpu\d+\s+)(\S+)\s+(.*)$')


def parse(ln):
    """Ritorna (fam, kind, addr) per una riga op, oppure None se non e' un'op.
    fam = 'PHY'/'RAD'/'MAC'..., kind = 'RD'/'MOD'/'WR'..., addr = int|None."""
    m = line.match(ln.rstrip('\n'))
    if not m:
        return None
    op = m.group(2)
    if '.' not in op:
        return (op, '', None)
    fam, kind = op.split('.', 1)
    kv = dict(re.findall(r'(\w+)=(\S+)', m.group(3)))
    addr = None
    if 'addr' in kv:
        try:
            addr = int(kv['addr'], 16)
        except ValueError:
            addr = None
    return (fam, kind, addr)


raw = open(SRC).readlines()
kept = []
dropped = 0
i = 0
while i < len(raw):
    cur = parse(raw[i])
    nxt = parse(raw[i + 1]) if i + 1 < len(raw) else None
    if (cur and nxt and cur[1] == 'RD' and nxt[1] == 'MOD'
            and cur[0] == nxt[0] and cur[2] is not None
            and cur[2] == nxt[2]):
        # RD interna del MOD che segue: la ripieghiamo (droppiamo la RD).
        dropped += 1
        i += 1
        continue
    kept.append(raw[i].rstrip('\n'))
    i += 1

with open(OUT, 'w') as f:
    f.write(f"# {SRC} - RMW READS FOLDED ({dropped} RD ripiegate)\n")
    f.write('\n'.join(kept) + '\n')
print(f"{OUT}: tenute {len(kept)}, ripiegate {dropped}")
