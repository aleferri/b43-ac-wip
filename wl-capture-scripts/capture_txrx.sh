#!/bin/sh
# One run for the frames: for each chanspec, the template RAM writes of the
# bring-up (beacon and probe response templates, with the bytes in front of
# the frame) and then the TX and RX frames of a station passing traffic, all
# in the same /proc/wl_diag stream, cut by MARK records:
#
#   tpl <chanspec>     from here, template RAM writes with their content
#   txrx <chanspec>    from here, TX.PKT and RX.PKT with their headers
#   end
#
# so `reverse-tools/split_trace.py --on mark` splits it afterwards.
#
# Assumes:
#   - wl_diag.ko armed (arm=1), with wlc_txfifo, wlc_recv and
#     wlc_bmac_write_template_ram in its plan, and the stream drained to the
#     host (cat /proc/wl_diag | nc <host> 5555) for the whole run;
#   - a station that rejoins the BSS by itself after each change, and
#     traffic in both directions (iperf3 -c ... and -R) while the script
#     waits in each `txrx` window.
#
# Two ways to change channel:
#
#   default    the script does `wl down; wl chanspec; wl up` (and `ssid`,
#              `bss up` when given an SSID). That BSS has whatever security
#              wl was left with, and whether the vendor's authenticator
#              still runs the WPA handshake after a down/up done behind its
#              back depends on the firmware.
#   MANUAL=1   the script touches nothing: for each chanspec it arms the
#              template capture and waits for Enter while you apply that
#              channel and width from the vendor's UI or CLI, which restarts
#              the network the way the firmware expects, keys included.
#              The chanspecs are then only the labels of the MARK records.
#
# Usage:
#   sh capture_txrx.sh                                 wl1, defaults below
#   sh capture_txrx.sh wl1 test-ap                     with the SSID to bring up
#   sh capture_txrx.sh wl1 test-ap "5g36/20 5g44/80"   chanspecs of your own
#
# The default chanspecs have the primary off the bottom of its block at 40
# and 80 MHz (40 in 36-40, 44 in 36-48), so the subband shows up in the TX
# headers; one the driver refuses is skipped with its message.
#
# Environment, all optional:
#   SETTLE      seconds after `up`, for the calibrations         (default 10)
#   ASSOC_WAIT  seconds to wait for a station to rejoin          (default 60)
#   TRAFFIC     seconds of the txrx window                       (default 30)
#   TPL_BUDGET  template RAM writes per chanspec                 (default 512)
#   PKT_BUDGET  TX frames, and RX frames, per chanspec           (default 512)
#   MANUAL      1 = channel changes done by hand, see above
#
# Only shell builtins plus `wl` and `sleep`: these busybox builds lack head,
# awk and others. No `set -u` either, which they do not handle.

IF="$1"
SSID="$2"
LIST="$3"
[ -n "$IF" ] || IF=wl1
[ -n "$LIST" ] || LIST="5g36/20 5g40/40 5g44/80"
[ -n "$SETTLE" ] || SETTLE=10
[ -n "$ASSOC_WAIT" ] || ASSOC_WAIT=60
[ -n "$TRAFFIC" ] || TRAFFIC=30
[ -n "$TPL_BUDGET" ] || TPL_BUDGET=512
[ -n "$PKT_BUDGET" ] || PKT_BUDGET=512

P=/sys/module/wl_diag/parameters
for f in txdump rxdump tpldump; do
    if [ ! -w "$P/$f" ]; then
        echo "$P/$f is not there: wl_diag.ko without the frame capture?" >&2
        exit 1
    fi
done
if ! wl -i "$IF" status >/dev/null 2>&1; then
    echo "interface '$IF' does not answer 'wl -i $IF status'" >&2
    exit 1
fi

mark() {
    echo "$1" > /proc/wl_diag
}

# Everything off, whatever a previous run left.
dumps_off() {
    echo 0 > "$P/txdump"
    echo 0 > "$P/rxdump"
    echo 0 > "$P/tpldump"
}

# A station in the assoclist, or ASSOC_WAIT seconds gone.
wait_assoc() {
    n=0
    while [ "$n" -lt "$ASSOC_WAIT" ]; do
        out=`wl -i "$IF" assoclist 2>/dev/null`
        [ -n "$out" ] && return 0
        sleep 1
        n=$((n + 1))
    done
    return 1
}

# Bring the BSS up on chanspec $1 with the template capture armed; 1 if the
# driver refuses the chanspec.
bring_up() {
    if [ "$MANUAL" = 1 ]; then
        mark "tpl $1"
        echo "$TPL_BUDGET" > "$P/tplbudget"
        echo 512 > "$P/tpldump"
        echo ">>> apply $1 from the UI/CLI now; Enter when it is up"
        read dummy
        return 0
    fi

    wl -i "$IF" down
    sleep 1
    if ! wl -i "$IF" chanspec "$1" > /dev/null 2>&1; then
        msg=`wl -i "$IF" chanspec "$1" 2>&1`
        echo "skipping $1: $msg"
        return 1
    fi
    mark "tpl $1"
    echo "$TPL_BUDGET" > "$P/tplbudget"
    echo 512 > "$P/tpldump"
    [ -n "$SSID" ] && wl -i "$IF" ssid "$SSID" > /dev/null 2>&1
    wl -i "$IF" up
    sleep "$SETTLE"
    [ -n "$SSID" ] && wl -i "$IF" bss up > /dev/null 2>&1
    return 0
}

dumps_off
for cs in $LIST; do
    bring_up "$cs" || continue

    echo "--- $cs: waiting for a station"
    if ! wait_assoc; then
        echo "no station on $cs after $ASSOC_WAIT s, skipping its traffic"
        echo 0 > "$P/tpldump"
        continue
    fi
    echo 0 > "$P/tpldump"

    mark "txrx $cs"
    echo "$PKT_BUDGET" > "$P/txbudget"
    echo "$PKT_BUDGET" > "$P/rxbudget"
    echo 168 > "$P/txdump"
    echo 96 > "$P/rxdump"
    echo ">>> $cs: traffic both ways now, $TRAFFIC s"
    sleep "$TRAFFIC"
    echo 0 > "$P/txdump"
    echo 0 > "$P/rxdump"
done

mark "end"
dumps_off
echo "done: split the trace on its MARK records."
