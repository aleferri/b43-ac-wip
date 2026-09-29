#!/bin/sh
# The port's 2.4 GHz delta against the stock driver's.
#
# Two bus captures of the hybrid wl hold scan hops on both bands, the
# MacBookAir6,1 and the archer-t5e. reverse-tools/band_delta.py takes ch40,
# ch44, ch2 and ch3 of each and keeps what moves with the band alone; the
# harness runs switch_channel on the same four channels, and the last line of
# the report says how many of the deltas both boards share the port
# reproduces.
#
# The captures are decoded, split and folded once into BAND_TMP (default
# /tmp/band), which later runs reuse.
#
# BOARD picks the harness profile (default d6220); archer carries the only
# dual-band SROM, so the 2.4 GHz values that come from the SROM only match
# there.
#
# Usage: [BOARD=archer] ./band_gate.sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$HERE/../..
TOOLS=$REPO/reverse-tools
T=${BAND_TMP:-/tmp/band}
CHS="40 44 2 3"
BOARD=${BOARD:-d6220}

hop() {		# first segment of a split dir on channel $2 at 20 MHz
	ls "$1"/*-up-ch"$2"-bw20.txt | head -1
}

prepare() {	# $1 name, $2 decoded ops
	[ -d "$T/$1/split" ] && return
	python3 "$TOOLS/split_trace.py" --on chanspec "$2" "$T/$1/split" >/dev/null
	for c in $CHS; do
		python3 "$TOOLS/ops_fold.py" fold "$(hop "$T/$1/split" "$c")" \
			-o "$T/$1/ch$c.fold" >/dev/null
	done
}

mkdir -p "$T/mb" "$T/at"
if [ ! -f "$T/mb/ops" ]; then
	xz -dc "$REPO/router-data/macbookair6-1/wl-init-20260926-132021.trace.xz" \
		> "$T/mb/trace"
	python3 "$TOOLS/mmio2ops.py" "$T/mb/trace" --format bpftrace -o "$T/mb/ops"
fi
if [ ! -f "$T/at/ops" ]; then
	unzip -p "$REPO/router-data/archer-t5e/mmiotrace.zip" > "$T/at/trace"
	python3 "$TOOLS/mmio2ops.py" "$T/at/trace" -o "$T/at/ops"
fi
prepare mb "$T/mb/ops"
prepare at "$T/at/ops"

for c in $CHS; do
	AC_CHANNEL=$c AC_BW=20 AC_FIRST_INIT=0 "$HERE/ac_trace" switch_channel "$BOARD" \
		> "$T/port-ch$c" 2>/dev/null
done

python3 "$TOOLS/band_delta.py" \
	"$T/mb/ch40.fold" "$T/mb/ch44.fold" "$T/mb/ch2.fold" "$T/mb/ch3.fold" \
	--also "$T/at/ch40.fold" "$T/at/ch44.fold" "$T/at/ch2.fold" "$T/at/ch3.fold" \
	--port "$T/port-ch40" "$T/port-ch44" "$T/port-ch2" "$T/port-ch3"
