#!/bin/sh
# wl shim for the capture scripts on DD-WRT: the commands DD-WRT's cut-down wl lacks go to
# /tmp/wlx (direct ioctls), everything else to the real wl. Install: mkdir -p /tmp/wlbin; cp wl-shim.sh
# /tmp/wlbin/wl; chmod +x /tmp/wlbin/wl; export PATH=/tmp/wlbin:$PATH   (REAL_WL = the original wl)
REAL_WL=${REAL_WL:-/usr/sbin/wl}
WLX=${WLX:-/tmp/wlx}
IF=
if [ "$1" = "-i" ]; then IF=$2; shift 2; fi
case "$1" in
up|down|isup|revinfo|ap|bss)
	[ -n "$IF" ] || IF=eth1
	exec "$WLX" "$IF" "$@" ;;
ssid)
	# After `insmod wl` the driver is a STA (DD-WRT sets AP mode in its own wlconf), so the scripts' `ssid` + `bss up`
	# never brought a BSS up (no tables 0x0e/0x42/0x62/0x82). AP mode first; the scripts call ssid with the radio down.
	[ -n "$IF" ] || IF=eth1
	[ $# -gt 1 ] && "$WLX" "$IF" ap 1 2>/dev/null
	exec "$REAL_WL" -i "$IF" "$@" ;;
txbf|vhtmode)
	[ -n "$IF" ] || IF=eth1
	exec "$WLX" "$IF" iovar "$@" ;;
*)
	if [ -n "$IF" ]; then exec "$REAL_WL" -i "$IF" "$@"; else exec "$REAL_WL" "$@"; fi ;;
esac
