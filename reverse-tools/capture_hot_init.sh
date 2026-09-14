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
#   sh capture_plan.sh 20a          prima metà dei 20 MHz  (UNII-1 + UNII-2)
#   sh capture_plan.sh 20b          seconda metà dei 20 MHz (UNII-2e + UNII-3)
#   sh capture_plan.sh 40           tutti i 40 MHz
#   sh capture_plan.sh 80           tutti gli 80 MHz
#   sh capture_plan.sh 20dfs        i soli canali con guardia radar, 20 MHz
#   sh capture_plan.sh 20meteo      i soli 5600-5650, che vogliono 600 s
#   sh capture_plan.sh 20a wl1 10   interfaccia e attesa espliciti
#   sh capture_plan.sh 20a wl1 10 test-ap   con SSID, per programmare il BSS
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

# A 40 e 80 MHz il driver vuole il CANALE BASSO del blocco, non quello
# centrale: 36/40 e non 38/40, 36/80 e non 42/80. Col centrale rifiuta.
# Il prefisso `5g` e' obbligatorio, e cosi' `-i $IF`: `wl chanspec` senza
# interfaccia agisce sul core 2.4 GHz band-locked.
C20A="36 40 44 48 52 56 60 64"
C20B="100 104 108 112 116 120 124 128 132 136 140 144 149 153 157 161 165"
C40="36 44 52 60 100 108 116 124 132 140 149 157"
C80="36 52 100 116 132 149"

# Le fasi DFS sono i sottoinsiemi di quelle sopra su cui il canale porta la
# guardia radar, cioe' quelli su cui `up` non arriva al BSS finche' il CAC non
# e' passato: 60 s, e 600 s sui blocchi che toccano i 5600-5650 MHz del radar
# meteo (20: 120 124 128; 40: 116 124; 80: 116), che stanno in una fase a
# parte perche' l'attesa che serve a loro sprecherebbe un'ora sulle altre.
# Che il check sia pendente lo dice la cattura, non il piano: sui segmenti che
# lo sono reverse-tools/cac_polls.py trova i turni di poll, e sugli altri no.
C20DFS="52 56 60 64 100 104 108 112 116 132 136 140"
C20METEO="120 124 128"
C40DFS="52 60 100 108 132 140"
C40METEO="116 124"
C80DFS="52 100 132"
C80METEO="116"

case "$FASE" in
    20a) LIST="$C20A"; BW=20 ;;
    20b) LIST="$C20B"; BW=20 ;;
    40)  LIST="$C40";  BW=40 ;;
    80)  LIST="$C80";  BW=80 ;;
    20dfs)   LIST="$C20DFS";   BW=20 ;;
    20meteo) LIST="$C20METEO"; BW=20 ;;
    40dfs)   LIST="$C40DFS";   BW=40 ;;
    40meteo) LIST="$C40METEO"; BW=40 ;;
    80dfs)   LIST="$C80DFS";   BW=80 ;;
    80meteo) LIST="$C80METEO"; BW=80 ;;
    *)   echo "uso: sh capture_plan.sh {20a|20b|40|80|20dfs|20meteo|40dfs|40meteo|80dfs|80meteo} [interfaccia] [attesa]" >&2
         exit 1 ;;
esac

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
bss_su() {
    [ -n "$SSID" ] || return 0
    wl -i "$IF" ssid "$SSID" > /dev/null 2>&1
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
    bss_su
    wl -i "$IF" up
    sleep "$SETTLE"
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
