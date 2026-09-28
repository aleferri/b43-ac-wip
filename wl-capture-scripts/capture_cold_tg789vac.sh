#!/bin/sh
# Cattura di init A FREDDO sul Technicolor TG789vac v2, un ciclo per canale.
# Stesso ciclo di capture_cold_init.sh -- MARK ; rmmod wl ; insmod wl ;
# chanspec ; [ssid] ; up ; attesa ; [bss up ; attesa] ; down -- ma su questo
# firmware il ricarico di `wl` ha quattro condizioni che lo script generico non
# conosce, e ognuna da sola manda il router in crash o produce una cattura che
# mente. Sono tutte documentate in router-data/tg789vac-v2/README.md.
#
# 1. wl, wlemf e wfd NON stanno in vmalloc: il kernel Technicolor li mette in
#    una regione fisica riservata da 5 MiB con un cursore che avanza e non
#    torna mai indietro. Dopo un rmmod il blocco di wl (3.91 MiB) non e'
#    disponibile e il secondo insmod ottiene `Load wl module core (null)`.
#    wl_diag riporta il cursore all'inizio del blocco alla notifica GOING,
#    ma solo se caricato con bump_ptr= e restore_alloc=1: questo script si
#    ferma se non li trova, perche' il primo rmmod senza rewind lascia la
#    radio giu' fino al reboot.
#
# 2. hostapd (la variante Broadcom, `hostapd -bund`) tiene aperto
#    /dev/wl_event, un char device la cui file_operations sta DENTRO il blocco
#    di wl. Il rmmod passa lo stesso, e il ricarico riscrive quel blocco sotto
#    i piedi di hostapd: una read durante la rilocazione salta su una word non
#    rilocata (oops in vfs_read, epc = offset), una read dopo trova
#    private_data liberata e crasha al primo evento, cioe' al primo up. Va
#    fermato via /etc/init.d/hostapd (non kill: lo script di init aspetta il
#    pid e non c'e' respawn) PRIMA del primo rmmod, e rilanciato a fine corsa.
#    Con hostapd fermo cade anche il 2.4 GHz: si guida da seriale o ethernet.
#
# 3. /lib/wireless/init_broadcom.sh configura L'ISTANZA del driver -- `nar 0`,
#    `phycal_tempdelta 40`, soglie radar scritte con un up/down sul chanspec
#    di default -- e gira una volta sola per boot (/tmp/hostapd_init_once).
#    Le istanze catturate qui NON la ricevono, per scelta: sono wl con i
#    default Broadcom, confrontabili con gli altri board della collezione, e
#    quell'up/down sarebbe comunque il primo phy_init del boot, cioe' il
#    freddo bruciato prima della cattura (nel dmesg di boot: cacstate ch 44
#    a 66 s da init_broadcom.sh, poi ch 36 a 74 s da hostapd). A fine corsa
#    /tmp/hostapd_init_once viene rimosso, cosi' `hostapd start` rifa'
#    init_broadcom.sh completo sull'istanza che resta in esercizio.
#
# 4. A ogni rmmod il FAP stampa `dqmHandlerRegisterHost: Exceeded maximum
#    number of DQM IRQ Handlers! (8)`: qualcuno, all'unbind di wfd, registra
#    handler senza liberare i vecchi. E' un leak per ciclo con un tetto di 8,
#    e oltre quel tetto il percorso RX accelerato della nuova istanza e'
#    compromesso. Lo script conta il messaggio a ogni ciclo e lo stampa: se
#    cresce, il numero di cicli per boot ha un limite e va misurato, non
#    ignorato.
#
# Sul device, una volta sola (il cursore lo trova reverse-tools/memfind.c e lo
# conferma la lui/lw in module_alloc; 0x80575464 e' quello di questa unita' e
# di questo firmware, non e' portabile):
#     insmod /tmp/wl_diag.ko target=wl arm=1 skipphyrd="0x253,0x254" \
#            bump_ptr=0x80575464 restore_alloc=1
#     cat /proc/wl_diag | nc <HOST> 5555 &
# sull'host:
#     ncat -l 5555 | python3 decode-wl-diag.py | tee cold.txt
# a fine corsa:
#     python3 reverse-tools/split_trace.py --on mark cold.txt split/
#
# Uso, come capture_cold_init.sh:
#   sh capture_cold_tg789vac.sh 20 36
#   sh capture_cold_tg789vac.sh 20 36 40 44 48
#   sh capture_cold_tg789vac.sh 20dfs
#   SETTLE=12 SSID=test-ap5 sh capture_cold_tg789vac.sh 20
#
# Variabili d'ambiente:
#   IF           interfaccia                                  (default wl1)
#   SETTLE       attesa dopo up, secondi                       (default 10)
#   SETTLE_BSS   attesa dopo bss up, secondi                   (default 10)
#   SSID         se impostato: ssid + bss up prima dell'attesa (default vuoto)
#   WL_KO        percorso di wl.ko           (default /lib/modules/3.4.11/wl.ko,
#                lo stesso di /etc/modules.d/57-bcm63xx-tch-wireless)
#   DEVID        deviceid ammessi su IF                        (default 0x43a2)
#   HOSTAPD_INIT script di init di hostapd          (default /etc/init.d/hostapd)
#   NO_RESTART   1 = a fine corsa NON rilanciare hostapd        (default vuoto)
#   TEMPDELTA    se impostato: `wl phycal_tempdelta` forzato a questo valore
#                dopo l'insmod e prima dell'up                (default vuoto)
#
# I comandi dati al driver li registra wl_diag stesso, con l'hook su
# wlc_ioctl: ogni `wl <variabile> <valore>` compare nella traccia come
# IOVAR.SET, gli altri ioctl come IOCTL, quindi lo script non marca ne' il
# canale ne' phycal_tempdelta. split_trace.py --on mod ricava il nome del
# segmento dal `wl chanspec` registrato. Serve un wl_diag con quell'hook: se il
# dmesg non ne riporta il piano lo script lo dice prima di partire.
#
# Niente `set -u`, niente head/awk/sed/tr: il busybox di questi firmware non li
# ha tutti. Qui servono i builtin della shell piu' wl, insmod, rmmod, sleep,
# grep, ls, dmesg, ifconfig.

DIR="${0%/*}"
[ "$DIR" = "$0" ] && DIR=.
if [ ! -f "$DIR/capture_profiles.sh" ]; then
    echo "manca $DIR/capture_profiles.sh: va copiato sul device accanto a questo script" >&2
    exit 1
fi
. "$DIR/capture_profiles.sh"

[ -n "$1" ] || { echo "uso: sh capture_cold_tg789vac.sh {$PROFILI} | <20|40|80> <canale> [canale...]" >&2; exit 1; }

# I profili 20, 40 e 80 hanno lo stesso nome delle larghezze: e' un profilo
# solo se e' l'unico argomento, altrimenti il primo argomento e' la larghezza.
# Gli altri profili (20dfs, 20meteo...) non prendono mai canali, quindi la
# regola non ha ambiguita'.
if [ $# -eq 1 ] && profilo "$1"; then
    set -- $LIST
else
    BW="$1"
    shift
    [ -n "$1" ] || { echo "manca almeno un canale" >&2; exit 1; }
    case "$BW" in
        20|40|80) ;;
        *) echo "primo argomento: un profilo ($PROFILI) o una larghezza (20, 40, 80), non '$BW'" >&2; exit 1 ;;
    esac
fi

[ -n "$IF" ]           || IF=wl1
[ -n "$SETTLE" ]       || SETTLE=10
[ -n "$SETTLE_BSS" ]   || SETTLE_BSS=10
[ -n "$WL_KO" ]        || WL_KO=/lib/modules/3.4.11/wl.ko
[ -n "$DEVID" ]        || DEVID="0x43a2"
case "$TEMPDELTA" in
    ''|[0-9]|[0-9][0-9]|[0-9][0-9][0-9]) ;;
    *) echo "TEMPDELTA non numerico: '$TEMPDELTA'" >&2; exit 1 ;;
esac
[ -n "$HOSTAPD_INIT" ] || HOSTAPD_INIT=/etc/init.d/hostapd
INIT_ONCE=/tmp/hostapd_init_once

case "$SETTLE" in
    ''|*[!0-9]*) echo "attesa non numerica: '$SETTLE'" >&2; exit 1 ;;
esac
case "$SETTLE_BSS" in
    ''|*[!0-9]*) echo "attesa bss non numerica: '$SETTLE_BSS'" >&2; exit 1 ;;
esac

# ---- controlli preliminari: tutti PRIMA del primo rmmod ------------------

if [ ! -r "$WL_KO" ]; then
    echo "WL_KO='$WL_KO' non leggibile" >&2
    exit 1
fi

if ! grep -q '^wl_diag ' /proc/modules 2>/dev/null; then
    echo "wl_diag non e' caricato. Caricalo una volta sola, prima di questo script:" >&2
    echo "  insmod /tmp/wl_diag.ko target=wl arm=1 bump_ptr=0x80575464 restore_alloc=1" >&2
    exit 1
fi

# Senza rewind il primo rmmod e' irreversibile fino al reboot: il cursore
# della regione riservata non torna indietro e il secondo insmod fallisce.
P=/sys/module/wl_diag/parameters
if [ ! -r "$P/bump_ptr" ] || [ "`cat $P/bump_ptr`" = "0" ]; then
    echo "wl_diag e' caricato SENZA bump_ptr: il rewind del cursore non avviene e" >&2
    echo "il primo rmmod lascia la radio giu' fino al reboot. Ricaricalo con" >&2
    echo "bump_ptr=0x80575464 restore_alloc=1." >&2
    exit 1
fi
if [ "`cat $P/restore_alloc 2>/dev/null`" != "1" ]; then
    echo "wl_diag ha bump_ptr ma restore_alloc=0 (solo dry-run): il cursore non" >&2
    echo "viene riportato indietro. Ricaricalo con restore_alloc=1." >&2
    exit 1
fi

if [ ! -w /proc/wl_diag ]; then
    echo "/proc/wl_diag non scrivibile: wl_diag troppo vecchio per i MARK" >&2
    exit 1
fi

if [ ! -x "$HOSTAPD_INIT" ]; then
    echo "$HOSTAPD_INIT non trovato: senza fermare hostapd il ricarico crasha" >&2
    exit 1
fi

# ---- funzioni -------------------------------------------------------------

marca() {
    echo "$1" > /proc/wl_diag
    return 0
}

caricato() {
    grep -q "^$1 " /proc/modules 2>/dev/null
}

conta_dmesg() {
    dmesg 2>/dev/null | grep -c "$1"
}

# Stampa "pid comm" di ogni processo con /dev/wl_event aperto. Vuoto = nessuno.
fd_wl_event() {
    for p in /proc/[0-9]*; do
        if ls -l "$p/fd" 2>/dev/null | grep -q wl_event; then
            echo "${p#/proc/} `cat $p/comm 2>/dev/null`"
        fi
    done
}

ferma_hostapd() {
    if [ -z "`fd_wl_event`" ]; then
        echo "hostapd: nessun processo ha /dev/wl_event aperto"
        return 0
    fi
    echo "hostapd: stop via $HOSTAPD_INIT"
    "$HOSTAPD_INIT" stop
    sleep 1
    APERTI=`fd_wl_event`
    if [ -n "$APERTI" ]; then
        echo "" >&2
        echo "/dev/wl_event e' ANCORA aperto da:" >&2
        echo "$APERTI" >&2
        echo "Il ricarico di wl con quel device aperto crasha il kernel. Mi fermo." >&2
        return 1
    fi
    return 0
}

# Rilancia hostapd come al boot: senza /tmp/hostapd_init_once `start` rifa'
# init_broadcom.sh, che riapplica nar, tempdelta e soglie radar sull'istanza
# nuova e poi tira su le radio con la configurazione uci.
riavvia_hostapd() {
    [ "$NO_RESTART" = "1" ] && { echo "NO_RESTART=1: hostapd resta fermo"; return 0; }
    if ! caricato wl; then
        echo "wl non e' caricato: hostapd non viene rilanciato. Prima: insmod $WL_KO" >&2
        return 1
    fi
    rm -f "$INIT_ONCE"
    echo "hostapd: start via $HOSTAPD_INIT (rifa' init_broadcom.sh)"
    "$HOSTAPD_INIT" start
}

attendi_if() {
    n=0
    while [ "$n" -lt 20 ]; do
        if [ -d "/sys/class/net/$IF" ] && wl -i "$IF" isup > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
        n=$((n + 1))
    done
    echo "l'interfaccia '$IF' non e' comparsa entro 20 s" >&2
    return 1
}

verifica_identita() {
    for d in $DEVID; do
        if wl -i "$IF" revinfo 2>/dev/null | grep -qi "deviceid $d"; then
            return 0
        fi
    done
    echo "" >&2
    echo "'$IF' non e' fra i device ammessi ($DEVID). Cosa risponde:" >&2
    wl -i "$IF" revinfo 2>&1 | grep -i "deviceid\|chipnum" >&2
    return 1
}

verifica_freddo() {
    st=`wl -i "$IF" isup 2>/dev/null`
    if [ "$st" = "1" ]; then
        echo "" >&2
        echo "'$IF' e' GIA' SU dopo il ricarico: non sarebbe una cattura a freddo." >&2
        echo "Con hostapd fermo non dovrebbe succedere: controlla chi la tira su." >&2
        return 1
    fi
    if [ "$st" != "0" ]; then
        echo "ATTENZIONE: 'wl -i $IF isup' ha risposto '$st'. Si continua." >&2
    fi
    return 0
}

scarica_wl() {
    APERTI=`fd_wl_event`
    if [ -n "$APERTI" ]; then
        echo "/dev/wl_event riaperto da: $APERTI -- non scarico wl" >&2
        return 1
    fi

    wl -i "$IF" down > /dev/null 2>&1
    ifconfig "$IF" down > /dev/null 2>&1

    REW0=`conta_dmesg 'wl_diag: cursor rewound'`
    if ! rmmod wl 2>&1; then
        echo "rmmod wl fallito: qualcuno tiene il modulo" >&2
        return 1
    fi
    sleep 1
    if caricato wl; then
        echo "wl risulta ancora caricato dopo il rmmod" >&2
        return 1
    fi
    if [ "`conta_dmesg 'wl_diag: cursor rewound'`" = "$REW0" ]; then
        echo "" >&2
        echo "wl_diag NON ha riportato indietro il cursore: l'insmod fallirebbe con" >&2
        echo "'Load wl module core (null)'. Guarda perche':  dmesg | grep wl_diag" >&2
        echo "wl e' scaricato e non ricaricabile senza rewind." >&2
        return 1
    fi
    return 0
}

carica_wl() {
    NULL0=`conta_dmesg 'Load wl module core.*null'`
    REUSE0=`conta_dmesg 'wl_diag: block reused'`
    if ! insmod "$WL_KO" 2>&1; then
        echo "insmod '$WL_KO' fallito: ORA NON C'E' NESSUN wl CARICATO." >&2
        if [ "`conta_dmesg 'Load wl module core.*null'`" != "$NULL0" ]; then
            echo "Il kernel ha risposto (null): regione riservata esaurita." >&2
        fi
        return 1
    fi
    sleep 2

    if [ "`conta_dmesg 'wl_diag: block reused'`" = "$REUSE0" ]; then
        echo "ATTENZIONE: wl_diag non conferma il riuso del blocco (dmesg | grep wl_diag)" >&2
    fi

    attendi_if || return 1
    verifica_identita || return 1
    verifica_freddo || return 1
    return 0
}

ciclo() {
    ch="$1"
    cs="5g$ch/$BW"

    echo ""
    echo "=================== $cs ==================="

    FAP0=`conta_dmesg 'Exceeded maximum number of DQM IRQ Handlers'`

    scarica_wl || return 1
    carica_wl  || return 1

    if [ -n "$TEMPDELTA" ]; then
        wl -i "$IF" phycal_tempdelta "$TEMPDELTA"
    fi

    if ! wl -i "$IF" chanspec "$cs" > /dev/null 2>&1; then
        msg=`wl -i "$IF" chanspec "$cs" 2>&1`
        echo "chanspec $cs rifiutato: $msg" >&2
        return 0
    fi

    if [ -n "$SSID" ]; then
        wl -i "$IF" ssid "$SSID" > /dev/null 2>&1
    fi

    wl -i "$IF" up
    sleep "$SETTLE"

    if [ -n "$SSID" ]; then
        wl -i "$IF" bss up > /dev/null 2>&1
        sleep "$SETTLE_BSS"
    fi

    wl -i "$IF" down
    sleep 1

    FAP1=`conta_dmesg 'Exceeded maximum number of DQM IRQ Handlers'`
    if [ "$FAP1" != "$FAP0" ]; then
        echo "FAP: DQM handler leak, messaggi 'Exceeded maximum' $FAP0 -> $FAP1" >&2
    fi
    return 0
}

# ---- corsa ---------------------------------------------------------------

echo "cold tg789vac: $IF, BW$BW, attesa ${SETTLE}s + ${SETTLE_BSS}s bss, deviceid ammessi: $DEVID"
echo "modulo: $WL_KO, cursore: `cat $P/bump_ptr`; istanze catturate con i default Broadcom, senza init_broadcom.sh"
[ -n "$SSID" ] || echo "SSID non impostato: la bss non sale e mancheranno le tabelle per-core"
echo "hostapd viene fermato: cadono ENTRAMBE le radio. Non guidare questa procedura in wifi."
echo "il lettore di /proc/wl_diag deve essere gia' attivo"

if [ "`conta_dmesg "wl_diag: hook plan 'wlc_ioctl'"`" = "0" ]; then
    echo "ATTENZIONE: wl_diag non riporta il piano dell'hook su wlc_ioctl:" >&2
    echo "i comandi a wl non finiranno nella traccia e i segmenti non avranno nome." >&2
fi

ferma_hostapd || exit 1

for c in "$@"; do
    case "$c" in
        ''|*[!0-9]*) echo "canale non numerico: '$c', salto" >&2; continue ;;
    esac
    if ! ciclo "$c"; then
        echo "" >&2
        echo "ciclo su 5g$c/$BW interrotto. Stato lasciato com'e' per l'ispezione:" >&2
        echo "  grep -E '^(wl|wl_diag) ' /proc/modules ; dmesg | grep -E 'wl_diag|Load wl module'" >&2
        echo "hostapd e' FERMO. Per rimettere in esercizio:" >&2
        echo "  [insmod $WL_KO ;] rm -f $INIT_ONCE ; $HOSTAPD_INIT start" >&2
        exit 1
    fi
done

marca "fine corsa"
echo ""
echo "FAP: messaggi 'Exceeded maximum' in questo boot: `conta_dmesg 'Exceeded maximum number of DQM IRQ Handlers'`"
riavvia_hostapd
echo ""
echo "fatto. Taglia la traccia con:"
echo "  python3 split_trace.py --on mark <trace> split/ --bringup-only"
