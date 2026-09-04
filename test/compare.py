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

VENDOR_LINE = re.compile(
    r'^\s*[0-9.]+\s+#\d+\s+cpu\d+\s+(.+?)\s*(?:;.*)?$'
)
TEST_LINE = re.compile(r'^cpu\d+\s+(.+?)\s*$')

# Vendor uses PHY.OR (set-in) and PHY.AND (clear-in) alongside PHY.MOD.
# Both are folded to the PHY.MOD single-op form that the wrapper emits:
#   phy_set(X) → PHY.MOD val=X mask=0
#   phy_mask(K) → PHY.MOD val=K mask=0    (K = ~clr in kernel terms)
# PHY.OR line looks like "PHY.OR ... val=<current_or_or_in> (set X)":
#   the (set X) group gives the OR-in bits directly → val=X mask=0.
# PHY.AND line "PHY.AND ... val=<masked> (clr X)": clr X gives the
# bits to clear → val=~X (the kmask) mask=0.
PHY_OR  = re.compile(r'^PHY\.OR\s+addr=(0x[0-9a-f]+)\s+val=0x[0-9a-f]+\s*\(set\s+(0x[0-9a-f]+)\)')
PHY_AND = re.compile(r'^PHY\.AND\s+addr=(0x[0-9a-f]+)\s+val=0x[0-9a-f]+\s*\(clr\s+(0x[0-9a-f]+)\)')

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
    0x08c,                 # gpiopull              <- GPIO.CTL
}
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

def drop_shadow_ops(ops):
    """Scarta le SI.COREREG che implementano l'op di alto livello precedente."""
    out = []
    parent = False
    for op in ops:
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

HEXNUM = re.compile(r'\b(0x[0-9a-fA-F]+)\b')

WS = re.compile(r'\s+')

def canon_ws(op: str) -> str:
    """Collassa lo spazio bianco: la spaziatura fra mnemonico e operandi non e'
    informazione, e i due lati la formattano diversamente."""
    return WS.sub(' ', op).strip()

def canon_values(op: str) -> str:
    """Porta ogni letterale esadecimale a una forma canonica senza zeri di
    riempimento.

    Il tracer vendor stampa i valori di ritorno a 32 bit (val=0x00000000) mentre
    l'harness li stampa a 16 (val=0x0000): sono lo stesso numero e il confronto
    non deve dipendere dalla larghezza del campo. Gli indirizzi passano per la
    stessa normalizzazione, che e' innocua perche' entrambi i lati la ricevono."""
    return HEXNUM.sub(lambda m: '0x%x' % int(m.group(1), 16), op)

def normalize_op(op: str) -> str:
    m = PHY_OR.match(op)
    if m:
        addr, setbits = m.groups()
        return canon_ws(canon_values(
            f"PHY.MOD  addr={addr} val={setbits} mask=0x0000"))
    m = PHY_AND.match(op)
    if m:
        addr, clrbits = m.groups()
        return canon_ws(canon_values(
            f"PHY.MOD  addr={addr} val=0x{(~int(clrbits, 16)) & 0xffff:04x} mask=0x0000"))
    # L'harness nomina l'abilitazione GPIO come il simbolo bcma
    # (bcma_chipco_gpio_outen), il tracer vendor come il registro (OE).
    op = re.sub(r'^GPIO\.OUTEN\b', 'GPIO.OE', op)
    return canon_ws(canon_values(op))

# ---------------------------------------------------------------------------
# LA METRICA PENALIZZA LE OP IN PIU'
#
# Il denominatore del punteggio e' l'unione: tutte le op di wl piu' quelle che
# il port emette e wl no. Fa 100% solo se i due flussi coincidono.
#
# Prima il denominatore erano le sole op di wl, e le inserzioni del port erano
# gratis. Non lo sono: il driver deve emettere le op di wl, non le sue, e un
# accesso di troppo a un registro e' un difetto quanto uno mancante -- puo'
# lasciare l'hardware in uno stato che il driver stock non produce mai. Con la
# vecchia metrica togliere 6000 op spurie dal port non muoveva il numero di
# un'unita', il che rendeva invisibile l'unico tipo di progresso che quel giorno
# si stava facendo.
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
    (0x0018, 0x001a, "BTL0/BTL1"),
    (0x001e, 0x001e, "TIMBPOS"),
    (0x0022, 0x0022, "ACKCTSPHYCTL"),
    (0x0030, 0x0030, "TXFCUR"),
    (0x0034, 0x0034, "RXPADOFF"),
    (0x003e, 0x003e, "NRRXTRANS"),
    (0x0040, 0x0042, "UCODESTAT/FWCAPA"),
    (0x0048, 0x0048, "PRSSIDLEN"),
    (0x004a, 0x004c, "PRTLEN/NOSLPZNATDTIM"),
    (0x0050, 0x0056, "PHYVER/PHYTYPE/BEACPHYCTL/KTP"),
    (0x0058, 0x0058, "TSSI_CCK, la meta' che il port non scrive: la 0x005a "
                     "la scrive channel_setup()"),
    (0x0066, 0x0066, "RADAR"),
    (0x0068, 0x006a, "BT_BASE0 / TSSI_OFDM_A, 32 bit"),
    (0x006e, 0x006e, "PHYTXNOI"),
    (0x0070, 0x0072, "TSSI_OFDM_G / RFRXSP1, 32 bit"),
    (0x0074, 0x0074, "PRMAXTIME"),
    (0x0088, 0x008a, "JSSI0/JSSI1"),
    (0x0094, 0x009e, "SPUWKUP/PRETBTT/SIZE01..SIZE67"),
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
    (0x0242, 0x02be, "EDCFQ, code 1..3"),
    (0x0318, 0x05d3, "TKIPTSCTTAK, 50 voci da 14 byte"),
    (0x05d4, 0x05de, "KEYIDXBLOCK, la parte che il port non scrive"),
]

# Celle che erano in CORE_SHM e sono state TOLTE perche' il port le scrive:
# 0x0010 SLOTT, 0x0016 WLCOREREV, 0x001c BTSFOFF, 0x003c DEFAULTIV,
# 0x0044/0x0046 SFFBLIM/LFFBLIM, 0x005c ANTSWAP, 0x005e-0x0062 HOSTF1-3,
# 0x0064 RFATT, 0x0078 HOSTF4, 0x0080 MAXBFRAMES, 0x00c0/0x00c2 MACHW,
# 0x00d4 HOSTF5, 0x0240 EDCFQ base, 0x05f4 PSM,
# 0x05e0-0x05f2 la coda di KEYIDXBLOCK, che il port azzera nella corsa
# 0x05e0-0x0666 di set_channel.
#
# Vanno tolte, non e' facoltativo: il perimetro scarta op dal solo lato vendor,
# quindi una cella che il port emette e il perimetro scarta diventa
# un'inserzione senza controparte e rompe il confronto posizionale a quel
# punto. E' successo -- il posizionale e' rimasto a @66 mentre l'LCS saliva --
# ed e' la ragione per cui questa lista va ristretta ogni volta che il port
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
SOLO_VENDOR = (r'^MAC\.BW\b',)

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
#   AMT.*  la address match table. Il port la scrive per via di `patches/0011`,
#          ricavata dalla cattura a freddo del DSL-3580L; le catture del d6220
#          non la hanno perche' l'hook su `wlc_bmac_write_amt` e' stato aggiunto
#          dopo che sono state prese. NON e' un'op di troppo: e' un'op giusta
#          senza oracolo, e ci resta finche' non c'e' un retrace del d6220 con
#          quell'hook. Quel giorno questa voce va togliata e il confronto
#          diventa piu' severo, che e' il verso giusto.
SOLO_PORT = (r'^AMT\.',)


def drop_solo_port(ops):
    """Togli dal port le op che il vendor legittimamente non ha."""
    out, dropped = [], []
    for op in ops:
        (dropped if any(re.search(p, op) for p in SOLO_PORT) else out).append(op)
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

    dict(pattern=r'^TPL\.RAMW\b',
         motivo="template RAM: i template dei frame, che b43 scrive in "
                "b43_write_template_common() e in "
                "b43_write_mac_bssid_templates(). Il PHY "
                "non ne tocca nessuna cella."),

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


def apply_perimeter(ops):
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

RET_SUFFIX = re.compile(r'\s+ret=0x[0-9a-fA-F]+')

def ops_equal(v: str, t: str) -> bool:
    """Confronto op-per-op con il valore letto trattato come wildcard quando il
    vendor non lo ha registrato.

    Le catture prodotte senza "capture ret val" loggano ogni read come
    val=UNDEFINED: la' il valore non e' confrontabile e va ignorato, mentre
    indirizzo e classe di op restano vincolanti. Sulle catture con i RETVAL
    ripiegati (merge_retvals.py) il valore c'e' e viene confrontato."""
    # Le op read-modify-write di bcma (PMU.RC, GPIO.*) portano nel trace vendor
    # il valore riletto dopo la modifica; gli stub bcma dell'harness non
    # modellano quel readback, quindi il suffisso non e' confrontabile.
    v = RET_SUFFIX.sub('', v)
    if v == t:
        return True
    if 'val=UNDEFINED' in v:
        return VAL_TOK.sub('val=*', v, count=1) == VAL_TOK.sub('val=*', t, count=1)
    return False

def extract_episode(raw: str) -> int:
    m = re.search(r'#(\d+)', raw)
    return int(m.group(1)) if m else -1

def load_vendor(path, ep_range):
    lo, hi = ep_range or (0, 10**9)
    out = []
    for line in open(path):
        m = VENDOR_LINE.match(line)
        if not m:
            continue
        ep = extract_episode(line)
        if not (lo <= ep <= hi):
            continue
        out.append(normalize_op(m.group(1)))
    return drop_shadow_ops(out)

def load_test(path):
    out = []
    for line in open(path):
        m = TEST_LINE.match(line)
        if m:
            out.append(normalize_op(m.group(1)))
    return out

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

    vendor = load_vendor(args.vendor, rng)
    test = load_test(args.test)

    vendor, sv = drop_solo_vendor(vendor)
    test, sp = drop_solo_port(test)
    if sv:
        print(f"solo vendor: {len(sv)} op scartate perche' nessun codice b43 "
              f"puo' emetterle; vedi SOLO_VENDOR in questo file")
    if sp:
        print(f"solo port: {len(sp)} op del port scartate perche' l'oracolo "
              f"non le puo' contenere; vedi SOLO_PORT in questo file")

    if not args.senza_perimetro:
        vendor, outside, keys = apply_perimeter(vendor)
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
