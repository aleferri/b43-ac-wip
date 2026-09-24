#!/usr/bin/env python3
"""
Normalize a wl-diag capture and compare it against the trace emitted by
ac_trace. Only op lines are compared (address / value / mask); the
episode number, timestamp, and cpuN prefix are stripped from the vendor
trace before diffing since the test does not simulate scheduling.

Usage:
    compare.py <vendor.txt> <test.out> [OPTIONS]

--range LO:HI     keep only vendor lines whose episode # is within [LO,HI]
--senza-perimetro confronta anche le op del core b43 (vedi PERIMETER)
--auto-align     find the offset in <test> that best matches <vendor>[0]
                  by scanning for the first common op; useful when the
                  test flow does a prologue (save-gain, save-tone, ...)
                  that the vendor does not emit. Reports the offset and
                  compares from there.
--align-on OP    like --auto-align but pin to a specific op string
                  (e.g. 'PHY.WR   addr=0x0463 val=0x0027').

Vendor uses PHY.OR/PHY.AND for single-bit sets/clears; they get folded
to PHY.MOD in the format `val=<kernel_mask> mask=<kernel_set>` so the
test's PHY.MOD lines diff cleanly.
"""
import re
import sys
import argparse
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir, 'reverse-tools'))
import tracelib  # noqa: E402

VENDOR_LINE = re.compile(
    r'^\s*[0-9.]+\s+#\d+\s+cpu\d+\s+(.+?)\s*(?:;.*)?$'
)
TEST_LINE = re.compile(r'^cpu\d+\s+(.+?)\s*$')

# Op di alto livello e loro "ombra" a livello di core register: il tracer
# vendor logga entrambe, l'harness (come il driver) solo la prima. Le ombre sono
# la coppia addr/data del regcontrol del chipcommon e i registri GPIO; le altre
# SI.COREREG (0x0600, 0x0080, 0x0088, e tutto cio' che non e' core 0) sono op a
# se' stanti e non vanno scartate.
SHADOW_OFFSETS = {
    0x658, 0x65c,          # regcontrol addr/data   <- PMU.RC
    0x660, 0x664,          # pllcontrol addr/data   <- PMU.PLL
    0x064,                 # gpioout               <- GPIO.OUT
    0x068,                 # gpioouten             <- GPIO.OE / GPIO.OUTEN
    0x06c,                 # gpiocontrol           <- GPIO.CTL
}
# NON un'ombra, anche se segue sempre una GPIO.CTL o GPIO.OE: (core 0,
# 0x08c) e' gpiotimeroutmask, scritto da si_gpioled(), cioe' la maschera dei
# GPIO che il timer dei LED puo' pilotare. E' un'op del blocco LED e sta nel
# PERIMETER con le altre.
# Op che il driver esegue davvero, ma da codice fuori dall'unita' sotto test:
# l'harness compila solo src/, non main.c di b43 ne' bcma. Vanno saltate, non
# riprodotte, e solo dopo aver verificato che chi le esegue le emetta *nel punto
# giusto* della sequenza -- altrimenti lo skip nasconde un errore d'ordine.
#
#   (core 3, 0x01e0) BCMA_CLKCTLST: b43_bcma_wireless_core_reset chiama
#   bcma_core_set_clockmode(BCMA_CLKMODE_FAST) prima di b43_phy_init, e la
#   cattura mette la richiesta a #37/#40, cioe' prima della prima op PHY (#60).
#   Ordine verificato. La seconda coppia (#115/#118) legge lo stesso registro
#   con HAVEHT ormai alto: assunta ripetizione della stessa richiesta, non
#   verificata separatamente.
#
#   (core 0, 0x600) BCMA_CC_PMU_CTL: il flush PLL_UPD che latcha i valori
#   PLLCTL. La patch 0007 lo emette con bcma_pmu_set32 subito dopo le due
#   bcma_chipco_pll_write, ed e' esattamente dove lo mette la cattura (#31/#34,
#   read e write del read-modify-write, immediatamente dopo le PLL a #15/#23).
#   Ordine verificato.
# Letture pure che il driver non fa: il tracer vendor le registra, ma sono
# read-back senza effetto e riprodurle vorrebbe dire aggiungere una lettura il
# cui risultato viene scartato. Si saltano, con la condizione che siano
# davvero prive di side effect (mask nulla).
FOREIGN_READBACK = re.compile(
    r'^PMU\.PLL addr=0x[23] val=0x0 mask=0x0\b')

FOREIGN_COREREG = {
    (0x3, 0x1e0),
    (0x0, 0x600),
}

SHADOW_PARENT = re.compile(r'^(?:PMU\.(?:RC|PLL)|GPIO\.(?:OUT|OE|OUTEN|CTL))\b')
SI_COREREG = re.compile(r'^SI\.COREREG\s+core=(0x[0-9a-fA-F]+)\s+off=(0x[0-9a-fA-F]+)')

# L'intestazione di una copia in object memory. Il tracer del vendor la scrive
# perche' aggancia la routine di copia, e subito sotto ci sono le word, una op
# per word: `OBJ.BULKW addr=0x0160 len=32` e poi le sedici `OBJ.WR` da 0x0160.
# In quel caso l'intestazione non e' accesso all'hardware, e' il nome di chi lo
# fa -- b43 scrive le stesse word senza passare da una routine di copia, e
# pretendere che emetta anche l'intestazione vorrebbe dire inventare un'op.
#
# Non sempre pero' e' un'ombra: sulle righe della address match table
# (routing RCMTA) le word non sono decodificate, l'intestazione e' l'unico
# record del traffico, e il port la deve emettere -- vedi b43_test_emit_amt().
# Percio' la condizione non e' la classe ma cosa la segue: si scarta solo se
# sotto c'e' la singola allo stesso indirizzo.
BULK_HEAD = re.compile(r'^OBJ\.BULK([RW])\s+addr=(0x[0-9a-fA-F]+)\s+len=')
BULK_WORD = re.compile(r'^OBJ\.(RD|WR)\s+addr=(0x[0-9a-fA-F]+)\b')

# Il chanspec in shared memory, stesso caso dell'intestazione qui sopra: il
# tracer aggancia la funzione che lo scrive, e la write e' l'op successiva --
# `CS.SHM ch=36 bw=20 band=5g raw=0xd024` e poi `OBJ.WR addr=0x00a0
# val=0xd024`, dove il campo raw del record e' la parola che la write porta.
# Un evento solo con due record, quindi l'hardware lo si confronta sulla write
# e il port non deve emettere il confine.
#
# La condizione e' cosa segue, non la classe, come per BULK_HEAD: se il record
# non ha sotto la sua write resta nel flusso e pesa. Sui 43 segmenti a freddo
# ce n'e' esattamente uno per segmento e la write c'e' sempre, con lo stesso
# valore; un segmento che registrasse il confine e non la write lo direbbe
# invece di sparire.
CHANSPEC_HEAD = re.compile(r'^CS\.SHM\b.*\braw=(0x[0-9a-fA-F]+)')
CHANSPEC_WORD = re.compile(
    r'^OBJ\.WR\s+addr=0x0*a0\s+val=(0x[0-9a-fA-F]+)\b')


def _chanspec_head_is_shadow(op, nxt):
    m = CHANSPEC_HEAD.match(op)
    if not m or nxt is None:
        return False
    w = CHANSPEC_WORD.match(nxt)
    return bool(w) and int(m.group(1), 16) == int(w.group(1), 16)


def _bulk_head_is_shadow(op, nxt):
    m = BULK_HEAD.match(op)
    if not m or nxt is None:
        return False
    w = BULK_WORD.match(nxt)
    if not w:
        return False
    if (m.group(1), w.group(1)) not in (('R', 'RD'), ('W', 'WR')):
        return False
    return int(m.group(2), 16) == int(w.group(2), 16)


# OBJ.SET: l'intestazione di un azzeramento di shared memory, stesso caso del
# BULK_HEAD qui sopra. `OBJ.SET addr=0x10f4 val=0x0000 len=960` e poi le write
# vere, una parola alla volta a partire dallo stesso indirizzo e con lo stesso
# valore. Un evento solo con due record: le write sono nel confronto e il port
# le emette tutte, quindi l'intestazione non va contata una seconda volta.
#
# Come per BULK_HEAD la condizione e' cosa segue, non la classe. Sui 43
# segmenti a freddo ce n'e' esattamente una per segmento e la write che le
# tocca c'e' sempre, con lo stesso indirizzo e lo stesso valore; un segmento
# che registrasse l'intestazione senza le write lo direbbe invece di sparire.
SET_HEAD = re.compile(
    r'^OBJ\.SET\s+addr=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)\s+len=')
SET_WORD = re.compile(r'^OBJ\.WR\s+addr=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)')


def _set_head_is_shadow(op, nxt):
    m = SET_HEAD.match(op)
    if not m or nxt is None:
        return False
    w = SET_WORD.match(nxt)
    if not w:
        return False
    return (int(m.group(1), 16) == int(w.group(1), 16) and
            int(m.group(2), 16) == int(w.group(2), 16))


# PHY.RDW: la lettura del data port senza riselezionare l'indirizzo.
#
# Il record non porta un indirizzo perche' la FUNZIONE non ne ha uno, non
# perche' il tracer se lo perda. Letto sul prologo del blob 7.14 del d6220
# (`mipsdis.py wlD6220.o 0xbc458 0x68`), phy_reg_read_wide prende il solo $a0:
#
#     lw   $v0, 228($a0)      base MMIO
#     lhu  $v0, 1022($v0)     legge base+0x3fe, PHY_DATA
#
# mentre phy_reg_read(pi, addr) a 0xb9d70 usa $a1 e prima scrive base+0x3fc,
# PHY_CONTROL. La wide quella scrittura non la fa: rilegge il data port con
# l'indirizzo lasciato dov'era.
#
# Quindi il registro non e' ignoto ed e' determinato per costruzione: e' quello
# che l'ultimo accesso PHY ha selezionato. Si risolve qui, e il confronto vede
# la stessa op che emette b43, che quel registro lo riseleziona ogni volta --
# una scrittura idempotente di PHY_CONTROL, senza effetto sul dato letto.
#
# Se nessun accesso PHY precede, l'indirizzo non c'e' e il record resta com'e':
# diventa una divergenza, che e' il verso giusto in cui sbagliare.
PHY_SELECT = re.compile(r'^PHY\.(?:RD|WR|MOD)\s+addr=(0x[0-9a-fA-F]+)')
WIDE_READ = re.compile(r'^PHY\.RDW\s+(val=\S+)')


def resolve_wide_reads(ops):
    """Da' a ogni PHY.RDW l'indirizzo che l'accesso precedente ha selezionato."""
    out, sel = [], None
    for op in ops:
        m = PHY_SELECT.match(op)
        if m:
            sel = m.group(1)
        w = WIDE_READ.match(op)
        if w and sel is not None:
            op = normalize_op(f"PHY.RD addr={sel} {w.group(1)}")
        out.append(op)
    return out


# Le intestazioni bulk senza le word sotto.
#
# Sul d6220 le copie in shared memory (selettore 1) e nello SCR (selettore 2)
# sono sempre seguite dalle loro word, e drop_shadow_ops scarta l'intestazione.
# Sul tg789vac-v2 non lo sono mai: 2510 BULKW, 423 BULKR e 43 SET con quei
# selettori sui 43 segmenti a freddo, senza una sola word sotto. Che la word
# sotto sia l'espansione lo dice anche il selettore: dopo `BULKW 0x0010 len=2`
# col selettore 2 il d6220 ha la word con sel=0x20000 e poi una scrittura SHM
# alla stessa cella, il tg789 solo la seconda. La testa pero' fissa gia' la
# forma dell'evento, perche' sui segmenti del d6220 ogni espansione e'
# esattamente len/2 op a 16 bit consecutive da addr, in ordine crescente. Qui
# la si ricostruisce da quella:
#
#   OBJ.SET    len/2 scritture di val: e' un memset, il valore e' nella testa.
#   OBJ.BULKR  len/2 letture val=UNDEFINED: il valore letto non c'e', e per
#              ops_equal() una lettura UNDEFINED vincola indirizzo e classe.
#   OBJ.BULKW  len/2 scritture val=UNDEFINED: `buf` il tracer non lo registra,
#              quindi di queste scritture si confrontano indirizzo e ordine e
#              NON il valore. Per questo il conto esce a parte: una scrittura
#              qui combacia con qualunque valore il port scriva.
#
# Il selettore 4 (RCMTA, le righe da 8 byte della address match table) resta
# com'e': non e' espanso su nessuna board, e il port quell'intestazione la
# emette -- vedi b43_test_emit_amt(). Una testa senza a5, cioe' con l'ARGX non
# ripiegato, resta anch'essa com'e': il selettore non si conosce.
BULK_RAW = re.compile(r'^OBJ\.BULK([RW])\s+addr=(0x[0-9a-fA-F]+)\s+len=(\d+)'
                      r'.*?\ba5=(0x[0-9a-fA-F]+)')
SET_RAW = re.compile(r'^OBJ\.SET\s+addr=(0x[0-9a-fA-F]+)\s+val=(0x[0-9a-fA-F]+)'
                     r'\s+len=(\d+)')
BULK_EXPAND_SEL = (0x10000, 0x20000)


def _same_sel(head, word):
    """La word sotto porta il selettore della testa, o non ne porta uno."""
    a5 = re.search(r'\ba5=(0x[0-9a-fA-F]+)', head)
    sel = re.search(r'\bsel=(0x[0-9a-fA-F]+)', word)
    return not a5 or not sel or int(a5.group(1), 16) == int(sel.group(1), 16)


def expand_bulk_heads(raws, espanse=None):
    """Sostituisce le intestazioni bulk non espanse con le loro word."""
    out = []
    for i, raw in enumerate(raws):
        nraw = raws[i + 1] if i + 1 < len(raws) else None
        nxt = normalize_op(nraw) if nraw is not None else None
        head = normalize_op(raw)
        if _set_head_is_shadow(head, nxt) or \
                (_bulk_head_is_shadow(head, nxt) and _same_sel(raw, nraw)):
            out.append(raw)
            continue
        m = SET_RAW.match(raw)
        if m:
            a, val, n = int(m.group(1), 16), m.group(2), int(m.group(3)) // 2
            kind, words = 'SET', [f"OBJ.WR addr=0x{a + 2 * k:04x} val={val}"
                                  for k in range(n)]
        else:
            m = BULK_RAW.match(raw)
            if not m or int(m.group(4), 16) not in BULK_EXPAND_SEL:
                out.append(raw)
                continue
            rw, a, n = m.group(1), int(m.group(2), 16), int(m.group(3)) // 2
            kind = 'BULK' + rw
            words = [f"OBJ.{'RD' if rw == 'R' else 'WR'} "
                     f"addr=0x{a + 2 * k:04x} val=UNDEFINED" for k in range(n)]
        out.extend(words)
        if espanse is not None:
            espanse[kind] = espanse.get(kind, 0) + len(words)
    return out


def format_espanse(espanse):
    parti = [f"{espanse[k]} word da {k}" for k in ('SET', 'BULKR', 'BULKW')
             if k in espanse]
    s = ", ".join(parti)
    if 'BULKW' in espanse:
        s += f"; delle {espanse['BULKW']} da BULKW il valore non e' confrontato"
    return s


def drop_shadow_ops(ops):
    """Scarta le op di alto livello di cui le seguenti sono l'attuazione."""
    out = []
    parent = False
    for i, op in enumerate(ops):
        nxt = ops[i + 1] if i + 1 < len(ops) else None
        if _bulk_head_is_shadow(op, nxt):
            continue
        if _chanspec_head_is_shadow(op, nxt):
            continue
        if _set_head_is_shadow(op, nxt):
            continue
        if FOREIGN_READBACK.match(op):
            parent = True          # le sue ombre restano ombre
            continue
        m = SI_COREREG.match(op)
        if m and (int(m.group(1), 16), int(m.group(2), 16)) in FOREIGN_COREREG:
            continue
        if m and parent and int(m.group(1), 16) == 0 \
                and int(m.group(2), 16) in SHADOW_OFFSETS:
            continue
        parent = bool(SHADOW_PARENT.match(op))
        out.append(op)
    return out

# La normalizzazione delle op sta in reverse-tools/tracelib.py, e non qui,
# perche' e' la definizione di "la stessa op": gli spazi, la larghezza dei
# letterali, il fold di PHY.AND/PHY.OR sulla forma PHY.MOD che il wrapper
# emette, e il nome dell'abilitazione GPIO -- che l'harness prende dal simbolo
# bcma (bcma_chipco_gpio_outen) e il tracer vendor dal registro (OE). Ogni tool
# che confronta i due lati deve usare la stessa, o due tool danno due risposte
# diverse alla stessa domanda.
normalize_op = tracelib.norm

# ---------------------------------------------------------------------------
# LA METRICA PENALIZZA LE OP IN PIU'
#
# Il denominatore del punteggio e' l'unione: tutte le op di wl piu' quelle che
# il port emette e wl no. Fa 100% solo se i due flussi coincidono.
#
# Le inserzioni del port NON sono gratis: il driver deve emettere le op di wl,
# non le sue, e un accesso di troppo a un registro e' un difetto quanto uno
# mancante -- puo' lasciare l'hardware in uno stato che il driver stock non
# produce mai. Col denominatore sulle sole op di wl, togliere 6000 op spurie
# dal port non muove il numero di un'unita', e il progresso resta invisibile.
#
# Le op che il port emette legittimamente e wl non ha vanno in SOLO_PORT, con
# la ragione scritta. La lista e' vuota di proposito.

# PERIMETRO: op che l'harness non puo' emettere, non op che non ci riguardano.
#
# L'obiettivo e' che b43 emetta una-per-una TUTTE le op di wl, comprese quelle
# del core. Quindi questo non e' un elenco di cose fuori scopo: e' il limite
# dell'harness, che compila solo src/ e non main.c ne' xmit.c ne' bcma. Cio' che
# finisce qui e' debito, ed e' tracciato in docs/retrace-todo.md.
#
# Percio' il numero da guardare e' quello SENZA perimetro, e --senza-perimetro
# e' il modo di ottenerlo. Il perimetro serve per una cosa sola: trovare la
# prossima divergenza del PHY senza fermarsi sulla prima op del core. E' uno
# strumento di navigazione, non un punteggio.
#
# Il criterio e' l'appartenenza: si scarta solo cio' di cui si puo' mostrare che
# e' di qualcun altro. NON la raggiungibilita' da src/, che sarebbe degenere --
# farebbe restringere il denominatore quando il port si restringe, e togliere
# codice alzerebbe il punteggio.
#
# Le prove:
#   CORE_SHM     nomi e blocchi di b43.h del kernel (drivers/net/wireless/
#                broadcom/b43/b43.h); le dimensioni dei blocchi vengono da la'.
#   TPL.RAMW     template RAM, che b43 scrive in
#                b43_write_template_common() e
#                b43_write_mac_bssid_templates().
#   MAC.MHF.RD   b43_hf_read() sta nel core; in src/ non c'e' nessuna
#                chiamata, e l'unico accesso del PHY e' mhf_maskset().
#   OTP, SROMCTL codice srom di bcma.
#
# Cio' che NON e' qui, e resta contato contro di noi anche quando e' con ogni
# probabilita' del core: i contatori 0x768-0x790, 0x7d6-0x7e6 e 0x10c-0x15e,
# che b43.h non nomina perche' b43 non legge mai le statistiche, e le regioni
# 0x8ec-0xa8e, 0x10f4-0x14b2, 0x0020, 0x018a-0x018c, 0x0eec. Spostarle qui
# serve una prova, non un numero migliore.
# ---------------------------------------------------------------------------
CORE_SHM = [
    (0x0000, 0x0006, "UCODEREV/PATCH/DATE/TIME"),
    (0x0008, 0x0008, "PCTLWDPOS"),
    (0x000e, 0x000e, "EDCFSTAT"),
    (0x0012, 0x0012, "DTIMPER"),
    (0x001e, 0x001e, "TIMBPOS"),
    (0x0022, 0x0022, "ACKCTSPHYCTL"),
    (0x0030, 0x0030, "TXFCUR"),
    (0x0034, 0x0034, "RXPADOFF"),
    (0x003e, 0x003e, "NRRXTRANS"),
    (0x0040, 0x0042, "UCODESTAT/FWCAPA"),
    (0x0048, 0x0048, "PRSSIDLEN"),
    (0x004a, 0x004c, "PRTLEN/NOSLPZNATDTIM"),
    (0x0054, 0x0056, "BEACPHYCTL/KTP. PHYVER (0x0050) e PHYTYPE (0x0052) "
                     "sono uscite dal perimetro: le scrive ora il port da "
                     "phy.rev e phy.type in shm_readback_block(), che e' "
                     "dove la cattura le mette"),
    (0x0058, 0x0058, "TSSI_CCK, la meta' che il port non scrive: la 0x005a "
                     "la scrive channel_setup()"),
    (0x0066, 0x0066, "RADAR"),
    (0x0068, 0x006a, "BT_BASE0 / TSSI_OFDM_A, 32 bit"),
    (0x006e, 0x006e, "PHYTXNOI"),
    (0x0070, 0x0072, "TSSI_OFDM_G / RFRXSP1, 32 bit"),
    (0x0088, 0x008a, "JSSI0/JSSI1"),
    (0x0098, 0x009e, "SIZE01..SIZE67. SPUWKUP (0x0094) e PRETBTT (0x0096) "
                     "sono uscite dal perimetro: le scrive ora il port, la "
                     "prima come membro AC della famiglia per-PHY di "
                     "brcmsmac e la seconda col valore di questo core"),
    (0x00a8, 0x00a8, "MCASTCOOKIE"),
    (0x00b0, 0x00b0, "EXTNPHYCTL"),
    (0x00b6, 0x00b6, "BCN_LI"),
    (0x0100, 0x0100, "CHAN_5GHZ"),
    (0x0108, 0x0108, "BCMCFIFOID"),
    (0x0160, 0x017e, "PRSSID, SSID da 32 byte"),
    (0x0180, 0x0186, "temporizzazioni probe response: non nominate in b43.h, "
                     "ma b43 spegne l'offload con PRMAXTIME=1 e non le scrive "
                     "mai. Vedi docs/retrace-todo.md"),
    (0x0188, 0x0188, "PRPHYCTL"),
    (0x01c0, 0x023e, "tabelle rate: OFDMDIRECT/BASIC, CCKDIRECT/BASIC"),
    (0x0318, 0x05d3, "TKIPTSCTTAK, 50 voci da 14 byte"),
    (0x05dc, 0x05de, "0x05dc, scritta una volta sola e fuori dalle quattro "
                     "occorrenze del blocco: porta lo stesso coremask ma non "
                     "e' stabilito da dove"),
]

# Celle che erano in CORE_SHM e sono state TOLTE perche' il port le scrive:
# 0x0010 SLOTT, 0x0016 WLCOREREV, 0x001c BTSFOFF, 0x003c DEFAULTIV,
# 0x0044/0x0046 SFFBLIM/LFFBLIM, 0x005c ANTSWAP, 0x005e-0x0062 HOSTF1-3,
# 0x0064 RFATT, 0x0078 HOSTF4, 0x0080 MAXBFRAMES, 0x00c0/0x00c2 MACHW,
# 0x00d4 HOSTF5, 0x0018/0x001a BTL0/BTL1,
# 0x0240-0x02be i quattro blocchi EDCFQ, 0x05f4 PSM,
# 0x05e0-0x05f2 la coda di KEYIDXBLOCK, che il port azzera nella corsa
# 0x05e0-0x0666 di set_channel,
# 0x05d6/0x05d8 le due celle centrali del blocco chainmask, che
# b43_phy_ac_chainmask_block() scrive ora a tutti e quattro i siti.
#
# Vanno tolte, non e' facoltativo: il perimetro scarta op dal solo lato vendor,
# quindi una cella che il port emette e il perimetro scarta diventa
# un'inserzione senza controparte e rompe il confronto posizionale a quel
# punto -- il posizionale si blocca su una divergenza mentre l'LCS sale, che
# e' il sintomo. Per questo la lista va ristretta ogni volta che il port
# impara a scrivere una cella.

# Celle che b43.h nomina come del MAC ma che il PHY AC usa davvero, con la
# citazione. Restano nel denominatore.
#
# Aggiungere una voce qui puo' solo abbassare il punteggio o lasciarlo dov'e',
# mai alzarlo: tiene un'op vendor nel confronto, quindi se il port la emette
# numeratore e denominatore crescono insieme, e se non la emette cresce solo il
# denominatore. E' il verso sicuro, ed e' il motivo per cui questa lista non ha
# bisogno della cautela che serve a CORE_SHM.
PHY_ANCHE = [
    (0x008c, 0x008c, "JSSIAUX per b43.h; la legge wd_stats_tail()"),
    (0x00a0, 0x00a0, "CHAN per b43.h; e' B43_SHM_AC_CHANSPEC in "
                     "src/phy_ac.h, e write_chanspec() la scrive"),
    (0x01c0, 0x01de, "DIRMAP_A: la scansione di set_channel la legge tutta, "
                     "e prb_rsp_rate_po() ne usa otto voci per calcolare "
                     "2*voce+offset"),
    (0x0200, 0x021e, "DIRMAP_B: letta dalla scansione, e il ciclo dei dodici "
                     "rate ne usa quattro voci per i blocchi CCK"),
    (0x01e0, 0x01fe, "BBRSMAP_A: la scrive b43_phy_ac_basic_rate_map()"),
    # Celle della config BSS che emit_core_bss_config() emette come doppione del
    # core. Nessuna di queste ha una scrittura precoce nella cattura, quindi
    # toglierle non scopre op che il port non emette. Fuori resta 0x0018 (BTL0):
    # il vendor la scrive anche a #655, dentro il blocco di chip init.
    (0x0012, 0x0012, "DTIMPER, emessa da emit_core_bss_config()"),
    (0x0018, 0x0018, "BTL0: a 7 da emit_core_shm_chipinit(), a 0x012a da "
                     "emit_core_bss_config()"),
    (0x001e, 0x001e, "TIMBPOS, idem"),
    (0x0022, 0x0022, "ACKCTSPHYCTL, idem"),
    (0x0048, 0x0048, "PRSSIDLEN, idem"),
    (0x004a, 0x004a, "PRTLEN, idem"),
    (0x0160, 0x017e, "PRSSID, l'SSID da 32 byte, idem"),
    (0x0056, 0x0056, "chiude la scansione delle direct-map table"),
]

# Op che il vendor emette e che NESSUN codice b43 puo' emettere, perche'
# corrispondono a un confine di funzione che b43 non ha e non hanno effetto
# sull'hardware. Si scartano dal solo lato vendor.
#
# Non e' il perimetro, che scarta celle di altri, e non e' SOLO_PORT, che
# scarta op del port senza oracolo. E' una terza categoria e va tenuta
# corta: ogni voce e' un pezzo di obiettivo dichiarato irraggiungibile, e serve
# la prova che non ci sia niente da emettere.
#
#   MAC.BW   hook su wlc_bmac_bw_set. L'equivalente GPL in brcmsmac,
#            brcms_b_bw_set(), fa wlc_phy_bw_state_set() -- che e' `pi->bw = bw`
#            e nient'altro -- piu' un reset e un init del PHY. Nelle catture
#            fra il record e il prologo radio non c'e' nessuna scrittura di
#            registro, e il prologo radio e' quell'init, che il port fa. In b43
#            la larghezza sta in phy.chandef, che b43_phy_init() punta prima di
#            switch_analog e di b43_software_rfkill: e' gia' impostata quando il
#            PHY arriva qui, e non c'e' nulla da scrivere.
#   MARK     record del tracer, non del driver: lo scrive lo script dello
#            sweep dallo userspace ('chNN bwB', prima del rmmod del canale
#            precedente) o il modulo wl-diag stesso ('mod COMING'/'mod GOING').
#            Non c'e' nessun registro dietro, quindi nessun codice puo'
#            emetterlo. Entra nella finestra solo da quando i segmenti a freddo
#            arrivano fino a 'mod GOING'.
#   probe response offload   PRTLEN (0x004a), PRSSID (0x0160-0x017e),
#            PRSSIDLEN (0x0048), le temporizzazioni 0x0180-0x0186 e la word di
#            template RAM 0x0700. b43 non fa rispondere il firmware ai probe:
#            scrive PRMAXTIME=1 in b43_wireless_core_init() (main.c:4918) con il
#            commento che un MaxTime di un microsecondo fa sempre scattare il
#            timeout, "so we never send any probe resp". Coerentemente le tre
#            celle del template sono definite in b43.h e mai scritte -- zero usi
#            di PRSSID, PRSSIDLEN e PRTLEN in main.c -- e 0x0180-0x0186 non sono
#            nemmeno nominate. Le probe response le costruisce mac80211 in
#            software, quindi non c'e' niente da emettere e l'AP resta
#            scopribile.
#            SCELTA DEL WIP, NON UN LIMITE: l'offload si salta per ora. Quando
#            verra' implementato -- il TODO post-WIP in docs/retrace-todo.md --
#            questa voce va togliata e il confronto diventa piu' severo, che e'
#            il verso giusto. Nota che PRMAXTIME (0x0074) NON e' qui: il vendor
#            la scrive una volta con 0, b43 due volte (0 in b43_chip_init poi 1),
#            quindi la' c'e' una controparte e una divergenza di valore, non
#            un'assenza.
SOLO_VENDOR = (
    r'^MAC\.BW\b',
    r"^MARK\b",
    r'^OBJ\.WR addr=0x(?:48|4a|18[0246])(?: |$)',
    r'^OBJ\.WR addr=0x1(?:6[02468ace]|7[02468ace])(?: |$)',
    r'^TPL\.RAMW addr=0x700(?: |$)',
)

# Op che il PORT emette e che il vendor legittimamente non ha. Si scartano dal
# solo lato test, e servono perche' il punteggio penalizza le op in piu': senza
# questa lista un'op giusta ma senza controparte peserebbe come un difetto.
#
# Ogni voce va argomentata qui, come per SOLO_VENDOR: un'op senza controparte
# di solito vuol dire che il port fa qualcosa di troppo, non che la metrica
# vada aggiustata. I casi legittimi sono due -- un'op che b43 deve fare per la
# sua struttura dove wl ne fa una diversa, e un'op giusta la cui controparte
# esiste ma non e' stata catturata perche' l'hook e' arrivato dopo. La seconda
# e' temporanea per definizione: si chiude con una ricattura.
# C'e' un terzo caso legittimo, e va nominato invece di far passare le voci
# sotto uno dei due sopra: un'op che emette b43 mainline, fuori da src/, e che
# il driver stock non ha affatto. Non e' debito del port, perche' non c'e'
# niente in src/ da correggere, e non e' pagabile senza toccare main.c. Il
# criterio e' quello del perimetro -- si scarta solo cio' di cui si mostra che
# e' di qualcun altro -- applicato al lato test.
#
#   AMT.*  la address match table. Il port la scrive per via di `patches/0011`,
#          ricavata dalla cattura a freddo del DSL-3580L. Non e' un'op di
#          troppo: e' un'op giusta che una cattura senza l'hook su
#          `wlc_bmac_write_amt` non puo' contenere. Sta quindi in
#          SOLO_PORT_SENZA_CLASSE e non qui: vale solo contro una cattura che
#          la classe non la traccia, e si spegne da se' contro una che la
#          traccia -- il d6220 e il DSL oggi, l'agcombo quando sara'
#          ricatturato.
#
#   OBJ su 0x0004/0x0006 e le tre costanti su 0x0000/0x0002
#          la meta' non allineata di b43_validate_chipaccess()
#          (main.c:3612-3625). La meta' allineata, che il vendor esegue op per
#          op a #511-#522 del segmento di riferimento, NON e' qui e va
#          confrontata: sono 12 op e combaciano. Queste sono le 16 che
#          restano -- il backup e il ripristino di 0x0004/0x0006, i quattro
#          write16 del pattern, la read32 non allineata e la write32 che la
#          segue.
#
#          Le due prove. Le celle 0x0004/0x0006: il vendor non le tocca in
#          nessuna cattura -- zero occorrenze su tutti e 26 i segmenti a
#          freddo, sul tick a regime e sulla cattura del DSL -- e src/ non
#          scrive SHM sotto 0x000c, quindi un'op la' non puo' essere del port
#          nel senso che conta. Le costanti 0x1122/0x3344/0xccdd su
#          0x0000/0x0002: non compaiono come valore OBJ su quelle celle in
#          nessuna cattura, e in b43 esistono solo dentro quel self-test.
#
#          Il pattern e' legato ai valori di proposito: se le costanti di
#          mainline cambiano smette di combaciare e le op tornano a pesare,
#          che e' il verso giusto in cui sbagliare.
#
#          E' l'unica voce di questa lista che non si chiude con una
#          ricattura: il vendor quel test non lo fa e non lo fara'.
#   REG.WR 0x49c   GPIO_CONTROL del MAC, come b43 pilota i LED: leds.c fa
#          read-modify-write della cella a ogni cambio di stato, in
#          b43_leds_init(), b43_leds_exit() e dai trigger. Il vendor gli
#          stessi LED li pilota con si_gpioout sul chipcommon -- le op che il
#          PERIMETER dichiara del core -- quindi qui la controparte non c'e'
#          e non ci sara': stessa funzione, registro diverso, per struttura.
#          Compare solo con il profilo --bus (test/integration): l'harness
#          di test/unit non compila leds.c.
SOLO_PORT = (
    r'^OBJ\.(RD|WR) addr=0x0*[46] ',
    r'^OBJ\.(RD|WR) addr=0x0*[02] val=0x0*(1122|3344|ccdd)\b',
    r'^REG\.WR off=0x49c\b',
)

# Il secondo caso della lista sopra: la controparte esiste, ma la cattura non
# traccia quella classe. E' temporanea per definizione, e la condizione la
# decide la cattura invece di chi legge -- appena il lato vendor porta una op
# della classe la voce si spegne e il confronto diventa piu' severo, senza che
# nessuno debba ricordarsi di venirla a togliere. Stesso criterio di
# check_class_coverage.py e dei flag oracle_has_* di wrap.c.
SOLO_PORT_SENZA_CLASSE = (
    (r'^AMT\.', 'AMT.'),
)


def drop_solo_port(ops, vendor=()):
    """Togli dal port le op che il vendor legittimamente non ha."""
    pats = list(SOLO_PORT)
    for pat, classe in SOLO_PORT_SENZA_CLASSE:
        if not any(op.startswith(classe) for op in vendor):
            pats.append(pat)
    out, dropped = [], []
    for op in ops:
        (dropped if any(re.search(p, op) for p in pats) else out).append(op)
    return out, dropped


def drop_solo_vendor(ops):
    """Togli dal vendor le op che nessun codice b43 puo' emettere."""
    out, dropped = [], []
    for op in ops:
        (dropped if any(re.search(p, op) for p in SOLO_VENDOR) else out).append(op)
    return out, dropped


PERIMETER = [
    dict(pattern=r'^OBJ\.(RD|WR) ', core_shm=True,
         motivo="celle di shared memory che b43.h del kernel dichiara del "
                "core. Sono le sole op OBJ che si scartano: il resto della "
                "finestra resta nel denominatore anche quando e' con ogni "
                "probabilita' del core, perche' non abbiamo una prova e "
                "sbagliare in quel verso gonfierebbe il punteggio. Vedi il "
                "blocco sopra per il criterio e per il motivo per cui non si "
                "usa la raggiungibilita' da src/."),

    dict(pattern=r'^TPL\.RAMW addr=0x48\b',
         motivo="template RAM, la cella 0x0048, che resta di altri. Le altre "
                "sono uscite dal perimetro: il port scrive 0x0200 e 0x0480 "
                "dai doppioni emit_core_bss_ssid() e 0x0700 da "
                "emit_core_prb_rsp_template(), che sono il codice di main.c "
                "del kernel rispecchiato nell'harness. Questa no: le sue due "
                "occorrenze, a #12960 e #13584, non cadono a un confine che "
                "l'harness controlli -- la prima segue la coppia 0x018a/0x018c "
                "che emette src/, la seconda la spazzata dei contatori -- "
                "quindi non c'e' un posto dove metterla senza spezzare una "
                "funzione del PHY. Il PHY non tocca template RAM."),

    dict(pattern=r'^MAC\.MHF\.RD\b',
         motivo="lettura nuda delle host flags. Il PHY non le legge mai da "
                "solo: il suo unico accesso e' b43_phy_ac_mhf_maskset(), che "
                "l'harness traccia come MAC.MHF con maschera, e in src/ non "
                "c'e' nessuna chiamata a b43_hf_read(). Nel segmento compare "
                "una volta sola, a #13537, in mezzo al blocco di config MAC "
                "del core -- fra OBJ.WR 0x60 e la sospensione del MAC -- ed e' "
                "b43_hf_read() di main.c."),

    dict(pattern=r'^(OTP\.|SROMCTL\.)',
         motivo="OTP e registro di controllo della SROM: le legge il codice "
                "srom di bcma. Non sono fuori scopo -- bcma/sprom.c e' toccato "
                "da patches/0001, che aggiunge l'estrazione rev 11 -- ma sono "
                "fuori da src/, e l'harness monta il profilo di board "
                "direttamente invece di leggere la SROM. Nell'attach a freddo "
                "cadono in mezzo alla sequenza analogica, fra le dieci letture "
                "di save e il banco AFE, quindi senza questa voce il confronto "
                "posizionale si ferma la'.\n"
                "TODO: verificare che le tre op del segmento (SROMCTL.RD, "
                "OTP.RDW, OTP.INIT a #548-#551 su cold01) siano quelle che "
                "patches/0001 gia' emette. Se lo sono, lo skip e' definitivo e "
                "la finestra del confronto va fatta partire dopo -- e' a questo "
                "che serve il --range di questo strumento. Se non lo sono, e' "
                "un buco della patch."),

    dict(pattern=r'^(GPIO\.(CTL|OUT|OE) |SI\.COREREG core=0x0 off=0x8c\b)',
         led=True,
         motivo="i LED. Nel blob (wlD6220.o) sono quattro funzioni del core, "
                "tutte su chipcommon: wlc_bmac_hw_up() all'attach -- "
                "si_gpiocontrol(mask, 0), si_gpioled(mask, mask) che e' la "
                "scrittura di gpiotimeroutmask a 0x8c, si_gpioout(mask, off) "
                "con il bit alzato per i LED active-low, si_gpioouten(mask, "
                "mask) -- wlc_bmac_led() per accendere e spegnere i singoli "
                "LED al bss-up e al down (gpio 2 e gpio 10, quest'ultimo "
                "active-low: 0x0004 e 0x0400), e wlc_bmac_led_hw_deinit() al "
                "rmmod, che rilascia la maschera con out, outen e gpioled a "
                "zero. Ognuna tocca un sottoinsieme non vuoto dei pin LED "
                "della board, che l'harness ricava dalla SROM del profilo "
                "(./ac_trace led_pins BOARD) e arrivano con --led-pins; le "
                "GPIO con maschera fuori da quei pin restano nel confronto. "
                "Sul PHY non hanno effetto, e in "
                "b43 i LED li fa leds.c dai campi gpio0-3 della SROM, per la "
                "sua via (MMIO GPIO_CONTROL del MAC) e non per quella del "
                "vendor; i pin 4-15 da NVRAM ledbh glieli insegnano "
                "patches/0016-0017. Vedi docs/retrace-todo.md."),

    dict(pattern=r'^CAL\.INIT\b',
         motivo="switch di forzatura delle calibrazioni del driver stock. In "
                "questa build il record non porta ne' indirizzo ne' valore, "
                "quindi non c'e' niente da riprodurre."),
]


ADDR = re.compile(r'\baddr=(0x[0-9a-f]+)')
SPACE = re.compile(r'^([A-Z]+)\.')


def _cella(op):
    m = ADDR.search(op)
    return int(m.group(1), 16) if m else None


def _core_shm(op):
    a = _cella(op)
    if a is None:
        return False
    if any(lo <= a <= hi for lo, hi, _ in PHY_ANCHE):
        return False
    return any(lo <= a <= hi for lo, hi, _ in CORE_SHM)


GPIO_MASK = re.compile(r'^GPIO\.\w+ .*\bmask=(0x[0-9a-f]+)')


def _led(op, pins):
    m = GPIO_MASK.match(op)
    if not m:
        return True
    mask = int(m.group(1), 16)
    return mask != 0 and not mask & ~pins


def apply_perimeter(ops, led_pins=0):
    """Togli le op vendor di cui si puo' mostrare che sono di altri.

    Ritorna (dentro, scartate, celle). La terza voce e' l'insieme
    (spazio, indirizzo) scartato: va guardata, non contata, perche' se una
    cella del PHY finisse la' dentro il conto tornerebbe con l'omissione
    nascosta.
    """
    out, dropped, keys = [], [], set()
    for op in ops:
        hit = None
        for k, r in enumerate(PERIMETER):
            if not re.search(r['pattern'], op):
                continue
            if r.get('core_shm') and not _core_shm(op):
                continue
            if r.get('led') and not _led(op, led_pins):
                continue
            hit = k
            break
        if hit is None:
            out.append(op)
        else:
            dropped.append((op, hit))
            m, s = ADDR.search(op), SPACE.match(op)
            if m and s:
                keys.add((s.group(1), m.group(1)))
    return out, dropped, keys


VAL_TOK = re.compile(r'val=(?:0x[0-9a-fA-F]+|UNDEFINED)')

# Celle il cui valore nessun codice puo' prevedere, quindi si confrontano
# indirizzo, classe e posizione ma non il valore.
#
# BSLOTS e REGGAP dei quattro blocchi EDCFQ: BSLOTS e' il backoff estratto a
# caso all'inizio del contention window, e REGGAP e' AIFS + BSLOTS. Misurati su
# tutti e 43 i segmenti e tutte e quattro le code, 172 punti: BSLOTS cade
# uniformemente in [0, CWMIN] -- 0..15 su best effort e background, 0..7 su
# video, 0..3 su voce -- e la relazione di REGGAP regge su ognuno dei 172.
# Non e' un'eccezione che nasconde un difetto: e' una cella nondeterministica,
# e pretenderne il valore vorrebbe dire indovinare un numero casuale.
VAL_NONDET = [
    (0x0240, 0x02be, 0x0a, "BSLOTS, backoff estratto a caso"),
    (0x0240, 0x02be, 0x0c, "REGGAP = AIFS + BSLOTS"),
]


# Celle il cui valore si confronta con una tolleranza, perche' il port lo
# calcola e l'ultimo bit non e' riproducibile.
#
# PHY 0x?a1 e' il coefficiente b della correzione RX IQ, scritto su 10 bit in
# complemento a due. Il port riproduce esattamente gli accumulatori e il
# coefficiente a -- quello combacia su tutti i punti misurati -- e sbaglia b di
# un LSB su parte dei casi.
#
# Non e' la regola di arrotondamento, ed e' stato verificato da due lati.
# brcmsmac, che porta lo stesso algoritmo in wlc_phy_calc_rx_iq_comp_nphy(),
# usa int_sqrt liscia: un floor, nessuna tabella. E spostando la soglia di
# arrotondamento da 0.40 a 0.98 il massimo e' 96 punti esatti su 148, con 38
# ancora bassi e 10 alti: una soglia sbagliata darebbe un errore di un segno
# solo, e invece scambia i bassi con gli alti. Il difetto sta nel valore sotto
# radice, dell'ordine dello 0.01%.
#
# LA SOGLIA E' LA MISURA DEL RESIDUO, NON UNA FRANCHIGIA DI COMODO, e va
# rifatta a ogni cambio del modello di solve. Se e' piu' larga del residuo
# smette di coprire l'ultimo bit e diventa una fascia dove un errore
# strutturale si nasconde: una soglia a +-4 e' larga il doppio della
# differenza fra sommare gli accumulatori e mediare i coefficienti -- cioe'
# fra il modello sbagliato e quello giusto -- e la terrebbe invisibile al
# confronto posizionale.
#
# Misura corrente, 144 scritture reali del port contro il vendor su tutti e 26
# i segmenti a freddo e tutti e 52 gli up a caldo:
#
#   -1 LSB: 12    0: 114    +1 LSB: 18    massimo |residuo|: 1
#
#   gruppo         -1    0   +1
#   freddo bw20     0   30    2
#   freddo bw40     0   13    1
#   freddo bw80     0    6    0
#   caldo  bw20    11   46    7
#   caldo  bw40     1   13    2
#   caldo  bw80     0    6    6
#
# Simmetrica, e non per prudenza: a freddo il residuo e' solo +1, a caldo e'
# bilaterale. Dedurla dal solo sweep a freddo la farebbe scrivere asimmetrica e
# romperebbe dodici punti a caldo.
#
# Dei 30 residui non nulli, 25 sono sulla catena 1 e 5 sulla catena 0: il
# debito e' quasi tutto la' , e agcombo dice che non e' una regola per catena
# (vedi docs/retrace-todo.md). Per rifare la misura, per ogni segmento si
# confronta l'ultima scrittura di 0x?a1 nella cattura con quella del port.
#
# La catena 2 e' dichiarata senza copertura: nessuna cattura del d6220 la
# scrive, quindi la sua voce non e' mai stata esercitata. Va verificata su
# agcombo o togliata, non tenuta per simmetria.
VAL_TOLLERANZA = [
    (0x06a1, 10, 1, "coefficiente b della RX IQ, catena 0"),
    (0x08a1, 10, 1, "coefficiente b della RX IQ, catena 1"),
    (0x0aa1, 10, 1, "coefficiente b della RX IQ, catena 2, senza copertura"),
]


def _s(v: int, bits: int) -> int:
    """Interpreta @v come intero con segno su @bits bit."""
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def val_entro_tolleranza(v: str, t: str) -> bool:
    """Le due op scrivono la stessa cella con valori entro la tolleranza?"""
    mv = re.match(r'PHY\.WR\s+addr=0x([0-9a-fA-F]+)\s+val=0x([0-9a-fA-F]+)', v)
    mt = re.match(r'PHY\.WR\s+addr=0x([0-9a-fA-F]+)\s+val=0x([0-9a-fA-F]+)', t)
    if not mv or not mt or mv.group(1) != mt.group(1):
        return False
    a = int(mv.group(1), 16)
    for addr, bits, tol, _ in VAL_TOLLERANZA:
        if a != addr:
            continue
        d = _s(int(mv.group(2), 16), bits) - _s(int(mt.group(2), 16), bits)
        return abs(d) <= tol
    return False


def val_nondet(op: str) -> bool:
    """L'op scrive una cella il cui valore non e' prevedibile?"""
    m = re.match(r'OBJ\.WR\s+addr=0x([0-9a-fA-F]+)', op)
    if not m:
        return False
    a = int(m.group(1), 16)
    return any(lo <= a <= hi and (a - lo) % 0x20 == off
               for lo, hi, off, _ in VAL_NONDET)

RET_SUFFIX = re.compile(r'\s+ret=0x[0-9a-fA-F]+')

MASKED_OP = re.compile(r'^(\S+(?: addr=\S+)?) val=(0x[0-9a-fA-F]+) mask=(0x[0-9a-fA-F]+)$')
PLAIN_OP = re.compile(r'^(\S+(?: addr=\S+)?) val=(0x[0-9a-fA-F]+)$')

def val_entro_maschera(v: str, t: str) -> bool:
    """Un'op del vendor con mask contro una del port a valore pieno: la write
    in cui unfold_bus() ha svolto un MOD, o una MAC.MCTRL, che il vendor traccia
    come maskset e il bus come scrittura intera. Il vendor vincola solo i bit
    della mask; gli altri li ha letti, e il port li ha letti a sua volta
    dall'oracolo, che per quelle letture non ha un valore."""
    mv, mt = MASKED_OP.match(v), PLAIN_OP.match(t)
    if not (mv and mt) or mv.group(1) != mt.group(1):
        return False
    mask = int(mv.group(3), 16)
    return (int(mv.group(2), 16) & mask) == (int(mt.group(2), 16) & mask)

def ops_equal(v: str, t: str) -> bool:
    """Confronto op-per-op con il valore letto trattato come wildcard quando il
    vendor non lo ha registrato.

    Le catture prodotte senza "capture ret val" loggano ogni read come
    val=UNDEFINED: la' il valore non e' confrontabile e va ignorato, mentre
    indirizzo e classe di op restano vincolanti. Sulle catture con i RETVAL
    ripiegati (`trace_filter.py --retvals`) il valore c'e' e viene
    confrontato."""
    # Le op read-modify-write di bcma (PMU.RC, GPIO.*) portano nel trace vendor
    # il valore riletto dopo la modifica; gli stub bcma dell'harness non
    # modellano quel readback, quindi il suffisso non e' confrontabile.
    v = RET_SUFFIX.sub('', v)
    if v == t:
        return True
    if val_entro_maschera(v, t):
        return True
    if val_nondet(v):
        return VAL_TOK.sub('val=*', v, count=1) == VAL_TOK.sub('val=*', t, count=1)
    if val_entro_tolleranza(v, t):
        return True
    if 'val=UNDEFINED' in v:
        return VAL_TOK.sub('val=*', v, count=1) == VAL_TOK.sub('val=*', t, count=1)
    return False

def extract_episode(raw: str) -> int:
    m = re.search(r'#(\d+)', raw)
    return int(m.group(1)) if m else -1

def op_forms(raws, profile):
    """Le op normalizzate di una traccia: una per riga, o quelle del profilo
    `bus`, che svolge i MOD e conosce la coppia RAD.MOD + RAD.RD."""
    if profile == 'bus':
        return tracelib.unfold_bus_seq(raws)
    return [normalize_op(r) for r in raws]

def load_vendor(path, ep_range, profile=None, espanse=None):
    lo, hi = ep_range or (0, 10**9)
    raws = []
    for line in open(path):
        m = VENDOR_LINE.match(line)
        if not m:
            continue
        ep = extract_episode(line)
        if not (lo <= ep <= hi):
            continue
        raws.append(m.group(1))
    raws = expand_bulk_heads(raws, espanse)
    return drop_shadow_ops(resolve_wide_reads(op_forms(raws, profile)))

def load_test(path, profile=None):
    raws = [m.group(1) for line in open(path) for m in [TEST_LINE.match(line)] if m]
    return op_forms(raws, profile)

def find_offset(test, target_op):
    """Return the index of `target_op` in test, or -1.

    Usa ops_equal, non l'uguaglianza esatta: se il vendor non ha registrato il
    valore letto l'ancora non deve dipendere da quello."""
    for i, op in enumerate(test):
        if ops_equal(target_op, op):
            return i
    return -1

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('vendor')
    ap.add_argument('test')
    ap.add_argument('--range', help='LO:HI vendor episode range')
    ap.add_argument('--bus', action='store_true',
                    help='profilo bus: MOD svolti in RD+WR, TBL.* scartati; '
                         'per una traccia presa al bus MMIO (test/integration)')
    ap.add_argument('--led-pins', type=lambda v: int(v, 0), default=None,
                    help='pin GPIO dei LED della board, da ./ac_trace '
                         'led_pins BOARD. Senza, il blocco LED del vendor '
                         'resta nel confronto')
    ap.add_argument('--auto-align', action='store_true',
                    help='skip test prologue by aligning on vendor[0]')
    ap.add_argument('--align-on', help='align test on this exact op string')
    ap.add_argument('--senza-perimetro', action='store_true',
                    help='confronta ANCHE le op fuori dal perimetro del PHY. '
                         'Il risultato non e\' una misura del port: si ferma '
                         'sulla prima op del core b43, che l\'harness non '
                         'compila. Serve per ispezionare una cattura, non per '
                         'dare un numero')
    ap.add_argument('--perimetro-addr', action='store_true',
                    help='elenca le celle scartate dal perimetro: l\'insieme '
                         'va guardato, perche\' una cella che il port dovrebbe '
                         'toccare e non tocca finisce qui')
    args = ap.parse_args()

    rng = None
    if args.range:
        lo, hi = args.range.split(':')
        rng = (int(lo), int(hi))

    profile = 'bus' if args.bus else None
    espanse = {}
    vendor = load_vendor(args.vendor, rng, profile, espanse)
    test = load_test(args.test, profile)
    if espanse:
        print(f"bulk espanse: {format_espanse(espanse)}")

    vendor, sv = drop_solo_vendor(vendor)
    test, sp = drop_solo_port(test, vendor)
    if sv:
        print(f"solo vendor: {len(sv)} op scartate perche' nessun codice b43 "
              f"puo' emetterle; vedi SOLO_VENDOR in questo file")
    if sp:
        print(f"solo port: {len(sp)} op del port scartate perche' l'oracolo "
              f"non le puo' contenere; vedi SOLO_PORT in questo file")

    if not args.senza_perimetro:
        if args.led_pins is None:
            print("pin LED non dati: le op LED del vendor restano nel "
                  "confronto; vedi --led-pins")
        vendor, outside, keys = apply_perimeter(vendor, args.led_pins or 0)
        print(f"fuori perimetro: {len(outside)} op vendor scartate su "
              f"{len(keys)} celle dichiarate di altri; "
              f"vedi PERIMETER in questo file")
        if args.perimetro_addr:
            for space in sorted({s for s, _ in keys}):
                a = sorted((x for s, x in keys if s == space),
                           key=lambda v: int(v, 16))
                print(f"  {space}: " + " ".join(a))

    if args.align_on:
        off = find_offset(test, args.align_on)
        if off < 0:
            print(f"align-on: op not found in test: {args.align_on}")
            return 2
        print(f"aligning test at offset {off} (--align-on)")
        test = test[off:]
    elif args.auto_align and vendor:
        off = find_offset(test, vendor[0])
        if off < 0:
            print(f"auto-align: vendor[0] not found in test: {vendor[0]}")
        else:
            print(f"aligning test at offset {off} (auto: '{vendor[0]}')")
            test = test[off:]

    print(f"vendor: {len(vendor)} ops")
    print(f"test:   {len(test)} ops")

    n = min(len(vendor), len(test))
    mismatches = 0
    for i in range(n):
        if not ops_equal(vendor[i], test[i]):
            mismatches += 1
            if mismatches <= 20:
                print(f"  @{i}:")
                print(f"    vendor: {vendor[i]}")
                print(f"    test:   {test[i]}")
    if len(vendor) != len(test):
        print(f"length differs: vendor={len(vendor)} test={len(test)}")

    if mismatches == 0 and len(vendor) == len(test):
        print("MATCH")
        return 0
    print(f"total mismatches (compared prefix): {mismatches}")
    return 1

if __name__ == '__main__':
    sys.exit(main())
