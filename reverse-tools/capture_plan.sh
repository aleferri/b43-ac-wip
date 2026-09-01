#!/bin/sh
# Piano di cattura: sweep di canali e larghezze, da splittare a posteriori sui
# record CHANSPEC con reverse-tools/split_by_chanspec.py.
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
#   sh capture_plan.sh 20a wl1 10   interfaccia e attesa espliciti
#   sh capture_plan.sh 20a wl1 10 test-ap   con SSID, per programmare il BSS
#
# Per gli init A FREDDO questo non e' lo strumento: vedi cold_capture.sh, che
# ricarica `wl` a ogni canale e prende anche l'attach. Qui la cal si forza sul
# flag, e quello da' una cal completa dentro un phy_init a caldo -- non le
# costanti di fase di attach.
#
# LA CAL COMPLETA non si forza col remove/rescan del device, che una volta su due
# non riesce: si arma wl_diag con full_init_off=<offset>, e lo stub azzera il byte
# "gia' calibrato" della struct pi a ogni cal_init. 251 su 7.14.89, 227 su 6.30.
#
#   insmod wl_diag.ko arm=1 skipphyrd="0x253,0x254" full_init_off=251
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
C20B="100 104 108 112 116 120 124 128 132 136 140"
C40="36 44 52 60 100 108 116 124 132"
C80="36 52 100 116"

case "$FASE" in
    20a) LIST="$C20A"; BW=20 ;;
    20b) LIST="$C20B"; BW=20 ;;
    40)  LIST="$C40";  BW=40 ;;
    80)  LIST="$C80";  BW=80 ;;
    *)   echo "uso: sh capture_plan.sh {20a|20b|40|80} [interfaccia] [attesa]" >&2
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

# Ogni combinazione va provata in DUE modi, cal completa e a caldo:
# l'interruttore e' a runtime proprio per poterli alternare dopo un solo insmod.
FORCE=/sys/module/wl_diag/parameters/force_full_init

forza() {
    if [ -w "$FORCE" ]; then
        echo "$1" > "$FORCE"
    else
        echo "ATTENZIONE: $FORCE non scrivibile, ciclo NON forzato" >&2
    fi
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
    forza "$1"
    emit "chanspec $cs force=$1"
    echo "--- $cs force=$1"
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
    ciclo 1        # cal completa forzata
    ciclo 0        # a caldo
done

# Si lascia l'interruttore a 1: leggerlo a fine corsa dopo un `forza 0` darebbe
# sempre 0 e non direbbe nulla. Comunque la traccia lo porta nei record CAL.INIT.
forza 1

emit "fine fase $FASE"
echo "fase $FASE completata. Splitta la traccia sui record CHANSPEC."
