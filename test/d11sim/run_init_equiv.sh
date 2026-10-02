#!/bin/sh
# Init-only state equivalence between b43 and wl, by final core state and RAM
# rather than op-by-op.
#
# Each side becomes a final-state dump:
#   host writes   : the driver's op stream replayed into the d11 core model
#   ucode writes  : the real firmware-42 version-stamp prologue, executed on
#                   that side's blob through b43-tools' arch15 interpreter
# then the two dumps are diffed over the host-configuration surface.
#
# The wl side runs from a bus capture here. The b43 side needs an op stream
# from test/integration (make run ... B43_TRACE_OUT=b43.trace); that harness
# needs kernel headers, so it is produced separately and passed in.
#
# Usage:
#   run_init_equiv.sh --wl-bin FILE  --wl-blob FILE \
#                     --b43-trace FILE --b43-blob FILE \
#                     [--start 0xF76] [--work DIR]
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIM="$(dirname "$0")/d11sim"
MMIO2OPS="$REPO/reverse-tools/mmio2ops.py"
CMP="$(dirname "$0")/compare_state.py"

WL_BIN= WL_BLOB= B43_TRACE= B43_BLOB= START=0xF76 WORK=/tmp/d11sim-equiv
B43_TOOLS=${B43_TOOLS:-}
while [ $# -gt 0 ]; do
	case "$1" in
	--wl-bin)    WL_BIN=$2;    shift 2 ;;
	--wl-blob)   WL_BLOB=$2;   shift 2 ;;
	--b43-trace) B43_TRACE=$2; shift 2 ;;
	--b43-blob)  B43_BLOB=$2;  shift 2 ;;
	--start)     START=$2;     shift 2 ;;
	--work)      WORK=$2;      shift 2 ;;
	--b43-tools) B43_TOOLS=$2; shift 2 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done
[ -n "$WL_BIN" ] && [ -n "$B43_TRACE" ] || { echo "need --wl-bin and --b43-trace" >&2; exit 2; }

# The interpreter-side tools live in b43-tools (clone github.com/aleferri/b43-tools).
[ -n "$B43_TOOLS" ] || for d in "$REPO/../b43-tools" "$HOME/b43-tools"; do
	[ -f "$d/interpreter/ucode_init.py" ] && B43_TOOLS=$d && break
done
[ -n "$B43_TOOLS" ] && [ -f "$B43_TOOLS/interpreter/ucode_init.py" ] || {
	echo "set B43_TOOLS or pass --b43-tools (clone github.com/aleferri/b43-tools)" >&2
	exit 2
}
UCODE_INIT="$B43_TOOLS/interpreter/ucode_init.py"
mkdir -p "$WORK"
[ -x "$SIM" ] || make -C "$(dirname "$0")" >/dev/null

echo "## wl side"
python3 "$MMIO2OPS" "$WL_BIN" --no-bulk -o "$WORK/wl.ops"
"$SIM" replay --ops "$WORK/wl.ops" --label wl --dump "$WORK/wl.state"
WL_UC=
if [ -n "$WL_BLOB" ]; then
	python3 "$UCODE_INIT" "$WL_BLOB" --start "$START" --out "$WORK/wl.ucode"
	WL_UC="--ucode-a $WORK/wl.ucode"
fi

echo "## b43 side"
"$SIM" replay --ops "$B43_TRACE" --label b43 --dump "$WORK/b43.state"
B43_UC=
if [ -n "$B43_BLOB" ]; then
	python3 "$UCODE_INIT" "$B43_BLOB" --start "$START" --out "$WORK/b43.ucode"
	B43_UC="--ucode-b $WORK/b43.ucode"
fi

echo "## equivalence"
# shellcheck disable=SC2086
python3 "$CMP" "$WORK/b43.state" "$WORK/wl.state" \
	$B43_UC $WL_UC --label-a b43 --label-b wl
