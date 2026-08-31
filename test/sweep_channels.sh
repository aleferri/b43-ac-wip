#!/bin/sh
# Run the port against every warm capture in a sweep, one channel at a time.
#
# Each segment of the sweep is one warm cycle on a single channel, which is
# what op_switch_channel does, so the flow is switch_channel with
# AC_FIRST_INIT=0 and the segment itself as the read oracle.
#
# The binary must be built with the channel guard overridden:
#     make clean && make AC_ANY_CHANNEL=1
#
# Usage: sweep_channels.sh <segment-dir> [board] [bw]
set -e

SEGDIR=${1:?segment directory}
BOARD=${2:-d6220}
BW=${3:-bw20}
OUT=${OUT:-/tmp/sweep-run}
mkdir -p "$OUT"

printf '%5s %8s %8s %10s %10s %s\n' ch vendor port full phy-rad-tbl status

for seg in "$SEGDIR"/*-up-ch*-"$BW".txt; do
	[ -e "$seg" ] || continue
	base=$(basename "$seg" .txt)
	ch=$(echo "$base" | sed 's/.*-ch\([0-9]*\)-.*/\1/')

	python3 ../reverse-tools/merge_retvals.py "$seg" "$OUT/$base.merged" \
		>/dev/null 2>&1

	first=$(grep -m1 -oE '#[0-9]+' "$seg" | tr -d '#')

	if ! AC_CHANNEL="$ch" AC_FIRST_INIT=0 \
	     AC_READ_ORACLE="$OUT/$base.merged" \
	     AC_READ_ORACLE_FROM="$first" \
	     ./ac_trace switch_channel "$BOARD" \
	     > "$OUT/$base.port" 2> "$OUT/$base.err"; then
		printf '%5s %8s %8s %10s %10s %s\n' "$ch" - - - - "flow failed"
		continue
	fi

	python3 ../reverse-tools/sweep_score.py \
		"$OUT/$base.merged" "$OUT/$base.port" "$ch"
done
