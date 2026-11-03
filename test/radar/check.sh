#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# check.sh: radar_ac.c against
#  - the RT-AC66U B1 runs with known pulses (radar-fifo.zip): every ETSI 1-4
#    and FCC 0-4 burst run is a radar, the idle AP's beacons and a train of
#    noise bursts with a radar's timing (noise10) are not;
#  - the agcombo's bss-up (vd625-agcombo/bss-up.zip), 175 s at ch100/80 that
#    the stock driver kept the channel through: not a radar;
#  - synthetic bursts of every type, 100 each with 30 % of the pulses missed
#    in each FIFO, all radars, and 60 s of random pulses at 20-2000 a second,
#    none (synth.py).
set -eu
cd "$(dirname "$0")"
REPO=../..
W=build
rm -rf "$W"
mkdir -p "$W/fifo" "$W/agc"
unzip -q -d "$W/fifo" "$REPO/router-data/rt-ac66u-b1/radar-fifo.zip" \
	'runs-wl/std-*.txt' 'runs-b43/idle5s.txt' 'runs-b43/noise10.txt'
unzip -q -d "$W/agc" "$REPO/router-data/vd625-agcombo/bss-up.zip"
python3 -I agcombo.py "$W/agc/bss-up-ch100-bw80.txt" > "$W/agcombo.txt"
MISS=0.3 SEEDS=100 python3 -I synth.py "$W/syn"

fail=0
expect() {	# expect radar|none FILE...
	want=$1; shift
	./replay "$@" | grep -v '^  poll' | while read -r f got _; do
		case $want:$got in
		radar:none | none:[!n]*) echo "FAIL $f $got" >&2; echo x ;;
		esac
	done | grep -c '^x' || true
}
n=$(expect radar "$W"/fifo/runs-wl/std-*.txt); fail=$((fail + n))
n=$(expect none "$W/fifo/runs-b43/idle5s.txt" "$W/fifo/runs-b43/noise10.txt" \
	"$W/agcombo.txt"); fail=$((fail + n))
for t in fcc0 fcc1 fcc2 fcc3 fcc4 etsi1 etsi2 etsi3 etsi4; do
	n=$(expect radar "$W"/syn/$t-*.txt); fail=$((fail + n))
done
n=$(expect none "$W"/syn/noise-*.txt); fail=$((fail + n))

if [ "$fail" -ne 0 ]; then
	echo "$fail failed"
	exit 1
fi
echo "all passed"
