#!/bin/sh
# Compare one sweep segment against the port, one phase at a time.
#
# A segment is a whole up cycle and no single flow reproduces it, so it is
# compared in two halves:
#
#   down  the radio init that heads the segment, up to the anchor
#   up    everything after it: rfkill, op_init, switch_channel
#
# Each half needs its own flow, and the reason is the read oracle. The oracle
# is a per-address queue seeded from the segment, and switch_analog() calls
# rx_gain_regs_program(), whose eleven reads happen *before* a segment starts.
# A flow that runs them takes the queue entries for 0x0728, 0x0725, 0x073a,
# 0x0720 and 0x0408 that the phase under test needs, so the phase then reads
# the second value for each and every read-modify-write after it lands one
# field off. That is why "full" cannot be used for either half.
#
# The anchor between the halves is RAD.MOD 0x08ea, the first op of the up. Its
# episode is computed rather than hardcoded: it moves with the segment.
#
# Usage: sweep_compare.sh <segment.txt> [down|up|both]

set -e
SEG=${1:?path to a sweep segment}
WHICH=${2:-both}
HERE=$(dirname "$0")
MERGED=/tmp/sweep_compare.merged
PORT=/tmp/sweep_compare.port

CH=$(basename "$SEG" | sed -n 's/.*ch\([0-9]*\)-bw.*/\1/p')
BW=$(basename "$SEG" | sed -n 's/.*-bw\([0-9]*\).*/\1/p')

# MAC.BW is written only when the operating width changes, so a segment that is
# not the first of its width inherits it. Default to "already set" and let the
# caller override with AC_MAC_WIDTH when replaying the first segment of a group.
case $BW in
40) MACW=2 ;;
80) MACW=3 ;;
*)  MACW=1 ;;
esac

python3 "$HERE/../reverse-tools/merge_retvals.py" "$SEG" "$MERGED" >/dev/null 2>&1
FIRST=$(grep -m1 -oE '#[0-9]+' "$SEG" | tr -d '#')

# episode of the anchor, and the last episode of the segment
ANCHOR=$(python3 - "$MERGED" <<'PY'
import re, sys
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+RAD\.MOD\s+addr=0x0*8ea\s+'
                 r'val=0x0*40\s+mask=0x0*40', line)
    if m:
        print(m.group(1))
        break
PY
)
LAST=$(grep -oE '#[0-9]+' "$SEG" | tail -1 | tr -d '#')

# The up window closes before the head of the next down; see find_up_end.py.
UPEND=$(python3 "$HERE/../reverse-tools/find_up_end.py" "$MERGED")
[ -n "$UPEND" ] || UPEND=$LAST

run() {
	AC_CHANNEL=$CH AC_BW=$BW AC_FIRST_INIT=0 \
	AC_MAC_WIDTH=${AC_MAC_WIDTH:-$MACW} \
	AC_LAST_CAL_CHANNEL=${AC_LAST_CAL_CHANNEL:-0} \
	AC_PROBE_TICKS=${AC_PROBE_TICKS:-10} \
	AC_WATCHDOG_TICKS=${AC_WATCHDOG_TICKS:-4} \
	AC_CRS_INDEX=${AC_CRS_INDEX:-0} \
	AC_CRS_SUBBAND=${AC_CRS_SUBBAND:-255} \
	AC_READ_ORACLE="$MERGED" AC_READ_ORACLE_FROM="$FIRST" \
	"$HERE/ac_trace" "$1" "${BOARD:-d6220}" 2>/dev/null > "$PORT"
}

echo "segment ch$CH bw$BW: episodes $FIRST..$LAST, anchor at $ANCHOR"

if [ "$WHICH" = down ] || [ "$WHICH" = both ]; then
	run down
	echo "--- down ---"
	python3 "$HERE/../reverse-tools/phase_diff.py" "$MERGED" "$PORT" \
		--until "$ANCHOR"
fi

if [ "$WHICH" = up ] || [ "$WHICH" = both ]; then
	run up
	echo "--- up ---"
	python3 "$HERE/../reverse-tools/phase_diff.py" "$MERGED" "$PORT" \
		--from "$ANCHOR" --until "$UPEND"
fi
