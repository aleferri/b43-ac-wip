#!/usr/bin/env python3
"""Taglia una traccia decodificata sui record MARK, un file per etichetta.

I MARK li mette chi cattura (`echo "ch36 bw20" > /proc/wl_diag`) e wl_diag ai
bordi di ogni caricamento del bersaglio ("mod COMING" / "mod GOING"), quindi il
confine e' scritto nella traccia e non va indovinato. E' la differenza con
split_by_chanspec.py, che taglia sui salti temporali con soglia 1.03 s perche'
per uno sweep a caldo non c'e' altro: i record CHANSPEC arrivano in ritardo di
un ciclo e uno solo per fase.

Uso:
    python3 split_by_mark.py trace.txt split/
    python3 split_by_mark.py trace.txt split/ --prefix cold --skip-mod

Il segmento parte DAL marcatore, cosi' "mod COMING" e l'attach che segue stanno
nello stesso file dell'etichetta di canale che li precede. Quello che viene
prima del primo MARK va in `00-preambolo.txt` invece di essere buttato.
"""
import argparse
import os
import re
import sys

# MARK dal decoder:  <ts> #<seq> cpu0 MARK     'ch36 bw20'
RE_MARK = re.compile(r"\bMARK\s+'([^']*)'")
# I marcatori automatici del modulo non sono confini di ciclo: sono bordi di
# caricamento DENTRO un ciclo, e tagliarci sopra spezzerebbe l'attach dal
# canale che lo ha chiesto.
RE_MOD = re.compile(r"^mod (COMING|GOING)$")


def nome_file(n, etichetta):
    """etichetta -> nome di file, tenendo la convenzione -chN-bwN del repo"""
    m = re.match(r"^ch(\d+)\s+bw(\d+)$", etichetta.strip())
    if m:
        base = f"ch{m.group(1)}-bw{m.group(2)}"
    else:
        base = re.sub(r"[^A-Za-z0-9._-]+", "-", etichetta.strip()) or "senza-nome"
    return f"{n:02d}-{base}.txt"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("outdir")
    ap.add_argument("--prefix", default="", help="prefisso dei nomi di file")
    ap.add_argument("--skip-mod", action="store_true",
                    help="taglia anche sui MARK 'mod COMING'/'mod GOING'")
    a = ap.parse_args()

    os.makedirs(a.outdir, exist_ok=True)

    segmenti = []          # (nome, righe)
    corrente = []
    nome = "00-preambolo.txt"
    n = 0
    trovati = 0

    with open(a.trace, encoding="utf-8", errors="replace") as f:
        for riga in f:
            m = RE_MARK.search(riga)
            if m:
                trovati += 1
                etichetta = m.group(1)
                automatico = bool(RE_MOD.match(etichetta))
                if not automatico or a.skip_mod:
                    if corrente:
                        segmenti.append((nome, corrente))
                    n += 1
                    nome = nome_file(n, etichetta)
                    corrente = []
            corrente.append(riga)

    if corrente:
        segmenti.append((nome, corrente))

    if trovati == 0:
        print("nessun record MARK nella traccia.", file=sys.stderr)
        print("Se la cattura viene da capture_plan.sh (sweep a caldo) il tool "
              "giusto e' split_by_chanspec.py.", file=sys.stderr)
        return 1

    for nome, righe in segmenti:
        dest = os.path.join(a.outdir, a.prefix + nome if a.prefix else nome)
        with open(dest, "w", encoding="utf-8") as g:
            g.writelines(righe)
        print(f"{dest}: {len(righe)} righe")

    print(f"\n{trovati} MARK trovati, {len(segmenti)} segmenti scritti.")
    print("I nomi vengono dalle ETICHETTE, non da un ordine assunto: se un "
          "canale manca, manca perche' il ciclo non c'e' stato.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
