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
import re
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


OP_HEAD = re.compile(r'^(\S+)\s+addr=(0x[0-9a-f]+)')
TBL_HEAD = re.compile(r'^TBL\.(?:WR|RD)\s+(id=\S+\s+off=\S+)')

# Op che NON hanno un'identita' propria: sono porte, e l'identita' e' l'accesso
# che le racchiude.
#
#   PHY 0x000d/0x000e   porta indirizzo delle tabelle
#   PHY 0x000f/0x0010   porta dati delle tabelle, word bassa e alta
#
# Che 0x0010 sia la seconda word della porta e non un registro si vede dalla
# corsa: 0x000d, 0x000e, poi 0x0010 e 0x000f alternati, una coppia per voce.
#
# Una TBL.WR e' un marcatore: i dati escono come una corsa di scritture sulla
# porta dati, una per voce. Due scritture su 0x000f non sono "la stessa op con
# un valore diverso" solo perche' l'indirizzo coincide -- l'indirizzo coincide
# sempre, e' una porta. Possono appartenere a tabelle diverse.
#
# Percio' la chiave di accoppiamento, per queste, include il marcatore TBL che
# le precede nel rispettivo flusso. Le altre classi hanno un indirizzo che
# identifica un registro e si accoppiano su quello.
PORTE = {('PHY.WR', '0xd'), ('PHY.WR', '0xe'), ('PHY.WR', '0xf'),
         ('PHY.WR', '0x10'),
         ('PHY.RD', '0xd'), ('PHY.RD', '0xe'), ('PHY.RD', '0xf'),
         ('PHY.RD', '0x10')}

# Relazioni di generazione controllate e per cui NON serve una regola, perche'
# le classi generate non compaiono nelle catture del d6220:
#
#   TPL.RAMW -> TPL.PTRW/TPL.DATW    zero occorrenze delle seconde
#   OBJ.BULKW -> OBJ.WR              la coppia bulk non e' agganciata la'
#
# E una che c'e' ma non e' un pericolo per l'accoppiamento: MAC.MHF scrive la
# cella HOSTF corrispondente in cinque casi su undici, quindi un MHF mancante
# porta con se' una OBJ.WR mancante. Sono due op tracciate e contano due, ma
# non si accoppiano fra loro -- le classi sono diverse -- quindi classify() non
# le confonde. Vedi il TODO sulle host flag in docs/retrace-todo.md.


def chiavi(ops):
    """Chiave di identita' per ogni op: per le porte include il marcatore TBL."""
    out = []
    tbl = None
    for o in ops:
        m = TBL_HEAD.match(o)
        if m:
            tbl = m.group(1)
        m = OP_HEAD.match(o)
        if not m:
            out.append(None)
            continue
        key = (m.group(1), m.group(2))
        out.append(key + (tbl,) if key in PORTE else key)
    return out


def lcs_stats(V, T):
    sm = difflib.SequenceMatcher(None, V, T, autojunk=False)
    oc = sm.get_opcodes()
    eq = sum(b - a for k, a, b, c, d in oc if k == 'equal')
    diff = [o for o in oc if o[0] != 'equal']
    return eq, len(diff), diff


def classify(V, T):
    """Separa il valore sbagliato dall'op di troppo.

    Un'op emessa sul registro giusto col valore sbagliato compare due volte nel
    diff -- manca la versione di wl e sopravanza quella del port -- e contarla
    come "una mancante piu' una di troppo" gonfia il difetto e lo chiama col
    nome sbagliato: il registro e' quello giusto, il numero no.

    L'accoppiamento e' per identita' dentro la stessa regione sostituita, dove
    l'identita' viene da chiavi(): un registro per le classi normali, il
    registro piu' il marcatore TBL per le porte delle tabelle. Senza quella
    distinzione due scritture sulla porta dati verrebbero appaiate solo perche'
    l'indirizzo e' lo stesso, e l'indirizzo di una porta e' sempre lo stesso.
    """
    kv, kt = chiavi(V), chiavi(T)
    sm = difflib.SequenceMatcher(None, V, T, autojunk=False)
    wrong = missing = surplus = 0
    for k, a, b, c, d in sm.get_opcodes():
        if k == 'equal':
            continue
        avail = {}
        for x in kt[c:d]:
            if x is not None:
                avail[x] = avail.get(x, 0) + 1
        paired = 0
        for x in kv[a:b]:
            if x is not None and avail.get(x):
                avail[x] -= 1
                paired += 1
        wrong += paired
        missing += (b - a) - paired
        surplus += (d - c) - paired
    return wrong, missing, surplus


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
        if 'val=UNDEFINED' in o or C.val_nondet(o):
            o = C.VAL_TOK.sub('val=*', o, count=1)
        return o

    rules = KNOWN.get(args.board, [])
    V0 = [canon(x) for x in v]
    T = [canon(x) for x in t]

    # Op che nessuna cattura puo' contenere: via da entrambi i lati, o il
    # denominatore e il numeratore parlano di cose diverse. Definite in
    # compare.py, come il perimetro.
    V0, sv = C.drop_solo_vendor(V0)
    T, sp = C.drop_solo_port(T)

    eq0, nreg0, _ = lcs_stats(V0, T)
    VP, outside, keys = C.apply_perimeter(V0)
    eqp, nregp, _ = lcs_stats(VP, T)
    V1, skipped, used = apply_skips(VP, rules, args.verbose)
    eq1, nreg1, diff1 = lcs_stats(V1, T)

    print(f"board {args.board}, finestra {lo}:{hi}")
    if sv:
        print(f"solo vendor     : {len(sv)} op che nessun codice b43 "
              f"puo' emettere")
    if sp:
        print(f"solo port       : {len(sp)} op del port senza oracolo, "
              f"vedi SOLO_PORT")
    print()
    print("La riga che conta e' 'grezzo'. Il denominatore e' l'unione: tutte le")
    print("op di wl PIU' quelle che il port emette e wl no, perche' un'op di")
    print("troppo e' un difetto quanto una mancante -- il driver deve emettere")
    print("le op di wl, non le sue. Fa 100% solo se i due flussi coincidono.")
    print("'nel perimetro' toglie cio' che l'harness non puo' emettere e serve")
    print("a navigare, non a dare un punteggio: quel debito e' in")
    print("docs/retrace-todo.md.\n")

    def riga(nome, eq, nv, nt, nreg, V, T):
        tot = nv + (nt - eq)
        wrong, missing, surplus = classify(V, T)
        print(f"{nome:16s}: {eq}/{tot} = {100.0 * eq / tot:.2f}%   {nreg} regioni")
        print(f"                  {wrong} col valore sbagliato, "
              f"{missing} op di wl mancanti, {surplus} op del port di troppo")

    riga("grezzo", eq0, len(V0), len(T), nreg0, V0, T)
    riga("nel perimetro", eqp, len(VP), len(T), nregp, VP, T)
    print(f"                  {len(outside)} op fuori perimetro su "
          f"{len(keys)} celle dichiarate di altri")
    riga("CON  eccezioni", eq1, len(V1), len(T), nreg1, V1, T)
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
