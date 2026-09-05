#!/bin/sh
# Cattura di init A FREDDO, un ciclo per canale. Kernel 3.4 e 2.6.30.
#
# Presupposto: wl_diag caricato UNA volta e lasciato li'. Si arma da se' alla
# notifica MODULE_STATE_COMING di `wl` e disarma al GOING, quindi il probe PCI e
# l'attach che ne segue cadono sotto gli hook senza remove/rescan del device. Il
# lettore resta aperto per tutta la corsa, quindi non ci sono pause: lo script
# gira anche sotto nohup.
#
# L'ordine delle notifiche lo permette su entrambi i kernel, verificato su
# kernel/module.c: load_module() finisce del tutto -- rilocazioni applicate,
# add_kallsyms fatta, modulo in lista -- poi arriva COMING, poi mod->init;
# in scaricamento gira mod->exit(), poi GOING, poi free_module().
#
# Sul device, una volta sola:
#   sul 3.4:
#     insmod /tmp/wl_diag.ko arm=1
#   sul 2.6.30, dove kallsyms_lookup_name esiste ma non e' esportata ai moduli
#   (l'export arriva in 2.6.33), le si passa l'indirizzo:
#     A=$(awk '$3=="kallsyms_lookup_name"{print $1}' /proc/kallsyms)
#     insmod /tmp/wl_diag.ko arm=1 klookup=0x$A
#   reverse-tools/gen_syms.py costruisce quella riga da un dump di
#   /proc/kallsyms, e dice in anticipo quali hook si risolveranno.
#   poi, su entrambi:
#     cat /proc/wl_diag | nc <HOST> 5555 &
# sull'host:
#     ncat -l 5555 | python3 decode-wl-diag.py | tee cold.txt
# a fine corsa:
#     python3 reverse-tools/split_trace.py --on mark cold.txt split/
#
# I confini fra i cicli sono record MARK dentro la traccia, scritti da qui con
# `echo "ch36 bw20" > /proc/wl_diag`; wl_diag aggiunge "mod COMING"/"mod GOING"
# ai bordi di ogni caricamento. Nessuna euristica sui salti temporali, e nessun
# file da rinominare a mano.
#
# Perche' ricaricare `wl` e non toccare la struct del PHY: azzerare il byte
# "gia' calibrato" darebbe una cal completa dentro un phy_init A CALDO, con i
# flag adiacenti (250 "phy_init fatto", 249 POR) intatti, e quindi senza le
# costanti di fase dell'attach: coppia di gain ADC in init_regs, campo a 6 bit
# di 0x02e4, preambolo analogico.
#
# Ciclo, per ogni canale:
#     MARK ; rmmod wl ; insmod wl ; chanspec ; [ssid] ; up ; attesa ;
#     [bss up ; attesa] ; down
#
# L'attach cade sul chanspec di default e solo il primo `up` sul canale
# chiesto: e' il phy_init per-canale che si vuole, il preambolo di probe non
# dipende dal canale.
#
# Su 2.6.30 il rescan PCI scarica `wl` per mano dello spazio utente del vendor:
# wl_diag non tiene un riferimento sul bersaglio proprio per non farlo fallire.
#
# ATTENZIONE: `wl` serve entrambi i core, quindi il rmmod porta giu' anche il
# 2.4 GHz. Questa procedura si guida da seriale o da ethernet, non in wifi.
#
# Uso:
#   sh cold_capture.sh 20 36                     un canale
#   sh cold_capture.sh 20 36 40 44 48            piu' canali, un ciclo ognuno
#   IF=wl1 SETTLE=10 SSID=test-ap sh cold_capture.sh 40 36 44
#
# Variabili d'ambiente:
#   IF         interfaccia                                   (default wl1)
#   SETTLE     attesa dopo up e dopo bss up, secondi          (default 10)
#   SSID       se impostato: ssid + bss up prima dell'attesa  (default vuoto)
#   WL_KO      percorso di wl.ko: OBBLIGATORIO, vedi sotto
#   DEVID      deviceid atteso su IF, da `wl revinfo`         (default 0x43b3)
#   BR         bridge da cui staccare l'interfaccia           (default vuoto)
#   KILL       demoni da terminare prima del ricarico         (default vuoto)
#
# Niente `set -u`, niente head/awk/sed/tr: il busybox di questi firmware non li
# ha tutti e uno script che li usa muore a meta' senza dirlo. Qui servono solo
# builtin della shell piu' wl, insmod, rmmod, sleep, grep.

BW="$1"
[ -n "$BW" ] || { echo "uso: sh cold_capture.sh <20|40|80> <canale> [canale...]" >&2; exit 1; }
shift
[ -n "$1" ] || { echo "manca almeno un canale" >&2; exit 1; }

[ -n "$IF" ]     || IF=wl1
[ -n "$SETTLE" ] || SETTLE=10
[ -n "$DEVID" ]  || DEVID=0x43b3

case "$BW" in
    20|40|80) ;;
    *) echo "larghezza non valida: '$BW' (20, 40 o 80)" >&2; exit 1 ;;
esac

case "$SETTLE" in
    ''|*[!0-9]*) echo "attesa non numerica: '$SETTLE'" >&2; exit 1 ;;
esac

# Il percorso di wl.ko va saputo PRIMA del primo rmmod, non al momento
# dell'insmod: `insmod wl` senza percorso funziona solo sui busybox con la
# variante modprobe-small, e su questi firmware non e' garantito. Se il
# problema salta fuori al primo ciclo, `wl` e' gia' stato scaricato e la radio
# resta giu' fino a un reboot.
if [ -z "$WL_KO" ]; then
    echo "serve WL_KO=<percorso di wl.ko>. Cercalo in" >&2
    echo "/lib/modules/`uname -r 2>/dev/null`/ o nella riga di insmod dello" >&2
    echo "script di avvio del vendor." >&2
    exit 1
fi

if [ ! -r "$WL_KO" ]; then
    echo "WL_KO='$WL_KO' non leggibile" >&2
    exit 1
fi

if ! grep -q '^wl_diag ' /proc/modules 2>/dev/null; then
    echo "wl_diag non e' caricato. Caricalo una volta sola, prima di questo" >&2
    echo "script:  insmod /tmp/wl_diag.ko arm=1" >&2
    exit 1
fi

# La write su /proc/wl_diag inietta un MARK: se non e' scrivibile i confini non
# finiscono nella traccia, e tanto vale fermarsi subito.
if [ ! -w /proc/wl_diag ]; then
    echo "/proc/wl_diag non scrivibile: wl_diag e' troppo vecchio per i MARK" >&2
    echo "(serve la versione con la .write e il proc a 0600)." >&2
    exit 1
fi

marca() {
    echo "$1" > /proc/wl_diag
    return 0
}

caricato() {
    grep -q "^$1 " /proc/modules 2>/dev/null
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

# La numerazione delle istanze la assegna l'ordine di probe. Si controlla il
# deviceid invece di fidarsi del nome, perche' catturare l'altra radio produce
# una traccia plausibile e sbagliata.
verifica_identita() {
    if ! wl -i "$IF" revinfo 2>/dev/null | grep -qi "deviceid $DEVID"; then
        echo "" >&2
        echo "'$IF' non e' il device atteso ($DEVID). Cosa risponde:" >&2
        wl -i "$IF" revinfo 2>&1 | grep -i "deviceid\|chipnum" >&2
        echo "Passa IF=<nome giusto>, o DEVID=<atteso> se e' il default a" >&2
        echo "essere sbagliato per questa board." >&2
        return 1
    fi
    return 0
}

# Il modulo appena caricato deve trovarsi con l'interfaccia GIU'. Se qualcuno
# l'ha portata su -- hotplug del vendor, wlconf, nas -- il primo phy_init e'
# gia' avvenuto e la traccia successiva sarebbe un down->up con l'etichetta
# sbagliata. Meglio fermarsi che produrre una cattura che mente.
verifica_freddo() {
    st=`wl -i "$IF" isup 2>/dev/null`
    if [ "$st" = "1" ]; then
        echo "" >&2
        echo "'$IF' e' GIA' SU dopo il ricarico di wl: il primo phy_init e la" >&2
        echo "prima cal sono andati, questa non sarebbe una cattura a freddo." >&2
        echo "Metti in KILL i demoni che la tirano su (nas, eapd, acsd," >&2
        echo "wps_monitor, wlconf) e riprova." >&2
        return 1
    fi
    if [ "$st" != "0" ]; then
        echo "ATTENZIONE: 'wl -i $IF isup' ha risposto '$st', stato non" >&2
        echo "verificabile. Si continua, ma controlla la traccia." >&2
    fi
    return 0
}

scarica_wl() {
    if [ -n "$KILL" ]; then
        for d in $KILL; do
            killall "$d" > /dev/null 2>&1
        done
        sleep 1
    fi

    wl -i "$IF" down > /dev/null 2>&1
    ifconfig "$IF" down > /dev/null 2>&1
    [ -n "$BR" ] && brctl delif "$BR" "$IF" > /dev/null 2>&1

    # Nessun -EBUSY da wl_diag: il tracer non tiene un riferimento sul
    # bersaglio, si ripristina da se' sulla notifica GOING.
    if ! rmmod wl 2>&1; then
        echo "rmmod wl fallito: qualcuno tiene il modulo (netdev in bridge," >&2
        echo "demone aperto sull'interfaccia)." >&2
        return 1
    fi
    sleep 1

    if caricato wl; then
        echo "wl risulta ancora caricato dopo il rmmod" >&2
        return 1
    fi
    return 0
}

carica_wl() {
    if ! insmod "$WL_KO" 2>&1; then
        echo "insmod '$WL_KO' fallito: ORA NON C'E' NESSUN wl CARICATO." >&2
        echo "Ricaricalo a mano prima di riprovare:  insmod $WL_KO" >&2
        return 1
    fi
    sleep 2

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

    # Il marcatore va PRIMA del rmmod, cosi' il detach di wl, l'attach del
    # ricarico e il primo up finiscono tutti nel segmento del canale.
    marca "ch$ch bw$BW"

    scarica_wl || return 1
    carica_wl  || return 1

    if ! wl -i "$IF" chanspec "$cs" > /dev/null 2>&1; then
        msg=`wl -i "$IF" chanspec "$cs" 2>&1`
        echo "chanspec $cs rifiutato: $msg" >&2
        # I blocchi che includono 120/124/128 sono TDWR e vengono rifiutati per
        # regolamento: 6 combinazioni su 32. Non si abortisce la corsa.
        return 0
    fi

    # Senza associazione mancano quattro tabelle intere (0x0e, 0x42, 0x62,
    # 0x82) e 0x40/0x60 restano a un core: 1638 parole contro 3740.
    #
    # DA RISOLVERE SUL DEVICE, la documentazione del tracer si contraddice:
    # la sezione "Impostare il BSS" dice che basta `wl ssid` PRIMA di `up` e
    # riporta 3740 parole, la sezione 6 del workflow dice che `up` da solo fa
    # solo attach e che la bss la porta su `wl bss up` dopo il set-ssid. Qui si
    # fanno entrambe le cose nell'ordine ssid -> up -> bss up e si conta le
    # parole di tabella nel segmento: se sono 3740 la sequenza e' buona, se
    # sono 1638 la bss non e' salita e il ramo va corretto, non ripetuto.
    if [ -n "$SSID" ]; then
        wl -i "$IF" ssid "$SSID" > /dev/null 2>&1
    fi

    wl -i "$IF" up
    sleep "$SETTLE"

    if [ -n "$SSID" ]; then
        wl -i "$IF" bss up > /dev/null 2>&1
        sleep "$SETTLE"
    fi

    wl -i "$IF" down
    sleep 1
    return 0
}

echo "cold_capture: $IF, BW$BW, attesa ${SETTLE}s, deviceid atteso $DEVID"
echo "modulo: $WL_KO"
[ -n "$SSID" ] || echo "SSID non impostato: la bss non sale e mancheranno le tabelle per-core"
echo "il rmmod di wl porta giu' anche il 2.4 GHz: non guidare questa procedura in wifi"
echo "il lettore di /proc/wl_diag deve essere gia' attivo"

for c in "$@"; do
    case "$c" in
        ''|*[!0-9]*) echo "canale non numerico: '$c', salto" >&2; continue ;;
    esac
    if ! ciclo "$c"; then
        echo "" >&2
        echo "ciclo su 5g$c/$BW interrotto. Stato lasciato com'e' per l'ispezione:" >&2
        echo "  grep -E '^(wl|wl_diag) ' /proc/modules ; dmesg | grep wl_diag" >&2
        exit 1
    fi
done

marca "fine corsa"
echo ""
echo "fatto. Taglia la traccia con:"
echo "  python3 split_trace.py --on mark <trace> split/ --bringup-only"
