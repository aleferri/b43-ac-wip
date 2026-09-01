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
import collections
import os
import re
import signal
import sys

# senza questo, un `| head` fa uscire un BrokenPipeError sul traceback
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

RX_CS = re.compile(r'\bCHANSPEC\b.*?\bch=(\d+)\s+bw=(\S+)\s+band=(\S+)')
RX_T = re.compile(r'\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(\S+)')

# Il chanspec del ciclo, da uno dei due hook che lo portano:
#   OBJ.WR a 0xa0   la scrittura in shared memory
#   CS.SHM          l'argomento di wlc_phy_chanspec_shm_set, che la esegue
# Sono adiacenti e ridondanti; basta che uno dei due sia armato nella build.
BW_CS = {0x1000: 20, 0x1800: 40, 0x2000: 80, 0x2800: 160}
RX_SHM = re.compile(r'OBJ\.WR\s+addr=0x0*a0\s+val=0x0*([0-9a-f]+)')
RX_CSSHM = re.compile(r'CS\.SHM\b.*\braw=0x0*([0-9a-f]+)')
CENTRO = {20: 0, 40: 2, 80: 6, 160: 14}


def chanspec_di(riga):
    """(canale basso, larghezza) da una riga, o None. Per 40 e 80 il valore
    porta il canale CENTRALE, e viene riportato al basso del blocco."""
    m = RX_SHM.search(riga) or RX_CSSHM.search(riga)
    if not m:
        return None
    cs = int(m.group(1), 16)
    bw = BW_CS.get(cs & 0x3800)
    if bw is None:
        return None
    return (cs & 0xff) - CENTRO[bw], bw


def etichetta(righe):
    for l in righe:
        et = chanspec_di(l)
        if et:
            return et
    return None

# Il gap da solo non basta: mentre l'interfaccia e' su, un campionamento
# periodico lascia buchi di ~1.00 s indistinguibili per durata dallo sleep 1 fra
# i cicli. Su d6220/DSL quel periodico e' OBJ.RD, su agcombo e' MAC.MCTRL, e in
# entrambi i casi ha la STESSA op ai due lati del buco. Il confine vero ha un
# cambio di contesto: si esce da un RETVAL o da una scrittura PHY e si rientra
# dal controllo MAC.
def is_boundary(dt, gap, op_prev, op_next):
    if dt <= gap:
        return False
    return op_prev != op_next


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

    # CONFINE e ETICHETTA sono due cose distinte, e vengono da segnali diversi.
    #
    # Il confine e' il salto temporale fra un ciclo e il successivo: lo sleep di
    # capture_plan.sh. Il chanspec NON e' un buon confine anche quando c'e', e la
    # ragione e' misurabile: dentro un ciclo cade a +68 record dall'inizio, dopo
    # la CAL.INIT che sta a +13, quindi tagliando li' la testa di ogni ciclo --
    # compresa la sua cal_init -- finisce in coda al segmento precedente.
    #
    # L'etichetta invece viene dal chanspec trovato DENTRO il segmento, che con
    # questo taglio e' quello del ciclo giusto.
    salti, prev_t, prev_op = [], None, None
    for i, l in enumerate(lines):
        m = RX_T.match(l)
        if not m:
            continue
        t, op = float(m.group(1)), m.group(3)
        if prev_t is not None and is_boundary(t - prev_t, gap, prev_op, op):
            salti.append(i)
        prev_t, prev_op = t, op

    cs_pos = [(i, chanspec_di(l)) for i, l in enumerate(lines) if chanspec_di(l)]
    bounds, criterio = salti, f"salti > {gap}s: {len(salti)}"

    # Validazione: ogni segmento deve contenere UN chanspec. Si contano i VALORI
    # distinti, non i record: quando sono armati entrambi gli hook, CS.SHM e la
    # OBJ.WR a 0xa0 che ne consegue portano lo stesso valore a due record di
    # distanza, e sono lo stesso evento. Se il conto non torna, il set di
    # confini e' sbagliato e lo si dice, invece di scrivere uno split plausibile
    # e falso.
    if cs_pos:
        import bisect
        per_seg = {}
        for pos, et in cs_pos:
            per_seg.setdefault(bisect.bisect_right(bounds, pos), set()).add(et)
        vuoti = [k for k in range(len(bounds) + 1) if not per_seg.get(k)]
        doppi = [k for k in range(len(bounds) + 1) if len(per_seg.get(k, ())) > 1]
        if vuoti or doppi:
            print(f"ATTENZIONE: {len(bounds) + 1} segmenti, {len(vuoti)} senza "
                  f"chanspec e {len(doppi)} con piu' di un chanspec distinto. "
                  f"I confini a salti non tornano su questa traccia.")
        else:
            criterio += f", validati dai chanspec (uno per segmento)"

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
        # Il primo segmento e' un frammento solo se non porta un chanspec suo:
        # con i confini a salti un ciclo intero ce l'ha sempre. NON si guarda la
        # CAL.INIT, che nelle catture prese prima dell'hook non c'e' comunque e
        # farebbe passare per frammento un ciclo completo.
        parziale = k == 0 and not et
        if parziale:
            base = "00-init-parziale"
        elif et:
            base = f"{k:02d}-up-ch{et[0]}-bw{et[1]}"
        else:
            base = f"{k:02d}" + (f"-up-ch{chans[k]}" if k < len(chans) else "")
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

    if any(nome[:2].isdigit() and '-up-ch' in nome for nome, *_ in esito):
        print("\nI nomi con -up-chN-bwN vengono dal CHANSPEC trovato nel segmento.")
    if chans:
        print("Quelli senza vengono dall'ORDINE della lista --canali, e vanno")
        print("verificati: e' posizionale.")
    print("A 40 e 80 MHz il chanspec porta il canale CENTRALE (5g36/40 -> ch=38);")
    print("nel nome si riporta il canale BASSO del blocco.")


if __name__ == '__main__':
    main()
