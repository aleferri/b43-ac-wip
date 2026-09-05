#!/bin/sh
# The port's gate against a sweep segment.
#
# Two conditions, and they are not interchangeable: --cold measures a first
# bring-up, --hot measures a cycle on an interface that is already up. Both
# are needed, because a predicate that tells the two apart is invisible in the
# other one.
#
# --cold (default)
#   The cold sweep: a module reloaded per channel, so every segment is an
#   isolated attach with all the trace classes armed. A segment is compared
#   against the `full` flow and whole, not split into `down` and `up`: the
#   module is loaded inside the segment, so the eleven reads of
#   rx_gain_regs_program() are in the segment and the oracle serves them where
#   the vendor made them. Splitting a cold segment instead leaves the head of
#   the attach (switch_analog and the GPIOs) and the tail after the last probe
#   group out of the comparison.
#
# --hot
#   The `up` segments of the hot sweep. Every predicate that tells a first
#   bring-up from a later one is invisible in the cold sweep -- it reads the
#   same on all 26 segments -- and a missing term does not show. That is
#   exactly what happened to b43_phy_ac_may_calibrate_tx(), which only looked
#   at center_freq <= 5250: cold the scores added up, and on the up segments
#   above 5250 the port sat at 35% instead of 80%. Forty-five points on half
#   the driver that nobody was measuring. The three default segments are
#   chosen to catch that case: one below 5250 MHz and two above. If a
#   predicate confuses the two conditions, the first stays put and the other
#   two collapse.
#
#   A hot segment does not contain the module load, so the flow must be `up`
#   and not `full`: the eleven reads of rx_gain_regs_program() fall before the
#   start of the segment, and a flow that executes them drains the oracle
#   queues the phase under test needs. AC_FIRST_INIT=0, because a hot segment
#   is not a first bring-up and the vendor's init_regs takes its two-pass
#   branch.
#
# The comparison window and the oracle window are two different things, and
# they start at different places:
#
#   from    the attach's first PHY op, the first of the ten save reads that
#           open switch_analog_once(). The comparison starts here. It must not
#           be hooked to the AFE bank (PHY.WR 0x173e), which comes after those
#           ten reads, and it must not be "any first PHY op": before it the
#           core writes PHY registers of its own (cold03 #452, PHY.WR 0xa6).
#   oracle  cold: the insmod, so the per-address queues also cover the OTP and
#           the core probe. Hot: the window start, because there is no insmod
#           to start from -- the OTPs and the core probe are outside the
#           segment.
#
# Anything but ch36 bw20 currently needs the validated-configs guard in
# set_channel() defeated at build time:
#     make clean && make AC_ANY_CHANNEL=1
#
# Usage:
#   ./gates.sh [segment...]                     cold, default ch36 bw20
#   ./gates.sh --hot [segment...]               hot, three default segments
#   ./gates.sh --hot --flow switch_channel DIR  one row per channel, over a dir
#
# Environment: COLD, HOT override the segment directories.

set -e
HERE=$(dirname "$0")
COLD=${COLD:-/tmp/cold/segmenti}
HOT=${HOT:-/tmp/hot/segmenti}

COND=cold
FLOW=
TABLE=0
while [ $# -gt 0 ]; do
	case $1 in
	--cold) COND=cold; shift ;;
	--hot)  COND=hot;  shift ;;
	--flow) FLOW=$2;   shift 2 ;;
	--table) TABLE=1;  shift ;;
	--) shift; break ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

if [ "$COND" = cold ]; then
	DIR=$COLD
	ARCHIVE=cold-sweep.zip
	: "${FLOW:=full}"
	FIRST_INIT=1
	DEFAULT="$COLD/cold01-ch36-bw20.txt"
else
	DIR=$HOT
	ARCHIVE=hot-sweep.zip
	: "${FLOW:=up}"
	FIRST_INIT=0
	DEFAULT="$HOT/01-up-ch36-bw20.txt $HOT/09-up-ch52-bw20.txt $HOT/19-up-ch104-bw20.txt"
fi

# switch_channel executes a single warm cycle, so its oracle starts at the
# very first op of the segment and its score is the one-row-per-channel table.
[ "$FLOW" = switch_channel ] && TABLE=1

if [ ! -d "$DIR" ]; then
	echo "missing $DIR:"
	echo "  unzip -d $(dirname "$DIR") router-data/d6220/$ARCHIVE"
	exit 1
fi

if [ $# -gt 0 ]; then
	if [ -d "$1" ]; then
		SEGS=$(ls "$1"/*-up-ch*.txt 2>/dev/null)
	else
		SEGS=$*
	fi
else
	SEGS=$DEFAULT
fi

[ "$TABLE" = 1 ] && printf '%5s %8s %8s %10s %10s %s\n' \
	ch vendor port full phy-rad-tbl status

fail=0
for seg in $SEGS; do
	[ -f "$seg" ] || { echo "missing $seg"; fail=1; continue; }
	base=$(basename "$seg" .txt)
	ch=$(echo "$base" | sed -n 's/.*ch\([0-9]*\)-bw.*/\1/p')
	bw=$(echo "$base" | sed -n 's/.*-bw\([0-9]*\).*/\1/p')

	# Cold only: an op absent from a capture that does not trace its class
	# says nothing about the driver, and deducing something from it has
	# produced wrong conclusions about the chanspec, the probe-response
	# writes and the noise sample.
	[ "$COND" = cold ] &&
		python3 "$HERE/../reverse-tools/check_class_coverage.py" \
			--require "$seg"

	python3 "$HERE/../reverse-tools/trace_filter.py" --retvals "$seg" \
		/tmp/gate.merged >/dev/null 2>&1 || cp "$seg" /tmp/gate.merged

	eval "$(python3 - /tmp/gate.merged "$FLOW" <<'PY'
import re, sys

path, flow = sys.argv[1], sys.argv[2]
first_op = first_phy = insmod = None
for line in open(path, errors='replace'):
    m = re.match(r"\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(\S+)", line)
    if not m:
        continue
    if first_op is None:
        first_op = m.group(1)
    if insmod is None and re.search(r"MARK\s+'mod COMING'", line):
        insmod = m.group(1)
    if first_phy is None and re.match(
            r"\s*[\d.]+\s+#\d+\s+cpu\d+\s+PHY\.RD\s+addr=0x0*739\s", line):
        first_phy = m.group(1)
    if insmod and first_phy:
        break

start = first_op if flow == 'switch_channel' else first_phy
print(f"from={start or ''}")
print(f"oracle={insmod or start or ''}")
PY
)"
	[ -n "$from" ] || { echo "$seg: no PHY op"; fail=1; continue; }
	last=$(grep -oE '#[0-9]+' /tmp/gate.merged | tail -1 | tr -d '#')

	# The probe phase deadline and the watchdog tick are clocks, and the
	# clock is in the segment's timestamps. See probe_schedule.py.
	sched=$(python3 "$HERE/../reverse-tools/probe_schedule.py" \
		/tmp/gate.merged --sh 2>/dev/null || true)

	# MAC.BW is written only by the first segment of each bandwidth: the
	# others inherit it. The segment knows by itself whether it has it.
	if grep -q ' MAC\.BW' /tmp/gate.merged; then
		macw=0
	else
		macw=$(case $bw in 40) echo 2 ;; 80) echo 3 ;; *) echo 1 ;; esac)
	fi

	[ "$TABLE" = 1 ] || echo "=== $base (from $from${sched:+, $sched}) ==="

	# shellcheck disable=SC2086
	if ! env AC_CHANNEL=$ch AC_BW=$bw AC_MAC_WIDTH=$macw \
	     AC_FIRST_INIT=$FIRST_INIT \
	     AC_READ_ORACLE=/tmp/gate.merged AC_READ_ORACLE_FROM=$oracle $sched \
		"$HERE/ac_trace" "$FLOW" d6220 2>/dev/null > /tmp/gate.full; then
		if [ "$TABLE" = 1 ]; then
			printf '%5s %8s %8s %10s %10s %s\n' \
				"$ch" - - - - "flow failed"
			continue
		fi
		echo "  the flow failed"; fail=1; continue
	fi

	# A flow that bails out early still produces a score, and that score
	# looks like a regression of the port rather than a run that never
	# happened, so it is said. The known cause today is the validated-configs
	# guard in set_channel(), which rejects anything but ch36 BW20 unless the
	# build defeats it; any other early exit deserves the same warning.
	# In --table mode it goes to stderr, because a line on stdout would
	# break the table; in detail mode it stays on stdout, next to the score
	# it disclaims -- a reader who redirects stderr away would otherwise see
	# a 2% score with no explanation of why the run never happened.
	nport=$(grep -c '^cpu' /tmp/gate.full || true)
	if [ "$nport" -lt 6000 ]; then
		{
		echo "  WARNING: the port emitted only $nport ops on ch$ch/bw$bw:"
		echo "  the flow bailed out early, the score below is not a"
		echo "  measurement of the port. If it is the channel guard:"
		echo "  make clean && make AC_ANY_CHANNEL=1"
		} >&$([ "$TABLE" = 1 ] && echo 2 || echo 1)
	fi

	if [ "$TABLE" = 1 ]; then
		python3 "$HERE/../reverse-tools/sweep_report.py" score \
			/tmp/gate.merged /tmp/gate.full "$ch"
		continue
	fi

	# The two measures, with the reference tools.
	#
	# The line to look at is "grezzo": wl ops reproduced in order over ALL
	# of wl's, because the goal is for b43 to emit them all, the core's
	# included. "nel perimetro" removes what the harness cannot emit and
	# only serves to show the next PHY divergence without stopping at the
	# first core op -- it is navigation, not a score. The core's debt is in
	# docs/retrace-todo.md.
	#
	# compare.py is the position-by-position comparison, which stops at the
	# first divergence and gives the context. The two measures are not
	# comparable: the first tolerates insertions, the second does not.
	echo "  --- cmp_skip ---"
	python3 "$HERE/cmp_skip.py" /tmp/gate.merged /tmp/gate.full \
		"$from:$last" --board d6220 \
		| grep -E 'grezzo|nel perimetro|CON  ecce|fuori perimetro|op saltate|valore sbagliato|op di wl mancanti|solo vendor|solo port|invisibili'
	echo "  --- compare ---"
	python3 "$HERE/compare.py" /tmp/gate.merged /tmp/gate.full \
		--range "$from:$last" --auto-align \
		> /tmp/gate.cmp || fail=1
	if [ "$COND" = cold ]; then
		sed -n '1,4p;/^  @/{p;q}' /tmp/gate.cmp
	else
		sed -n '/^  @/{p;q}' /tmp/gate.cmp
	fi
done

exit $fail
