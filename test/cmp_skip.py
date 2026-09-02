#!/usr/bin/env python3
"""Confronto vendor/test con una lista DICHIARATA di divergenze note.

Il problema che risolve: una singola op non riprodotta a inizio traccia
disallinea tutto il resto, e 30000 op diventano illeggibili. Togliendo dal
flusso vendor le op che sappiamo di non voler emettere, il confronto a valle
torna misurabile.

Il problema che introduce, e per cui ci sono delle regole: una lista di
eccezioni e' anche il modo piu' comodo di far tornare un numero.

  1. Ogni voce ha un MOTIVO scritto e un CONTESTO (op precedente attesa).
     Se il contesto non combacia, la voce non si applica e lo si dice.
  2. Il risultato riporta SEMPRE quante op sono state saltate. La percentuale
     non si cita da sola.
  3. Una voce e' legittima solo se sappiamo perche' il port non deve emettere
     quell'op. "Si allinea meglio" non e' un motivo.
  4. Le op saltate che potrebbero avere effetti su valori letti piu' tardi sono
     marcate `cascata=True`: in quel caso il confronto a valle e' sospetto e il
     tool lo segnala.

Uso: cmp_skip.py vendor.txt test.txt lo:hi [--board agcombo] [--verbose]
"""
import argparse
import difflib
import importlib.util
import re
import sys


# ---------------------------------------------------------------------------
# Divergenze note, per board. Ogni voce:
#   pattern  : regex sull'op vendor da saltare
#   dopo     : regex sull'op vendor immediatamente precedente (contesto)
#   dopo2    : regex sull'op due posizioni prima (opzionale)
#   prima    : regex sull'op immediatamente successiva (opzionale)
#   prima2   : regex sull'op due posizioni dopo (opzionale)
#   max      : quante volte al massimo puo' applicarsi
#   motivo   : perche' il port non deve emetterla
#   cascata  : True se l'op potrebbe influenzare valori letti dopo
# ---------------------------------------------------------------------------
KNOWN = {
    'd6220': [
        dict(pattern=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             dopo=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             prima=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             prima2=r'^PHY\.RD addr=0x7af',
             ctx=[(-6, r'^PHY\.RD addr=0x527')],
             max=3, cascata=False,
             motivo="coppia enable+suspend interlacciata dal contesto up "
                    "durante l'attesa ~1 s del probe pacing di "
                    "rxiqcal_finalize: compare solo dopo risvegli in ritardo "
                    "(gap 1.32 s invece di 1.00 s, es. #28859/#28950) e solo "
                    "nella cattura -up -- assente in attach ch44, attach "
                    "ch36-bw40 e down-to-bss. Il contesto a -6 (peek 0x527, "
                    "cioe' iter regolare) esclude la coppia gemella in coda "
                    "all'extended-first, che il port trascrive ed emette. "
                    "Rumore di scheduling, non struttura del driver: il port "
                    "non deve emetterla."),

        dict(pattern=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             dopo=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             dopo2=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             prima=r'^PHY\.RD addr=0x7af',
             ctx=[(-7, r'^PHY\.RD addr=0x527')],
             max=3, cascata=False,
             motivo="meta' suspend della coppia sopra: stessa evidenza."),

        dict(pattern=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             dopo=r'^MAC\.MHF addr=0x0 val=0x0 mask=0x4000$',
             prima=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             max=1, cascata=False,
             motivo="prima coppia suspend+enable fra la MHF (clr 0x4000) e la "
                    "GPIO della finalize: #28759 arriva 1.37 s dopo la MHF e "
                    "su un'altra cpu (cpu0 -> cpu1) -- attivita' MAC "
                    "interlacciata dal contesto up, assente nel warm. Il port "
                    "non la emette."),
        dict(pattern=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             dopo=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             dopo2=r'^MAC\.MHF addr=0x0 val=0x0 mask=0x4000$',
             max=1, cascata=False,
             motivo="meta' enable della coppia sopra."),
        dict(pattern=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             dopo=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             prima=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             ctx=[(-3, r'^MAC\.MHF addr=0x0 val=0x0 mask=0x4000$')],
             max=1, cascata=False,
             motivo="seconda coppia fra MHF e GPIO: #28761 a +1.29 s dalla "
                    "prima, stessa evidenza."),
        dict(pattern=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             dopo=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             prima=r'^GPIO\.OUT val=0x4 mask=0x4',
             ctx=[(-4, r'^MAC\.MHF addr=0x0 val=0x0 mask=0x4000$')],
             max=1, cascata=False,
             motivo="meta' enable della seconda coppia."),
    ],
    'agcombo': [
        dict(pattern=r'^PHY\.WR addr=0x1ec val=0x2$',
             dopo=r'^PHY\.MOD addr=0x2e4',
             max=2, cascata=False,
             motivo="radar detect OFF. wlc_phy_radar_detect_on_off_cfg_acphy e' "
                    "144B in 7.14.43 (due scritture: 0x2 poi 0x9c40) e 52B in "
                    "7.14.89, dove lo spegnimento preliminare non c'e' piu'. "
                    "Il port modella 7.14.89, quindi non deve emetterla."),

        dict(pattern=r'^(SI\.COREREG|PMU\.PLL|GPIO\.(OUT|OE) val=0x0 mask=0x0)',
             dopo=None,
             max=6, cascata=True,
             motivo="re-init di clock e bus fra le due entrate nell'analogico. "
                    "Sono op di bcma/b43-core, non del PHY: il PHY non "
                    "riprogramma il PLL. NON CAPITO se e per chip o per "
                    "versione -- il codice di switch_radio e' identico fra "
                    "7.14.89 e 7.14.43, quindi la differenza e' a runtime."),
    ],
}


def load_compare(path='compare.py'):
    spec = importlib.util.spec_from_file_location('cmp', path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def apply_skips(ops, rules, verbose=False):
    """togli dal flusso vendor le op che combaciano, rispettando max e contesto"""
    used = [0] * len(rules)
    out, skipped = [], []
    for i, op in enumerate(ops):
        hit = None
        for k, r in enumerate(rules):
            if used[k] >= r['max']:
                continue
            if not re.search(r['pattern'], op):
                continue
            ctx = [('dopo', i - 1), ('dopo2', i - 2),
                   ('prima', i + 1), ('prima2', i + 2)]
            ok = True
            for key, j in ctx:
                rx = r.get(key)
                if rx is None:
                    continue
                neigh = ops[j] if 0 <= j < len(ops) else ''
                if not re.search(rx, neigh):
                    ok = False
                    break
            for off, rx in r.get('ctx', ()):
                neigh = ops[i + off] if 0 <= i + off < len(ops) else ''
                if not re.search(rx, neigh):
                    ok = False
                    break
            if not ok:
                continue
            hit = k
            break
        if hit is None:
            out.append(op)
        else:
            used[hit] += 1
            skipped.append((i, op, hit))
    return out, skipped, used


def lcs_stats(V, T):
    sm = difflib.SequenceMatcher(None, V, T, autojunk=False)
    oc = sm.get_opcodes()
    eq = sum(b - a for k, a, b, c, d in oc if k == 'equal')
    diff = [o for o in oc if o[0] != 'equal']
    return eq, len(diff), diff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vendor')
    ap.add_argument('test')
    ap.add_argument('range')
    ap.add_argument('--board', default='agcombo')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    C = load_compare()
    lo, hi = (int(x) for x in args.range.split(':'))
    v = C.load_vendor(args.vendor, (lo, hi))
    t = C.load_test(args.test)
    off = C.find_offset(t, v[0])
    if off > 0:
        t = t[off:]

    def canon(o):
        o = C.RET_SUFFIX.sub('', o)
        if 'val=UNDEFINED' in o:
            o = C.VAL_TOK.sub('val=*', o, count=1)
        return o

    rules = KNOWN.get(args.board, [])
    V0 = [canon(x) for x in v]
    T = [canon(x) for x in t]

    eq0, nreg0, _ = lcs_stats(V0, T)
    VP, outside, keys = C.apply_perimeter(V0)
    eqp, nregp, _ = lcs_stats(VP, T)
    V1, skipped, used = apply_skips(VP, rules, args.verbose)
    eq1, nreg1, diff1 = lcs_stats(V1, T)

    print(f"board {args.board}, finestra {lo}:{hi}\n")
    print("La riga che conta e' 'grezzo': op di wl riprodotte in ordine su")
    print("TUTTE quelle di wl. 'nel perimetro' toglie cio' che l'harness non")
    print("puo' emettere e serve a navigare, non a dare un punteggio: quel")
    print("debito e' tracciato in docs/retrace-todo.md.\n")
    print(f"grezzo          : {eq0}/{len(V0)} = {100.0 * eq0 / len(V0):.2f}%   {nreg0} regioni")
    print(f"nel perimetro   : {eqp}/{len(VP)} = {100.0 * eqp / len(VP):.2f}%   {nregp} regioni")
    print(f"                  {len(outside)} op fuori perimetro su "
          f"{len(keys)} celle dichiarate di altri")
    print(f"CON  eccezioni  : {eq1}/{len(V1)} = {100.0 * eq1 / len(V1):.2f}%   {nreg1} regioni")
    print(f"                  {len(skipped)} op saltate su {len(rules)} regole\n")

    for k, r in enumerate(C.PERIMETER):
        n = sum(1 for _, h in outside if h == k)
        print(f"  [perimetro {k}] {n} op" + ("  NON APPLICATA" if not n else ""))
        for line in re.findall(r'.{1,72}(?:\s|$)', r['motivo']):
            print(f"      {line.strip()}")
        print()

    casc = False
    for k, r in enumerate(rules):
        if not used[k]:
            print(f"  [regola {k}] NON APPLICATA (0 volte)")
        else:
            flag = '  ** CASCATA **' if r['cascata'] else ''
            print(f"  [regola {k}] {used[k]}/{r['max']} volte{flag}")
            casc = casc or r['cascata']
        for line in re.findall(r'.{1,72}(?:\s|$)', r['motivo']):
            print(f"      {line.strip()}")
        print()

    if casc:
        print("ATTENZIONE: almeno una regola e' marcata cascata. Le op saltate")
        print("potrebbero influenzare valori letti piu' tardi, quindi il")
        print("confronto a valle e' indicativo e non probante.\n")

    print("prime regioni divergenti dopo le eccezioni:")
    for k, a, b, c, d in diff1[:6]:
        print(f"  {k:<7} V[{a}:{b}]={b - a}  T[{c}:{d}]={d - c}")
        for i in range(a, min(b, a + 2)):
            print(f"      V {V1[i][:64]}")
        for i in range(c, min(d, c + 2)):
            print(f"      T {T[i][:64]}")


if __name__ == '__main__':
    main()
