#!/bin/sh
# Piano di cattura: sweep di canali e larghezze, da splittare a posteriori sui
# record CHANSPEC con `reverse-tools/split_trace.py --on chanspec`.
#
# Presupposti:
#   - wl_diag.ko armato (arm=1) e listener TCP attivo sull'host;
#   - il modulo wl resta caricato per tutta la procedura.
#
# Il ciclo e' {chanspec; up; attesa; down}: ogni canale ottiene un DOWN->UP,
# non un attach. Su 3.4 la traccia puo' contenere 1 attach iniziale dal
# re-probe PCI + N down->up; su 2.6.30 solo N down->up, perche' il rescan
# scarica wl. I segmenti vanno confrontati col gate `switch_channel` e
# AC_FIRST_INIT=0, non con `full` che modella l'attach.
#
# L'attesa dopo `up` lascia completare le calibrazioni asincrone (crsmin, PAPD,
# fdiqi), che sono quelle che leggono da shared memory e template RAM.
#
# LE FASI SONO SEPARATE di proposito: una run intera produce troppi byte per
# raccoglierli in un colpo. Una fase per volta, ognuna col suo file di traccia.
#
# Uso:
#   sh capture_plan.sh 20           i 20 MHz SENZA guardia radar
#   sh capture_plan.sh 40           i 40 MHz senza guardia radar
#   sh capture_plan.sh 80           gli 80 MHz senza guardia radar
#   sh capture_plan.sh 20dfs        i soli canali con guardia radar, 20 MHz
#   sh capture_plan.sh 20meteo      i soli 5600-5650, che vogliono 600 s
#   sh capture_plan.sh 20 wl1 10    interfaccia e attesa espliciti
#   sh capture_plan.sh 20 wl1 10 test-ap    con SSID, per programmare il BSS
#
# I gruppi sono disgiunti e stanno in capture_profiles.sh, che va copiato sul
# device accanto a questo script.
#
# Questo e' lo sweep A CALDO: un solo insmod e N cicli down->up. Per gli init a
# freddo, e per l'attach, lo strumento e' cold_capture.sh, che ricarica `wl` a
# ogni canale con gli hook gia' armati dal MODULE_STATE_COMING.
#
# Niente `set -u`: il busybox di questi firmware non lo gestisce.
#
# Sui canali DFS (52-64 e 100-140) il rivelatore radar interroga PHY 0x0253/0x0254
# in continuo, fino all'85% del volume. Per lo sweep di massa conviene armare con
#   insmod wl_diag.ko arm=1 skipphyrd="0x253,0x254"
# e fare a parte due o tre catture DFS senza filtro.

FASE="$1"
IF="$2"
SETTLE="$3"
SSID="$4"
[ -n "$IF" ] || IF=wl1
[ -n "$SETTLE" ] || SETTLE=10
# Attesa dopo `bss up`, variabile d'ambiente come nello script a freddo. NON
# legata a SETTLE: serve solo a lasciare in traccia il blocco BSS e qualche
# giro di watchdog, mentre SETTLE deve coprire il CAC sui canali con guardia
# radar. Legarle raddoppierebbe un'attesa da 70 s per niente.
[ -n "$SETTLE_BSS" ] || SETTLE_BSS=10

# Il prefisso `5g` e' obbligatorio, e cosi' `-i $IF`: `wl chanspec` senza
# interfaccia agisce sul core 2.4 GHz band-locked. Le liste, e la regola per
# cui i gruppi con guardia radar sono disgiunti da quelli senza, stanno in
# capture_profiles.sh.
#
# Che il check radar sia pendente lo dice la cattura, non il piano: sui
# segmenti che lo sono reverse-tools/cac_polls.py trova i turni di poll.
DIR="${0%/*}"
[ "$DIR" = "$0" ] && DIR=.
if [ ! -f "$DIR/capture_profiles.sh" ]; then
    echo "manca $DIR/capture_profiles.sh: va copiato sul device accanto a questo script" >&2
    exit 1
fi
. "$DIR/capture_profiles.sh"

if ! profilo "$FASE"; then
    echo "uso: sh capture_plan.sh {$PROFILI} [interfaccia] [attesa] [ssid]" >&2
    exit 1
fi

case "$SETTLE" in
    ''|*[!0-9]*) echo "attesa non numerica: '$SETTLE'" >&2; exit 1 ;;
esac

if ! wl -i "$IF" status >/dev/null 2>&1; then
    echo "interfaccia '$IF' non risponde a 'wl -i $IF status'" >&2
    exit 1
fi

# /dev/kmsg e' read-only su alcuni firmware, e busybox riporta l'errore di
# redirezione anche con 2>/dev/null: si prova una volta e si decide.
KMSG=
if { echo "wl_diag: capture_plan" > /dev/kmsg ; } 2>/dev/null; then
    KMSG=/dev/kmsg
fi

emit() {
    if [ -n "$KMSG" ]; then
        echo "wl_diag: === $1 ===" > "$KMSG"
    fi
    return 0
}

# Solo builtin della shell piu' `wl` e `sleep`: su questi busybox mancano head,
# awk e altri, e uno script che li usa muore a meta' senza dirlo.

# Il BSS. Senza associarsi mancano quattro tabelle intere (0x0e, 0x42, 0x62,
# 0x82) e 0x40/0x60 restano a un core invece di tre: 1638 parole contro 3740 di
# una cattura to-bss. Con SSID passato si imposta, altrimenti si salta.
# DA VERIFICARE sul device: `wl ssid` e' la via scelta, ma se su questo firmware
# serve altro (`wl ap 1`, o nvram + wlconf) va corretto qui.
# L'SSID si imposta PRIMA di `up`, la BSS si porta su DOPO. Che i due passi
# siano entrambi necessari e' misurato e non dedotto: uno sweep a caldo con il
# solo `wl ssid` ha prodotto, sullo stesso canale, esattamente gli stessi 1654
# TBL.WR di uno senza SSID. `wl ssid` da solo non porta su niente.
bss_ssid() {
    [ -n "$SSID" ] || return 0
    wl -i "$IF" ssid "$SSID" > /dev/null 2>&1
    return 0
}

bss_su() {
    [ -n "$SSID" ] || return 0
    wl -i "$IF" bss up > /dev/null 2>&1
    sleep "$SETTLE_BSS"
    return 0
}

imposta() {
    cs="5g$1/$BW"
    if ! wl -i "$IF" chanspec "$cs" > /dev/null 2>&1; then
        msg=`wl -i "$IF" chanspec "$cs" 2>&1`
        echo "salto $cs: $msg"
        return 1
    fi
    return 0
}

ciclo() {
    emit "chanspec $cs"
    echo "--- $cs"
    bss_ssid
    wl -i "$IF" up
    sleep "$SETTLE"
    bss_su
    wl -i "$IF" down
    sleep 1
}

emit "inizio fase $FASE"
wl -i "$IF" down          # sempre, qualunque sia lo stato di partenza
sleep 1

for c in $LIST; do
    imposta "$c" || continue
    ciclo
done

emit "fine fase $FASE"
echo "fase $FASE completata. Splitta la traccia sui record CHANSPEC."
