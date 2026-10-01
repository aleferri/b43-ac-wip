#!/usr/bin/env bash
# Capture wl's complete register-access sequence for a full device init.
#
# Changing the binding of wl while probes attach crashed the machine twice
# (PCI unbind/rebind; reload of an unbound wl). This keeps wl loaded and
# bound, attaches the probes to the running driver (known to work), and
# triggers a full re-init by taking the interface down and up.
# Syncs to disk every second so partial data survives a crash.
#
#   sudo systemd-run --unit=wltrace --collect --no-block \
#     --setenv=PATH="$PATH" /run/current-system/sw/bin/bash trace_wl.sh
#
# If WiFi is not back afterwards, the script reboots.

PROJ=/home/gonsolo/bcm4360-acphy
DEV=0000:03:00.0
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$PROJ/traces"
TRACE="$PROJ/traces/wl-init-$STAMP.trace"
exec >"$PROJ/traces/wl-init-$STAMP.meta" 2>&1
set -x

(while sleep 1; do sync; done) &
SYNCER=$!

# Give the Claude session time to send its reply before WiFi drops.
sleep 5

sysctl -w kernel.panic_on_oops=1 kernel.panic=10 \
	kernel.hung_task_panic=1 kernel.hung_task_timeout_secs=30 \
	kernel.softlockup_panic=1
sync

BPFTRACE_PERF_RB_PAGES=16384 bpftrace "$PROJ/wl_full_trace.bt" >"$TRACE" 2>&1 &
BT=$!
for i in $(seq 1 30); do
	grep -q "wl trace start" "$TRACE" && break
	sleep 1
done
if ! grep -q "wl trace start" "$TRACE"; then
	echo "bpftrace did not start"
	kill $BT
fi

echo "wltrace: === IFDOWN ===" > /dev/kmsg
ip link set wlp3s0 down
sleep 2
echo "wltrace: === IFUP ===" > /dev/kmsg
ip link set wlp3s0 up

# Let NetworkManager reassociate so the trace covers the channel set too.
sleep 20
echo "wltrace: === STOP ===" > /dev/kmsg
kill -INT $BT
wait $BT
sync

readlink -f /sys/bus/pci/devices/$DEV/driver
nmcli -t -f DEVICE,STATE device
dmesg | sed -n '/wltrace: === IFDOWN ===/,$p'

kill $SYNCER
if readlink /sys/bus/pci/devices/$DEV/driver | grep -q '/wl$' &&
   nmcli -t -f STATE general | grep -q '^connected'; then
	echo "===== WiFi is back ====="
	sysctl -w kernel.panic_on_oops=0 kernel.panic=0 \
		kernel.hung_task_panic=0 kernel.hung_task_timeout_secs=120 \
		kernel.softlockup_panic=0
	sync
else
	echo "===== WiFi NOT back, rebooting ====="
	journalctl --sync
	sync
	systemctl reboot
fi
