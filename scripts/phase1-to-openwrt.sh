#!/usr/bin/env bash
#
# phase1-to-openwrt.sh - convert the phase1 b43 AC-PHY patches into the form
# OpenWrt's mac80211 package expects and drop them into a target tree.
#
# What it does:
#   - selects only the b43 patches (patches/*-b43-*.patch);
#     the ssb/bcma patches are left out
#   - skips debug-only patches (NNNN-b43-DEBUG-*.patch); those are local
#     bring-up aids and must not be shipped to OpenWrt
#   - renames  NNNN-b43-<desc>.patch  ->  816-NN-<short>.patch
#     (NN = the two-digit source number, so apply order is preserved)
#   - CONFIG_B43*  ->  CPTCFG_B43*   (backports config namespace)
#   - b43dbg(...)  ->  b43info(...)  (promote debug logging to info)
#   - writes everything into
#       $TARGET_DIR/package/kernel/mac80211/patches/brcm
#
# Usage:
#   TARGET_DIR=/path/to/openwrt ./scripts/phase1-to-openwrt.sh
#   ./scripts/phase1-to-openwrt.sh /path/to/openwrt
#
set -eu

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC_DIR="$SCRIPT_DIR/../patches"

TARGET_DIR="${1:-${TARGET_DIR:-}}"
if [ -z "$TARGET_DIR" ]; then
	echo "error: target tree not given (set \$TARGET_DIR or pass it as arg 1)" >&2
	exit 1
fi
if [ ! -d "$SRC_DIR" ]; then
	echo "error: patch source not found: $SRC_DIR" >&2
	exit 1
fi

OUT_DIR="$TARGET_DIR/package/kernel/mac80211/patches/brcm"

# Short name per patch, matched on the descriptive part of the filename so it
# survives renumbering. Unknown patches fall back to a slug of that same part.
short_name() {
	case "$1" in
	*register-definitions*) echo "acphy-regdefs" ;;
	*5-GHz-channel*)        echo "acphy-5ghz-channels" ;;
	*dma-64k*)              echo "dma-64k-align" ;;
	*2069-radio*)           echo "acphy-radio-2069" ;;
	*init-channel-tuning*)  echo "acphy-init" ;;
	*wire-AC-PHY-into*)     echo "acphy-txrx-wiring" ;;
	*)                      echo "" ;;
	esac
}

mkdir -p "$OUT_DIR"

count=0
for src in "$SRC_DIR"/*-b43-*.patch; do
	[ -e "$src" ] || { echo "error: no b43 patches in $SRC_DIR" >&2; exit 1; }

	base=$(basename "$src")

	# debug-only bring-up patches are not shipped to OpenWrt
	case "$base" in
	*-b43-DEBUG-*) printf '  skip (debug)  %s\n' "$base"; continue ;;
	esac

	num=${base%%-*}			# NNNN
	xx=${num: -2}			# two-digit suffix, e.g. 0011 -> 11

	name=$(short_name "$base")
	if [ -z "$name" ]; then
		# fallback: slug from the descriptive part of the filename
		name=$(printf '%s' "${base#*-b43-}" \
			| sed -e 's/\.patch$//' -e 's/^add-//' -e 's/-for-AC-PHY//' \
			      -e 's/[^A-Za-z0-9]\{1,\}/-/g' \
			| tr 'A-Z' 'a-z')
		name=${name%-}
	fi

	dst="$OUT_DIR/816-$xx-$name.patch"
	sed -e 's/CONFIG_B43/CPTCFG_B43/g' \
	    -e 's/b43dbg(/b43info(/g' \
	    "$src" > "$dst"

	printf '  %s  ->  %s\n' "$base" "816-$xx-$name.patch"
	count=$((count + 1))
done

printf 'converted %d b43 patch(es) into %s\n' "$count" "$OUT_DIR"
