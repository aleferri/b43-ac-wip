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
import re
import importlib.util
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir, 'reverse-tools'))
import tracelib  # noqa: E402


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
                    "GPIO della finalize. NON e' rumore del contesto up, come "
                    "diceva questa voce: e' il suspend interno di una ricarica "
                    "del beacon, e il port ne emette "
                    "@beacon_reload_pre. Sui sette segmenti sotto i 5250 MHz "
                    "quelle bastano e questa regola non scatta; sui diciannove "
                    "sopra ne resta una senza controparte, due op con la voce "
                    "sotto, e il residuo e' che la' il vendor ne ha una in piu' "
                    "di quante il chiamante ne dichiari."),
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
                    "prima, stessa natura -- e sui 26 segmenti a freddo non "
                    "scatta mai, perche' quella ricarica il port la emette."),
        dict(pattern=r'^MAC\.MCTRL val=0x1 mask=0x1$',
             dopo=r'^MAC\.MCTRL val=0x0 mask=0x1$',
             prima=r'^GPIO\.OUT val=0x4 mask=0x4',
             ctx=[(-4, r'^MAC\.MHF addr=0x0 val=0x0 mask=0x4000$')],
             max=1, cascata=False,
             motivo="meta' enable della seconda coppia."),

        # L'azzeramento delle righe MAC delle chiavi pairwise e le due righe in
        # cima. Le emette il core: b43_upload_card_macaddress() e
        # b43_security_init() -> b43_clear_keys() -> keymac_write() ->
        # b43_amt_write(), dopo il primo switch_channel. ADDRM.SET e AMT.WR
        # sono le etichette che il tracer del vendor mette sull'ingresso di
        # keymac_write()/b43_amt_write() -- niente addr=/val=, solo idx= --
        # e la suite di integrazione, che vede solo il bus, non le puo'
        # produrre. Le OBJ.BULKR/OBJ.BULKW invece sono traffico bus, e b43 le
        # fa a 16 bit: stanno qui finche' il confronto non sa svolgerle.
        # Solo col profilo bus: l'harness di ../unit emette i record del
        # vendor lui stesso, nel punto di b43 (vedi MOVED).
        # Vedi docs/retrace-todo.md, "Key-table clearing (ADDRM.SET)".
        dict(pattern=r'^ADDRM\.SET idx=',
             dopo=None,
             max=60, solo_bus=True, cascata=False,
             motivo="etichetta di ingresso di keymac_write() sul vendor, non "
                    "un accesso bus: b43 ci arriva da b43_clear_keys() e "
                    "b43_upload_card_macaddress(), ma sul bus i confini di "
                    "funzione non si vedono."),
        dict(pattern=r'^AMT\.WR idx=',
             dopo=r'^ADDRM\.SET idx=',
             max=60, solo_bus=True, cascata=False,
             motivo="etichetta di ingresso di b43_amt_write(): stessa "
                    "ragione della voce sopra, non un accesso bus."),
        dict(pattern=r'^OBJ\.BULKR addr=0x[0-9a-f]+ len=8$',
             dopo=r'^AMT\.WR idx=',
             dopo2=r'^ADDRM\.SET idx=',
             max=60, solo_bus=True, cascata=False,
             motivo="rilettura della riga, che KEEP_FLAGS comporta."),
        dict(pattern=r'^OBJ\.BULKW addr=0x[0-9a-f]+ len=8$',
             dopo=r'^OBJ\.BULKR addr=0x[0-9a-f]+ len=8$',
             dopo2=r'^AMT\.WR idx=',
             max=60, solo_bus=True, cascata=False,
             motivo="riscrittura della riga."),
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


# ---------------------------------------------------------------------------
# Blocchi che b43 emette in un altro punto del flusso, per board. Ogni voce e'
# un blocco delimitato del flusso vendor e il punto in cui b43 lo emette:
#   da, a    : regex sulla prima e sull'ultima op del blocco
#   dopo     : regex sull'op che precede il blocco (contesto, opzionale)
#   lung     : quante op al massimo fra `da` e `a`, estremi compresi
#   verso    : regex sulla prima op dopo il blocco dietro cui b43 lo emette
#   verso_ctx: contesto di quell'op, coppie (offset, regex) (opzionale)
#   indietro : True se b43 lo emette prima: `verso` e' allora l'ultima op
#              prima del blocco
#   max      : quanti blocchi al massimo
#   motivo   : dove e perche' b43 lo emette li'
#
# Il blocco si sposta nel flusso vendor prima dell'allineamento: le sue op
# restano tutte nel confronto, e contano come giuste solo se il port le emette
# nel punto dichiarato.
# ---------------------------------------------------------------------------
MOVED = {
    'd6220': [
        dict(da=r'^OBJ\.RD addr=0xcc ',
             dopo=r'^OBJ\.RD addr=0x77e ',
             a=r'^MAC\.MCTRL val=0x1 mask=0x1$', lung=60,
             verso=r'^OBJ\.RD addr=0x14e ',
             max=40,
             motivo="ricarica del template beacon dentro la spazzata del "
                    "watchdog, fra le due passate dei contatori a 32 bit. In "
                    "b43 e' b43_update_templates(), che mac80211 chiama da "
                    "bss_info_changed(BSS_CHANGED_BEACON): un contesto a "
                    "parte, che non entra nel giro. b43 la emette alla fine "
                    "della spazzata, dopo l'ultima lettura (0x014e)."),
        dict(da=r'^ADDRM\.SET idx=0x0$',
             a=r'^OBJ\.BULKW addr=0x1b8 len=8$', lung=224,
             verso=r'^OBJ\.BULKW addr=0x1f8 len=8$',
             verso_ctx=[(-6, r'^AMT\.WR idx=0x3e a3=0x8002$')],
             max=1,
             motivo="azzeramento delle righe MAC delle chiavi pairwise. Il "
                    "vendor lo fa dentro il channel setup; in b43 e' "
                    "b43_security_init() -> b43_clear_keys(), nella coda di "
                    "b43_wireless_core_init() dopo b43_upload_card_macaddress(), "
                    "cioe' dopo le due righe in cima. b43 ne azzera 50 e non "
                    "56 (B43_NR_PAIRWISE_KEYS): le sei in piu' restano "
                    "mancanti, ed e' la parte chiavi."),
        dict(da=r'^AMT\.WR idx=0x3f$',
             dopo=r'^PHY\.MOD addr=0x2e4 val=0xf00 mask=0x3f00$',
             a=r'^OBJ\.BULKW addr=0x1f8 len=8$', lung=3,
             verso=r'^OBJ\.BULKW addr=0x1b8 len=8$', indietro=True,
             max=1,
             motivo="la riga della stazione senza flag quando parte il "
                    "channel availability check. Il vendor la chiude dentro "
                    "il channel setup, fra la soglia degli impulsi radar "
                    "(0x02e4) e il primo poll; in b43 la chiude "
                    "b43_op_config() quando mac80211 sintonizza con "
                    "radar_enabled, cioe' dopo la coda del core init: dietro "
                    "l'azzeramento delle chiavi."),
    ],
}


def apply_moves(ops, rules):
    """Sposta i blocchi dichiarati in MOVED dove b43 li emette."""
    used = [0] * len(rules)
    moved_ops = [0] * len(rules)
    out = list(ops)
    fixed = [False] * len(out)
    i = 0
    while i < len(out):
        hit = None
        for k, r in enumerate(rules):
            if used[k] >= r['max'] or fixed[i]:
                continue
            if not re.search(r['da'], out[i]):
                continue
            if r.get('dopo') and not (i > 0 and re.search(r['dopo'], out[i - 1])):
                continue
            end = next((j for j in range(i, min(len(out), i + r['lung']))
                        if re.search(r['a'], out[j])), None)
            if end is None:
                continue
            def at(j):
                return re.search(r['verso'], out[j]) and \
                    all(0 <= j + o < len(out) and re.search(x, out[j + o])
                        for o, x in r.get('verso_ctx', ()))
            if r.get('indietro'):
                dest = next((j + 1 for j in range(i - 1, -1, -1) if at(j)),
                            None)
            else:
                dest = next((j + 1 for j in range(end + 1, len(out))
                             if at(j)), None)
            if dest is None:
                continue
            hit = (k, end, dest)
            break
        if hit is None:
            i += 1
            continue
        k, end, dest = hit
        block = out[i:end + 1]
        if dest > end:
            rest = out[end + 1:dest]
            out[i:dest] = rest + block
            fixed[i:dest] = fixed[end + 1:dest] + [True] * len(block)
        else:
            rest = out[dest:i]
            out[dest:end + 1] = block + rest
            fixed[dest:end + 1] = [True] * len(block) + fixed[dest:i]
            i = end + 1
        used[k] += 1
        moved_ops[k] += len(block)
    return out, used, moved_ops


def load_compare(path=None):
    if path is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'compare.py')
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


OP_HEAD = re.compile(r'^(\S+)\s+(?:addr|off)=(0x[0-9a-f]+)')
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
#
# Una che invece c'era in questa lista e non ci sta piu': `OBJ.BULKW -> OBJ.WR`
# stava qui perche' la coppia bulk non era agganciata nelle catture del d6220.
# Adesso lo e' -- su cold01 sono 130 OBJ.BULKR e 151 OBJ.BULKW -- e i due lati
# emettono entrambi l'intestazione, quindi si accoppiano da soli sulla loro
# classe. Se ricompare una cattura in cui il bulk non c'e' e le op singole si',
# quella si' che vuole una regola.
#
# E una che c'e' ma non e' un pericolo per l'accoppiamento: MAC.MHF scrive la
# cella HOSTF corrispondente in cinque casi su undici, quindi un MHF mancante
# porta con se' una OBJ.WR mancante. Sono due op tracciate e contano due, ma
# non si accoppiano fra loro -- le classi sono diverse -- quindi classify() non
# le confonde. Vedi "Host-flag order" in docs/retrace-todo.md.


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
    oc = tracelib.align_opcodes(V, T)
    eq = sum(b - a for k, a, b, c, d in oc if k == 'equal')
    diff = [o for o in oc if o[0] != 'equal']
    return eq, len(diff), diff


def ident(o):
    """Identita' di un'op senza il valore: classe e registro, o la sola classe
    per quelle che un registro non lo hanno (MAC.MCTRL, GPIO.*)."""
    m = OP_HEAD.match(o)
    if m:
        return m.group(1) + ' ' + m.group(2)
    return o.split(' ', 1)[0]


def bus_stats(V, T, C):
    """Allineamento e conteggi per il profilo bus.

    Una traccia presa al bus ha i valori pieni; il vendor svolto ha la lettura
    di un MOD senza valore e la scrittura vincolata a maschera. Le due stringhe
    non sono mai uguali, quindi l'allineamento va fatto sull'identita' -- classe
    e registro -- e il valore si giudica dentro i blocchi allineati con
    ops_equal() di compare.py, che le maschere e le wildcard le conosce. Fuori
    dai blocchi allineati le identita' differiscono per costruzione: la' e'
    tutto mancante o di troppo, non c'e' un valore da confrontare.
    """
    kv = [ident(o) for o in V]
    kt = [ident(o) for o in T]
    eq = wrong = missing = surplus = 0
    diff = []
    def same(v, t):
        # canon() ha reso 'val=*' la lettura senza valore; ops_equal() la
        # conosce come UNDEFINED. Il lato a livello di accessor puo' essere
        # l'uno o l'altro: il vendor wl-diag contro una traccia al bus
        # (test/integration), o l'harness contro una cattura al bus
        # (reverse-tools/mmio2ops.py). Il lato bus non ha ne' jolly ne'
        # maschere, quindi provare i due versi non allarga il confronto.
        return (C.ops_equal(v.replace('val=*', 'val=UNDEFINED'), t) or
                C.ops_equal(t.replace('val=*', 'val=UNDEFINED'), v))

    for k, a, b, c, d in tracelib.align_opcodes(kv, kt):
        if k == 'equal':
            bad = [i for i in range(b - a) if not same(V[a + i], T[c + i])]
            eq += (b - a) - len(bad)
            wrong += len(bad)
            for i in bad:
                diff.append(('value', a + i, a + i + 1, c + i, c + i + 1))
            continue
        missing += b - a
        surplus += d - c
        diff.append((k, a, b, c, d))
    return eq, len(diff), diff, wrong, missing, surplus


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

    Un'op del vendor col valore jolly (`val=*`, vedi canon()) appaiata cosi'
    non ha un valore da sbagliare: per ops_equal() di compare.py e' uguale, e
    torna a parte come quarto valore perche' il chiamante la conti fra le
    uguali. L'allineamento esatto non la vede, perche' confronta stringhe.
    """
    kv, kt = chiavi(V), chiavi(T)
    wrong = missing = surplus = jolly = 0
    for k, a, b, c, d in tracelib.align_opcodes(V, T):
        if k == 'equal':
            continue
        avail = {}
        for x in kt[c:d]:
            if x is not None:
                avail[x] = avail.get(x, 0) + 1
        paired = 0
        for i, x in enumerate(kv[a:b]):
            if x is not None and avail.get(x):
                avail[x] -= 1
                paired += 1
                if 'val=*' in V[a + i]:
                    jolly += 1
                else:
                    wrong += 1
        missing += (b - a) - paired
        surplus += (d - c) - paired
    return wrong, missing, surplus, jolly


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vendor')
    ap.add_argument('test')
    ap.add_argument('range')
    ap.add_argument('--bus', action='store_true',
                    help='profilo bus di compare.py: MOD svolti, TBL.* scartati')
    ap.add_argument('--board', default='agcombo')
    ap.add_argument('--led-pins', type=lambda v: int(v, 0), default=0,
                    help='pin GPIO dei LED, da ./ac_trace led_pins BOARD')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    C = load_compare()
    lo, hi = (int(x) for x in args.range.split(':'))
    profile = 'bus' if args.bus else None
    espanse = {}
    v = C.load_vendor(args.vendor, (lo, hi), profile, espanse)
    t = C.load_test(args.test, profile)
    off = C.find_offset(t, v[0])
    if off > 0:
        t = t[off:]

    def canon(o):
        o = C.RET_SUFFIX.sub('', o)
        if 'val=UNDEFINED' in o or C.val_nondet(o):
            o = C.VAL_TOK.sub('val=*', o, count=1)
        return o

    rules = [r for r in KNOWN.get(args.board, [])
             if args.bus or not r.get('solo_bus')]
    moves = MOVED.get(args.board, [])
    V0 = [canon(x) for x in v]
    T = [canon(x) for x in t]

    # Op che nessuna cattura puo' contenere: via da entrambi i lati, o il
    # denominatore e il numeratore parlano di cose diverse. Definite in
    # compare.py, come il perimetro.
    V0, sv = C.drop_solo_vendor(V0)
    V0, mv_used, mv_ops = apply_moves(V0, moves)
    T, sp = C.drop_solo_port(T, V0)

    def stats(V, T):
        if args.bus:
            return bus_stats(V, T, C)
        eq, nreg, diff = lcs_stats(V, T)
        wrong, missing, surplus, jolly = classify(V, T)
        return (eq + jolly, nreg, diff, wrong, missing, surplus)

    eq0, nreg0, _, *cls0 = stats(V0, T)
    VP, outside, keys = C.apply_perimeter(V0, args.led_pins)
    eqp, nregp, _, *clsp = stats(VP, T)
    V1, skipped, used = apply_skips(VP, rules, args.verbose)
    eq1, nreg1, diff1, *cls1 = stats(V1, T)

    print(f"board {args.board}, finestra {lo}:{hi}")
    if espanse:
        print(f"bulk espanse    : {C.format_espanse(espanse)}")
    if sv:
        print(f"solo vendor     : {len(sv)} op che nessun codice b43 "
              f"puo' emettere")
    if sp:
        print(f"solo port       : {len(sp)} op del port senza oracolo, "
              f"vedi SOLO_PORT")
    if moves:
        print(f"spostate        : {sum(mv_ops)} op in {sum(mv_used)} blocchi "
              f"su {len(moves)} regole, vedi MOVED")
    print()
    print("La riga che conta e' 'grezzo'. Il denominatore e' l'unione: tutte le")
    print("op di wl PIU' quelle che il port emette e wl no, perche' un'op di")
    print("troppo e' un difetto quanto una mancante -- il driver deve emettere")
    print("le op di wl, non le sue. Fa 100% solo se i due flussi coincidono.")
    print("'nel perimetro' toglie cio' che l'harness non puo' emettere e serve")
    print("a navigare, non a dare un punteggio: quel debito e' in")
    print("docs/retrace-todo.md.\n")

    def riga(nome, eq, nv, nt, nreg, cls):
        tot = nv + (nt - eq)
        wrong, missing, surplus = cls
        print(f"{nome:16s}: {eq}/{tot} = {100.0 * eq / tot:.2f}%   {nreg} regioni")
        print(f"                  {wrong} col valore sbagliato, "
              f"{missing} op di wl mancanti, {surplus} op del port di troppo")

    riga("grezzo", eq0, len(V0), len(T), nreg0, cls0)
    riga("nel perimetro", eqp, len(VP), len(T), nregp, clsp)
    print(f"                  {len(outside)} op fuori perimetro su "
          f"{len(keys)} celle dichiarate di altri")
    riga("CON  eccezioni", eq1, len(V1), len(T), nreg1, cls1)
    print(f"                  {len(skipped)} op saltate su {len(rules)} regole\n")

    for k, r in enumerate(C.PERIMETER):
        n = sum(1 for _, h in outside if h == k)
        print(f"  [perimetro {k}] {n} op" + ("  NON APPLICATA" if not n else ""))
        for line in re.findall(r'.{1,72}(?:\s|$)', r['motivo']):
            print(f"      {line.strip()}")
        print()

    for k, r in enumerate(moves):
        tag = "NON APPLICATA (0 volte)" if not mv_used[k] else \
            f"{mv_used[k]}/{r['max']} blocchi, {mv_ops[k]} op"
        print(f"  [spostamento {k}] {tag}")
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
