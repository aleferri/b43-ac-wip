#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# check.sh: radar_ac.c against
#  - the RT-AC66U B1 runs with known pulses (radar-fifo.zip): every ETSI 1-4
#    and FCC 0-4 burst run is a radar, the idle AP's beacons and a train of
#    noise bursts with a radar's timing (noise10) are not;
#  - the agcombo's bss-up (vd625-agcombo/bss-up.zip), 175 s at ch100/80 that
#    the stock driver kept the channel through: not a radar;
#  - the runs at 20 and 40 MHz of radar-widths.zip: ETSI 1 and trains of 1
#    and 5 us at 1000 us a radar, of 20 us not, the noise bursts not at 40;
#  - synthetic bursts of every type, ETSI 5 and 6 (staggered) included, 100
#    each with 30 % of the pulses missed in each FIFO, all radars with every
#    region's types matched and with their own region's (replay -r), and 60 s
#    of random pulses at 20-2000 a second, none in any region (synth.py).
set -eu
cd "$(dirname "$0")"
REPO=../..
W=build
rm -rf "$W"
mkdir -p "$W/fifo" "$W/agc"
unzip -q -d "$W/fifo" "$REPO/router-data/rt-ac66u-b1/radar-fifo.zip" \
	'runs-wl/std-*.txt' 'runs-b43/idle5s.txt' 'runs-b43/noise10.txt'
unzip -q -d "$W/agc" "$REPO/router-data/vd625-agcombo/bss-up.zip"
unzip -q -d "$W/widths" "$REPO/router-data/rt-ac66u-b1/radar-widths.zip" 'fifo-*' 
python3 -I agcombo.py "$W/agc/bss-up-ch100-bw80.txt" > "$W/agcombo.txt"
MISS=0.3 SEEDS=100 python3 -I synth.py "$W/syn"

fail=0
expect() {	# expect radar|none [-w MHz] [-r region] FILE...
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
# Every type with all regions' types matched, and with only its region's.
for t in fcc0 fcc1 fcc2 fcc3 fcc4 fcc5 fcc6 etsi1 etsi2 etsi3 etsi4 etsi5 etsi6; do
	n=$(expect radar "$W"/syn/$t-*.txt); fail=$((fail + n))
	n=$(expect radar -r "${t%%[0-9]*}" "$W"/syn/$t-*.txt); fail=$((fail + n))
done
for r in all fcc etsi; do
	n=$(expect none -r $r "$W"/syn/noise-*.txt "$W"/syn/long-*.txt); fail=$((fail + n))
done
# 20 and 40 MHz (radar-widths.zip).
for w in 20 40; do
	F=$W/widths/fifo-$w/w$w
	n=$(expect radar -w $w "$F"-etsi1.txt "$F"-pw1-pri1000.txt \
		"$F"-pw5-pri1000.txt); fail=$((fail + n))
	n=$(expect none -w $w "$F"-pw20-pri1000.txt); fail=$((fail + n))
done
for w in 20 40; do
	n=$(expect none -w $w "$W"/widths/fifo-$w/w$w-noise10.txt); fail=$((fail + n))
done
# 20 MHz in service (radar-20mhz.zip): the noise bursts and the HackRF
# sending nothing are not a radar in any region, every test signal is one
# with all types (etsi3-flooded aside: a carrier filled the FIFOs over it).
mkdir -p "$W/w20"
unzip -q -d "$W/w20" "$REPO/router-data/rt-ac66u-b1/radar-20mhz.zip" 'fifo/*.txt'
for f in "$W"/w20/fifo/*.txt; do
	sed -n -e 's/^b43-radar poll .*/poll/p' -e 's/^b43-radar \([01] .*\)/radar \1/p' \
		"$f" > "${f%.txt}.rp"
done
for r in all fcc etsi; do
	n=$(expect none -w 20 -r $r "$W"/w20/fifo/noise*.rp "$W"/w20/fifo/silent*.rp)
	fail=$((fail + n))
done
n=$(expect radar -w 20 $(ls "$W"/w20/fifo/*.rp | grep -v -e /noise -e /silent -e /etsi3-flooded -e /summary)); fail=$((fail + n))
# FCC 5 in service at 52/80 (radar-fcc5.zip): a radar with the FCC types,
# not with the ETSI types; 180 s of nothing sent and single bursts of long
# pulses are not a radar.
mkdir -p "$W/fcc5"
unzip -q -d "$W/fcc5" "$REPO/router-data/rt-ac66u-b1/radar-fcc5.zip" 'fifo/*.txt'
for f in "$W"/fcc5/fifo/*.txt; do
	sed -n -e 's/^b43-radar poll .*/poll/p' -e 's/^b43-radar \([01] .*\)/radar \1/p' \
		"$f" > "${f%.txt}.rp"
done
for r in all fcc; do
	n=$(expect radar -r $r "$W"/fcc5/fifo/fcc5-*.rp); fail=$((fail + n))
done
n=$(expect none -r etsi "$W"/fcc5/fifo/fcc5-*.rp); fail=$((fail + n))
n=$(expect none "$W"/fcc5/fifo/idle-*.rp "$W"/fcc5/fifo/lp*.rp); fail=$((fail + n))

if [ "$fail" -ne 0 ]; then
	echo "$fail failed"
	exit 1
fi
echo "all passed"
