#!/bin/sh
# Which register reads does the port actually consume?
#
# Static analysis cannot answer this: b43_phy_read_log() is a macro and what
# matters is whether the caller uses the result, which is invisible at runtime,
# and two thirds of the read sites compute their address so a source scan
# cannot even name them.
#
# So ask the question the other way round. Run the flow once for a baseline,
# then once per read address with the oracle's values for that address
# perturbed. If the emitted trace changes, the driver consumed the value. If
# it does not, the driver discarded it -- and if the captures show that value
# varying between configurations, that is a latent bug: the port is
# reproducing whatever constant held on the board the capture came from.
#
# Usage: consumed_reads.sh <merged-capture> [flow] [board] [oracle-from]

set -e
CAP=${1:?merged capture}
FLOW=${2:-switch_channel}
BOARD=${3:-d6220}
FROM=${4:-0}

run() {
	AC_FIRST_INIT=${AC_FIRST_INIT:-0} \
	AC_READ_ORACLE="$CAP" AC_READ_ORACLE_FROM="$FROM" \
	AC_READ_PERTURB="$1" AC_READ_PERTURB_KIND="$2" \
	AC_READ_PERTURB_MASK="${MASK:-1}" \
	./ac_trace "$FLOW" "$BOARD" 2>/dev/null
}

# Two corrections the naive version needs:
#
#  - the read's own trace line carries the value, so perturbing it always
#    shows up; that line is filtered out of both sides before diffing, or
#    every read looks consumed.
#  - a one-bit perturbation can be legitimately masked away. PHY 0x0012 is
#    consumed as `read >> 2`, which discards exactly the bit a 0x0001 flip
#    touches, so it looked discarded. Several masks are tried and the address
#    counts as consumed if any of them reaches the output.

MASKS=${MASKS:-"0x0001 0x0004 0x0040 0x0400 0x4000"}

printf '%-6s %-8s %8s  %s\n' kind addr 'max diff' verdict
printf -- '------------------------------------------------\n'

# every address the capture supplies a value for, by kind
for kind in phy radio; do
	case $kind in
	phy)   pat='PHY\.RD' ;;
	radio) pat='RAD\.RD' ;;
	esac
	addrs=$(grep -oE "$pat +addr=0x[0-9a-f]+" "$CAP" 2>/dev/null |
		grep -oE '0x[0-9a-f]+' | sort -u)
	for a in $addrs; do
		short=$(printf '%s' "$a" | sed 's/^0x0*//')
		filt="$pat +addr=0x0*$short "
		run "" phy | grep -vE "$filt" > /tmp/consumed.base
		best=0
		for m in $MASKS; do
			MASK=$m run "$a" "$kind" | grep -vE "$filt" \
				> /tmp/consumed.pert
			d=$(diff /tmp/consumed.base /tmp/consumed.pert |
			    grep -c '^[<>]' || true)
			[ "$d" -gt "$best" ] && best=$d
		done
		if [ "$best" -gt 0 ]; then
			v="consumed"
		else
			v="DISCARDED"
		fi
		printf '%-6s %-8s %8s  %s\n' "$kind" "$a" "$best" "$v"
	done
done
