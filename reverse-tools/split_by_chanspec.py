#!/usr/bin/env python3
"""Taglia una traccia decodificata in un segmento per canale.

Due criteri, nell'ordine:

  CHANSPEC  se i record ci sono, sono il confine esatto e danno anche il nome.
  gap       altrimenti si taglia sui salti temporali: fra un `wl down` e il
            `chanspec` successivo capture_plan.sh dorme 1 s, e nessuna sequenza
            di bring-up ha pause simili. Il nome viene dalla lista dei canali
            della fase, passata con --canali, e va verificato: e' posizionale.

Uso:
  split_by_chanspec.py trace.txt [dir] [--gap 0.4] [--canali 36,40,44,...]
"""
import os
import re
import signal
import sys

# senza questo, un `| head` fa uscire un BrokenPipeError sul traceback
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

RX_CS = re.compile(r'\bCHANSPEC\b.*?\bch=(\d+)\s+bw=(\S+)\s+band=(\S+)')
RX_T = re.compile(r'\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(\S+)')

# La scrittura del chanspec in shared memory: un record per ciclo, all'inizio, e
# porta l'etichetta. E' il confine esatto e va preferito ai salti temporali.
BW_CS = {0x1000: 20, 0x1800: 40, 0x2000: 80, 0x2800: 160}
RX_SHM = re.compile(r'OBJ\.WR\s+addr=0x0*a0\s+val=0x0*([0-9a-f]+)')
CENTRO = {20: 0, 40: 2, 80: 6, 160: 14}


def etichetta(righe):
    """(canale basso, larghezza) dal primo chanspec in SHM. Per 40 e 80 il
    valore porta il canale CENTRALE, e viene riportato al basso del blocco."""
    for l in righe:
        m = RX_SHM.search(l)
        if not m:
            continue
        cs = int(m.group(1), 16)
        bw = BW_CS.get(cs & 0x3800)
        if bw is not None:
            return (cs & 0xff) - CENTRO[bw], bw
    return None

# Il gap da solo non basta: mentre l'interfaccia e' su, il campionamento
# periodico del rumore lascia buchi di 1.00 s fra due OBJ.RD, indistinguibili
# per durata dallo sleep 1 fra i cicli. Il confine vero ha un cambio di
# contesto: si esce da una scrittura PHY e si rientra dal controllo MAC.
def is_boundary(dt, gap, op_prev, op_next):
    if dt <= gap:
        return False
    return not (op_prev == op_next == 'OBJ.RD')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=')[0]: a.split('=')[1] for a in sys.argv[1:]
            if a.startswith('--') and '=' in a}
    src = args[0]
    out = args[1] if len(args) > 1 else 'split'
    gap = float(opts.get('--gap', 1.03))
    chans = opts.get('--canali', '').split(',') if opts.get('--canali') else []
    if os.path.exists(out) and not os.path.isdir(out):
        sys.exit(f"'{out}' esiste e non e' una directory")
    os.makedirs(out, exist_ok=True)

    lines = open(src, errors='replace').read().split('\n')

    # Confini di ciclo: solo i salti temporali. I record CHANSPEC NON si usano
    # come confine -- la funzione viene invocata col chanspec corrente, che e' in
    # ritardo di un ciclo, e in una fase compaiono valori della precedente. Si
    # raccolgono e si riportano, per verifica.
    bounds = [i for i, l in enumerate(lines) if RX_SHM.search(l)]
    if bounds:
        criterio = f"chanspec in SHM: {len(bounds)}"
    else:
        # Ripiego. Sul DSL funzionava perche' lo sleep fra i cicli lasciava
        # 1.06 s contro gli 1.00 s esatti del campionamento del rumore; sul
        # d6220 le due durate coincidono e il criterio non discrimina, dava 27
        # segmenti per 19 canali.
        prev_t, prev_op = None, None
        for i, l in enumerate(lines):
            m = RX_T.match(l)
            if not m:
                continue
            t, op = float(m.group(1)), m.group(3)
            if prev_t is not None and is_boundary(t - prev_t, gap, prev_op, op):
                bounds.append(i)
            prev_t, prev_op = t, op
        criterio = f"salti > {gap}s: {len(bounds)}"

    edges = [0] + bounds + [len(lines)]
    nseg = len(edges) - 1
    print(f"{nseg} segmenti (confini da {criterio})")
    if chans and nseg != len(chans):
        print(f"ATTENZIONE: {nseg} segmenti ma {len(chans)} canali nella lista. "
              f"I nomi posizionali sono inaffidabili.")

    # Si scrive TUTTO prima di stampare. Con SIGPIPE a SIG_DFL un `| head` uccide
    # il processo al primo print, e stampando dentro il ciclo si finirebbe con i
    # primi file scritti e gli altri no -- silenziosamente. E' successo.
    used = {}
    esito = []
    for k in range(nseg):
        body = lines[edges[k]:edges[k + 1]]
        if not any(x.strip() for x in body):
            continue
        # Il primo segmento e' la coda di init prima del primo chanspec: bus,
        # OTP, PLL. Tutti gli altri sono cicli up/down.
        #
        # NON si chiamano "attach" ne' "to-bss", e il conto delle tabelle dice
        # perche': il primo segmento ha lo stesso profilo di un ciclo qualsiasi
        # -- 641 record e 1638 parole, identici -- mentre una cattura to-bss del
        # d6220 ne scrive 3740, con quattro tabelle in piu' (0x0e, 0x42, 0x62,
        # 0x82) e 0x40/0x60 a 384 parole invece di 128, cioe' per tre core
        # invece di uno. capture_plan.sh non porta l'interfaccia ad associarsi,
        # e la programmazione per-core sembra avvenire la'.
        et = etichetta(body)
        if k == 0:
            base = "00-init-parziale"
        elif et:
            base = f"{k:02d}-up-ch{et[0]}-bw{et[1]}"
        else:
            base = f"{k:02d}" + (f"-ch{chans[k]}" if k < len(chans) else "")
        n = used.get(base, 0) + 1
        used[base] = n
        name = base if n == 1 else f"{base}-{n}"
        open(os.path.join(out, f"{name}.txt"), 'w').write('\n'.join(body) + '\n')

        cs = [f"ch{m.group(1)}/{m.group(2)}" for x in body
              if (m := RX_CS.search(x))]
        atteso = f"ch{chans[k]}" if k < len(chans) else None
        disaccordo = bool(cs) and atteso and not any(
            c.startswith(atteso + '/') for c in cs)
        esito.append((name, len(body), cs, disaccordo))

    for name, nrighe, cs, disaccordo in esito:
        tag = ''
        if cs:
            tag = f"   CHANSPEC: {','.join(cs)}" + ("  <-- non concorda" if disaccordo else "")
        print(f"  {name + '.txt':24} {nrighe:8} righe{tag}")

    print("\nI nomi vengono dall'ORDINE della fase in capture_plan.sh, non dai")
    print("record CHANSPEC. Per 40 e 80 MHz il chanspec porta il canale CENTRALE")
    print("(5g36/40 -> ch=38), quindi non coincide col nome per costruzione.")


if __name__ == '__main__':
    main()
