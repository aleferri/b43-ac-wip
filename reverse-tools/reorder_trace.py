#!/usr/bin/env python3
"""Riordina una traccia wl-diag secondo l'ordine di una traccia di riferimento.

Serve quando due catture dello stesso flow vengono da versioni diverse del
driver OEM (es. agcombo 7.14 vs D6220 piu' recente, che e' l'ordine
replicato dall'harness): le fasi macro coincidono ma l'ordine locale delle
op differisce, e compare.py -- che e' posizionale -- deraglia al primo
blocco trasposto. Questo tool accoppia le op per chiave strutturale
(tipo + indirizzo, o id/off/len per le TBL; i VALORI non fanno parte
della chiave) in due stadi: prima uno scheletro monotono (ancore uniche
per lato filtrate con una LIS, sotto-segmenti risolti ricorsivamente e
per occorrenza), poi il recupero dei blocchi MACRO spostati: i run
contigui di residui del riferimento vengono cercati come sequenza di
chiavi ovunque nel vendor, senza vincolo di ordine -- e' il caso di fasi
che una versione del driver esegue altrove nel flow (es. a init invece
che a ogni set_channel). Con --replicate un blocco che il vendor esegue
una volta sola dove il riferimento lo ripete viene riemesso, marcato
';replicated' (compare.py scarta i commenti ';...'). Il tool emette:

  --out-vendor   le op vendor accoppiate, testo originale, nell'ordine del
                 riferimento
  --out-ref      le op del riferimento accoppiate, testo originale
  --res-vendor   op vendor senza partner (contenuto solo-vecchio-driver)
  --res-ref      op riferimento senza partner (contenuto solo-nuovo-driver)

I primi due file hanno la stessa lunghezza e vanno dati a compare.py.
La semantica del riordino: il MACRO ordine (blocchi/fasi permutati tra
versioni del driver) viene normalizzato perche' e' una scelta
architetturale lecita; il MICRO ordine dentro i blocchi viene
PRESERVATO com'e' nel vendor, perche' un'op locale fuori posto e' un
probabile bug o una differenza 4352/4360 e deve restare visibile: nel
diff appare come cluster di mismatch localizzato (uno swap = coppia di
mismatch adiacenti; una fase interna sfasata = tratto sfasato). I
mismatch di compare.py sono quindi due specie di segnale: valori
diversi e micro-ordine diverso. I residui restano ispezionabili a
parte.

Entrambi gli input vanno prima passati da collapse_trace.py: le op di
meccanismo del port tabelle dipendono da come il driver carica le tabelle
e accoppiarle non ha senso. Il tool avvisa se ne vede.

I poll (run consecutivi della stessa RD) vengono collassati a 1 su
entrambi i lati prima dell'accoppiamento: il numero di iterazioni dipende
dal timing hardware, non dal driver.

Uso:
    reorder_trace.py <reference> <vendor> --out-vendor F --out-ref F
                     [--res-vendor F] [--res-ref F]

<reference> accetta sia il formato harness ("cpuN OP ...") sia il formato
cattura ("<time> #<ep> cpuN OP ..."); <vendor> idem.
"""
import argparse
import bisect
import re
import sys
from collections import defaultdict, Counter

CAPTURE_LINE = re.compile(r'^\s*[0-9.]+\s+#\d+\s+cpu\d+\s+(.+?)\s*(?:;.*)?$')
BARE_LINE = re.compile(r'^cpu\d+\s+(.+?)\s*$')
PORT_ADDRS = ('addr=0x000d', 'addr=0x000e', 'addr=0x000f',
              'addr=0x0010', 'addr=0x0011')


def parse(path):
    """-> lista di (riga_originale, op_testo)."""
    out = []
    for raw in open(path):
        line = raw.rstrip('\n')
        m = CAPTURE_LINE.match(line) or BARE_LINE.match(line)
        if m:
            out.append((line, re.sub(r'\s+', ' ', m.group(1).strip())))
    return out


def coarse_key(op):
    """Chiave strutturale: tipo (con OR/AND foldate a MOD) + indirizzo.
    Le TBL usano id/off/len. I valori restano fuori dalla chiave."""
    if op.startswith('TBL.'):
        m = re.match(r'(TBL\.\S+)\s+(id=\S+ off=\S+ len=\S+)', op)
        return f'{m.group(1)} {m.group(2)}' if m else op
    m = re.match(r'(\S+)\s+(addr=\S+)', op)
    if m:
        typ = m.group(1)
        if typ in ('PHY.OR', 'PHY.AND'):
            typ = 'PHY.MOD'
        return f'{typ} {m.group(2)}'
    return op.split(' ')[0]        # MAC.MCTRL, PMU.*, GPIO...


def squash_polls(entries):
    out = []
    for e in entries:
        if out and '.RD ' in e[1] and \
           coarse_key(e[1]) == coarse_key(out[-1][1]):
            continue
        out.append(e)
    return out


def match_segment(RK, VK, r0, r1, v0, v1, depth=0):
    """Accoppia R[r0:r1] con V[v0:v1] per chiave strutturale.

    Le chiavi che compaiono UNA volta per lato nel segmento sono ancore
    affidabili; la LIS sulle loro posizioni vendor scarta le ancore
    mutuamente incoerenti, e i sotto-segmenti tra ancore consecutive
    vengono risolti ricorsivamente. Nel caso base (nessuna ancora)
    l'accoppiamento e' per occorrenza: k-esima con k-esima dentro il
    segmento -- a quel punto i segmenti sono piccoli e omogenei e
    l'ambiguita' delle op ripetute e' locale per costruzione.
    """
    if r0 >= r1 or v0 >= v1:
        return []
    cr = Counter(RK[r0:r1])
    cv = Counter(VK[v0:v1])
    uniq = {k for k in cr if cr[k] == 1 and cv.get(k) == 1}
    if uniq and depth < 12:
        rpos = {RK[j]: j for j in range(r0, r1) if RK[j] in uniq}
        vpos = {VK[i]: i for i in range(v0, v1) if VK[i] in uniq}
        anchors = sorted((rpos[k], vpos[k]) for k in uniq)
        seq = [v for _, v in anchors]
        tails, tidx, prev = [], [], [-1] * len(seq)
        for t, x in enumerate(seq):
            p = bisect.bisect_left(tails, x)
            if p == len(tails):
                tails.append(x)
                tidx.append(t)
            else:
                tails[p] = x
                tidx[p] = t
            prev[t] = tidx[p - 1] if p > 0 else -1
        keep = set()
        t = tidx[len(tails) - 1] if tails else -1
        while t != -1:
            keep.add(t)
            t = prev[t]
        anchors = [a for t, a in enumerate(anchors) if t in keep]
        if anchors:
            pairs = []
            pr, pv = r0, v0
            for j, i in anchors:
                pairs += match_segment(RK, VK, pr, j, pv, i, depth + 1)
                pairs.append((j, i))
                pr, pv = j + 1, i + 1
            pairs += match_segment(RK, VK, pr, r1, pv, v1, depth + 1)
            return pairs
    vocc = defaultdict(list)
    for i in range(v0, v1):
        vocc[VK[i]].append(i)
    ptr = defaultdict(int)
    pairs = []
    for j in range(r0, r1):
        k = ptr[RK[j]]
        if k < len(vocc[RK[j]]):
            pairs.append((j, vocc[RK[j]][k]))
            ptr[RK[j]] += 1
    return pairs


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('reference')
    ap.add_argument('vendor')
    ap.add_argument('--out-vendor', required=True)
    ap.add_argument('--out-ref', required=True)
    ap.add_argument('--res-vendor')
    ap.add_argument('--res-ref')
    ap.add_argument('--min-block', type=int, default=4,
                    help='lunghezza minima di un run residuo per il '
                         'recupero a blocchi dello stadio 2 (default 4)')
    ap.add_argument('--macro-gap', type=int, default=64,
                    help='salto massimo di indice vendor entro un blocco '
                         'macro: sotto e' + "'" + ' micro-riordino da '
                         'preservare nel diff, sopra e' + "'" + ' confine '
                         'di blocco (default 64)')
    ap.add_argument('--replicate', action='store_true',
                    help='riemetti (marcati ";replicated") blocchi vendor '
                         'gia' + "'" + ' consumati quando il riferimento li '
                         'ripete e il vendor li esegue una volta sola')
    args = ap.parse_args()

    ref = squash_polls(parse(args.reference))
    ven = squash_polls(parse(args.vendor))
    for name, seq in (('reference', ref), ('vendor', ven)):
        leak = sum(1 for _, op in seq
                   if op.startswith('PHY.') and
                      any(p in op for p in PORT_ADDRS))
        if leak:
            print(f"avviso: {leak} op di meccanismo tabelle in {name} -- "
                  f"passa l'input da collapse_trace.py", file=sys.stderr)

    RK = [coarse_key(op) for _, op in ref]
    VK = [coarse_key(op) for _, op in ven]
    pairs = sorted(match_segment(RK, VK, 0, len(ref), 0, len(ven)))
    stage1 = len(pairs)

    # Stadio 2 -- blocchi macro spostati. Lo stadio 1 e' vincolato alla
    # monotonia (le ancore passano per una LIS): un blocco che il vecchio
    # driver esegue in un'altra fase del flow (es. a init invece che a
    # ogni set_channel) la viola e resta nei residui di ENTRAMBI i lati.
    # Qui ogni run contiguo di residui ref viene cercato come sequenza di
    # chiavi contigua nel vendor, ovunque essa sia. Con --replicate, se la
    # sequenza esiste solo in regione gia' consumata (il vendor la esegue
    # una volta sola dove il riferimento la ripete), il blocco vendor
    # viene riemesso con marker ';replicated' -- compare.py scarta i
    # commenti ';...', quindi resta confrontabile ma la duplicazione e'
    # visibile nel file.
    matched_ref = {j for j, _ in pairs}
    matched_ven = {i for _, i in pairs}
    replicated = []                    # indici pairs con vendor riemesso
    runs = []
    j = 0
    while j < len(ref):
        if j in matched_ref:
            j += 1
            continue
        j0 = j
        while j < len(ref) and j not in matched_ref:
            j += 1
        runs.append((j0, j))
    moved = 0
    for j0, j1 in runs:
        if j1 - j0 < args.min_block:
            continue
        keyseq = RK[j0:j1]
        hit_free = hit_used = None
        for i0 in range(0, len(ven) - len(keyseq) + 1):
            if VK[i0:i0 + len(keyseq)] != keyseq:
                continue
            span = range(i0, i0 + len(keyseq))
            if all(i not in matched_ven for i in span):
                hit_free = i0
                break
            if hit_used is None:
                hit_used = i0
        if hit_free is not None:
            for t, jj in enumerate(range(j0, j1)):
                pairs.append((jj, hit_free + t))
                matched_ven.add(hit_free + t)
                matched_ref.add(jj)
            moved += j1 - j0
        elif hit_used is not None and args.replicate:
            for t, jj in enumerate(range(j0, j1)):
                pairs.append((jj, hit_used + t))
                matched_ref.add(jj)
                replicated.append((jj, hit_used + t))
            moved += j1 - j0
    pairs.sort()
    replicated = set(replicated)

    # Emissione. Il MACRO ordine viene normalizzato (blocchi vendor
    # permutati nell'ordine del riferimento: e' la scelta architetturale
    # lecita della versione di driver), il MICRO ordine NO: dentro ogni
    # blocco le op vendor escono nel loro ordine originale, cosi' uno
    # swap locale -- probabile bug o differenza 4352/4360 -- resta
    # visibile in compare.py come coppia di mismatch adiacenti invece di
    # venire silenziosamente riallineato. Un blocco e' un run di coppie
    # il cui indice vendor non salta piu' di --macro-gap.
    blocks = []
    cur = [pairs[0]] if pairs else []
    for (j1, i1), (j2, i2) in zip(pairs, pairs[1:]):
        if abs(i2 - (i1 + (j2 - j1))) > args.macro_gap:
            blocks.append(cur)
            cur = []
        cur.append((j2, i2))
    if cur:
        blocks.append(cur)
    micro = 0
    with open(args.out_vendor, 'w') as fv, open(args.out_ref, 'w') as fr:
        for blk in blocks:
            vord = sorted(blk, key=lambda p: p[1])
            micro += sum(1 for a, b in zip(blk, vord) if a != b)
            for (j, _), (_, i) in zip(blk, vord):
                tag = ' ;replicated' if (j, i) in replicated else ''
                fv.write(ven[i][0] + tag + '\n')
                fr.write(ref[j][0] + '\n')
    if args.res_vendor:
        with open(args.res_vendor, 'w') as f:
            for i, (line, _) in enumerate(ven):
                if i not in matched_ven:
                    f.write(line + '\n')
    if args.res_ref:
        with open(args.res_ref, 'w') as f:
            for j, (line, _) in enumerate(ref):
                if j not in matched_ref:
                    f.write(line + '\n')

    nv, nr = len(ven), len(ref)
    print(f"reference {nr} op, vendor {nv} op (post squash-poll)",
          file=sys.stderr)
    print(f"accoppiate {len(pairs)} "
          f"({len(pairs)/nr*100:.1f}% ref, {len(pairs)/nv*100:.1f}% vendor): "
          f"{stage1} monotone, {moved} in blocchi spostati"
          f" (di cui {len(replicated)} replicate); "
          f"blocchi macro {len(blocks)}; "
          f"micro-riordini preservati nel diff: {micro} op; "
          f"residui: ref {nr-len(matched_ref)}, "
          f"vendor {nv-len(matched_ven)}",
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
