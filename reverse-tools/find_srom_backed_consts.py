#!/usr/bin/env python3
"""Find hardcoded driver constants that are really board data from the SROM.

A value transcribed from a capture and a value derived from the SROM look
identical in the source: both are integer literals. The difference shows up only
on a different board -- which is exactly when it is too late. This script looks
for the overlap ahead of time: every literal in the driver that equals a value
the reference board's SROM declares is a candidate for "this is board data, not
a magic number".

Two stages, because stage one alone is noisy:

  1. Value match. Small ubiquitous values (0x0001, 0x0002) collide with SROM
     fields by chance, so matches are ranked and the ones worth reading are the
     specific ones -- a value that occurs a handful of times in the driver and
     belongs to a named power/gain field.

  2. Canonical confirmation. Broadcom's own `bcmsrom_tbl.h` (field -> word +
     mask) and `bcmsrom_fmt.h` (word indices) are the authority. If a candidate
     value belongs to a field that exists there, the match is real; if the field
     does not exist, the value match was luck. Pass --canonical to enable.

Confirmed by this method so far:
  - `tssifloor5g[grp]`, hardcoded as 0x03ff at PHY 0x0724. The field is
    unprogrammed on the reference boards and reads 0x3ff, i.e. exactly the
    constant, so the transcription was invisible.

Usage:
    python3 find_srom_backed_consts.py [--nvram FILE] [--min-uses N]
                                       [--canonical bcmsrom_tbl.h]
"""
import argparse
import collections
import glob
import os
import re

DRIVER = 'src/phy_ac.c'

# Campi SROM che descrivono potenza / gain / front-end: un match qui vale piu'
# di un match su un campo amministrativo (boardnum, ccode, ...).
RF_HINT = re.compile(
    r'maxp|pa[0-9]|pdgain|epagain|tssi|elna|triso|trelnabyp|rxgain|noiselvl|'
    r'femctrl|papdcap|tworange|pdoffset|measpower|gainctrl|edthresh|agbg|aga',
    re.I)


def driver_constants(path):
    src = open(path, errors='replace').read()
    # i commenti contengono numeri di trace: non sono costanti del codice
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    c = collections.Counter()
    for m in re.finditer(r'\b0x([0-9a-fA-F]{2,4})\b', src):
        v = int(m.group(1), 16)
        if 0 < v <= 0xffff:
            c[v] += 1
    return c


def srom_values(path):
    """valore -> insieme di campi NVRAM che lo contengono"""
    out = collections.defaultdict(set)
    for line in open(path, errors='replace'):
        if '=' not in line:
            continue
        k, v = line.strip().split('=', 1)
        for tok in re.split(r'[,\s]+', v):
            tok = tok.strip()
            if not tok:
                continue
            try:
                n = int(tok, 16) if tok.lower().startswith('0x') else int(tok)
            except ValueError:
                continue
            if 0 < n <= 0xffff:
                out[n].add(k)
    return out


def canonical_fields(path):
    """nome campo -> (word symbol, mask) da bcmsrom_tbl.h"""
    out = {}
    for line in open(path, errors='replace'):
        m = re.match(r'\s*\{\s*"([a-z0-9_]+)"\s*,\s*\S+\s*,\s*[^,]+,\s*'
                     r'(SROM\w+)\s*,\s*(0x[0-9a-fA-F]+)', line)
        if m:
            out.setdefault(m.group(1), (m.group(2), m.group(3)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nvram')
    ap.add_argument('--min-uses', type=int, default=1)
    ap.add_argument('--max-uses', type=int, default=20,
                    help='sopra questa soglia il match e\' quasi certamente '
                         'casuale (default 20)')
    ap.add_argument('--canonical', help='percorso di bcmsrom_tbl.h')
    args = ap.parse_args()

    nvram = args.nvram
    if not nvram:
        cand = sorted(glob.glob('router-data/*/*nvram*'))
        if not cand:
            raise SystemExit('nessuna NVRAM trovata: passa --nvram')
        nvram = cand[0]
    if not os.path.exists(DRIVER):
        raise SystemExit(f'{DRIVER} non trovato: lancia dalla radice del repo')

    consts = driver_constants(DRIVER)
    srom = srom_values(nvram)
    canon = canonical_fields(args.canonical) if args.canonical else None

    print(f"driver: {len(consts)} costanti distinte")
    print(f"SROM:   {len(srom)} valori distinti da {nvram}")
    if canon:
        print(f"canonico: {len(canon)} campi da {args.canonical}")
    print()

    rows = []
    for v, n in consts.items():
        if v not in srom:
            continue
        if not (args.min_uses <= n <= args.max_uses):
            continue
        fields = sorted(srom[v])
        rf = [f for f in fields if RF_HINT.search(f)]
        if not rf:
            continue
        conf = ''
        if canon:
            known = [f for f in rf if re.sub(r'[0-9]$', '', f) in canon or f in canon]
            conf = 'canonico' if known else 'NON nel canonico'
        rows.append((len(rf), n, v, rf, conf))

    rows.sort(key=lambda t: (t[1], -t[0]))
    print("candidati: costanti che coincidono con un campo SROM di RF/potenza")
    print(f"{'valore':>8} {'usi':>4} {'conferma':>16}   campi")
    for _, n, v, rf, conf in rows:
        print(f"  0x{v:04x} {n:4} {conf:>16}   {', '.join(rf[:4])}"
              f"{' ...' if len(rf) > 4 else ''}")
    print()
    print("Come si legge: pochi usi + campo di potenza/gain = da verificare nel")
    print("codice. Molti usi = quasi certamente coincidenza. La conferma")
    print("canonica dice se il campo esiste davvero nel layout Broadcom.")


if __name__ == '__main__':
    main()
