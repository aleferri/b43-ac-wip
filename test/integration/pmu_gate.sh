#!/bin/sh
# bcma's BCM4352/BCM4360 PMU and PCIe2 init against the stock driver's attach.
#
# Both captures are chip rev 3, one per value of chipstatus bit 5:
#
#   macbook  4360, 0x024d, bit 5 clear
#   archer   4352, 0x1a6d, bit 5 set
#
# The archer's wl 6.30.223 does not write the fractional PLL on the 4352,
# the 7.14 does (d6220 cold01 #102-#114): there the comparison starts at the
# resource table. Chip revs 2 and 4 must write nothing; a USB-strapped 4360
# (bit 8) only skips the PLL, and runs without an oracle, which would answer
# the chipstatus read.
#
# Prerequisites: make b43-trace. Usage: ./pmu_gate.sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$HERE/../..
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0

xz -dc "$REPO/router-data/macbookair6-1/wl-firstload-20260926-190438.trace.xz" \
	> "$T/macbook.raw"
unzip -p "$REPO/router-data/archer-t5e/mmiotrace.zip" > "$T/archer.raw"
for b in macbook archer; do
	python3 "$REPO/reverse-tools/mmio2ops.py" "$T/$b.raw" --keep-flush \
		-o "$T/$b.ops" > /dev/null 2>&1
done

port() {	# board oracle rev chipstatus out
	B43_BOARD=$1 B43_READ_ORACLE=$2 B43_CHIP_REV=$3 B43_CC_CHIPSTATUS=$4 \
		B43_TRACE_OUT=$5 "$HERE/b43-trace" > /dev/null 2>&1 || true
}

check() {	# name expected actual
	if diff -u "$2" "$3" > "$T/diff"; then
		echo "PASS $1"
	else
		echo "FAIL $1"
		cat "$T/diff"
		fail=1
	fi
}

for run in macbook: archer:--from=RES_TABSEL; do
	b=${run%%:*}
	from=${run#*:}
	port "$b" "$T/$b.ops" 3 "" "$T/$b.port"
	python3 "$HERE/pmu_ops.py" --stop gpiotimer $from "$T/$b.ops" \
		> "$T/$b.want"
	python3 "$HERE/pmu_ops.py" --stop bcma $from "$T/$b.port" > "$T/$b.got"
	check "$b rev 3" "$T/$b.want" "$T/$b.got"

	for rev in 2 4; do
		port "$b" "$T/$b.ops" $rev "" "$T/$b.r$rev"
		python3 "$HERE/pmu_ops.py" --stop bcma "$T/$b.r$rev" \
			> "$T/$b.got$rev"
		check "$b rev $rev" /dev/null "$T/$b.got$rev"
	done
done

port macbook "" 3 0x34d "$T/usb.port"
python3 "$HERE/pmu_ops.py" --stop gpiotimer --from RES_TABSEL "$T/macbook.ops" \
	> "$T/usb.want"
python3 "$HERE/pmu_ops.py" --stop bcma "$T/usb.port" > "$T/usb.got"
check "macbook rev 3, USB strap" "$T/usb.want" "$T/usb.got"
exit $fail
