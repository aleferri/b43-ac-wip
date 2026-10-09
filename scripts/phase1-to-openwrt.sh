#!/bin/sh
# Rigenera le patch b43 per OpenWrt, patches/816-02 e 816-03, e le installa in
# un albero OpenWrt.
#
# Stanno sul b43 di backports con le patch di OpenWrt che lo precedono nel
# pacchetto mac80211 (le brcm/ prima di 816 e le altre directory in ordine di
# Build/Patch): la base si ricostruisce dal tarball di backports e dalle patch
# dell'albero OpenWrt dato, e ogni file della 816 deve combaciare con essa.
# Come regen-patches.sh, lo script porta sulle 816 correnti quello che e'
# cambiato in b43/ da quando sono state generate (scripts/carry.sh) e registra
# in patches/regen-base-openwrt l'albero di HEAD e la versione di backports.
# Le 816 seguono la 0002 e la 0003: b43/ con CPTCFG_B43 al posto di
# CONFIG_B43.
#
# Le patch bcma/ssb (880) non stanno su backports ma sul kernel di OpenWrt, e
# non passano di qui.
#
# Uso:    scripts/phase1-to-openwrt.sh /percorso/openwrt
#         TARGET_DIR=/percorso/openwrt scripts/phase1-to-openwrt.sh
# Env:    CARRY_RESUME  la directory lasciata da un merge in conflitto, dopo
#                       averlo risolto
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$REPO/scripts/carry.sh"

TARGET_DIR=${1:-${TARGET_DIR:-}}
[ -n "$TARGET_DIR" ] ||
	carry_die "albero OpenWrt non dato (\$TARGET_DIR o primo argomento)"
MAC80211=$TARGET_DIR/package/kernel/mac80211
[ -f "$MAC80211/Makefile" ] || carry_die "$MAC80211/Makefile assente"
OUT_DIR=$MAC80211/patches/brcm
SERIES=816

B43DIR=drivers/net/wireless/broadcom/b43
CARRY_BASEFILE=$REPO/patches/regen-base-openwrt
CARRY_TOPS=b43

mk() { sed -n "s/^$1:=//p" "$MAC80211/Makefile" | head -1; }
VERSION=$(mk PKG_SOURCE_VERSION)
expand() { echo "$1" | sed "s/\$(PKG_SOURCE_VERSION)/$VERSION/g"; }
SOURCE=$(expand "$(mk PKG_SOURCE)")
SOURCE_URL=$(expand "$(mk PKG_SOURCE_URL)")
HASH=$(mk PKG_HASH)
[ -n "$VERSION" ] && [ -n "$SOURCE" ] && [ -n "$SOURCE_URL" ] ||
	carry_die "$MAC80211/Makefile: PKG_SOURCE_VERSION/PKG_SOURCE/PKG_SOURCE_URL non trovati"

CARRY_PATCHES=
for n in 02 03; do
	p=$(ls "$REPO/patches/$SERIES-$n"-*.patch 2>/dev/null) ||
		carry_die "patches/$SERIES-$n-*.patch assente: serve per il messaggio"
	CARRY_PATCHES="$CARRY_PATCHES $p"
done

carry_map() {
	b43=$(echo "$1" | sed -n 's/^b43 //p')
	ac=$(git -C "$REPO" show "$b43:Makefile" |
		sed -n 's/^b43-$(CONFIG_B43_PHY_AC)[[:space:]]*+=//p' |
		tr ' \t' '\n\n' | sed -n 's/\.o$//p')
	git -C "$REPO" ls-tree --name-only "$b43" | while read -r f; do
		n=1
		case $f in
		Makefile) n=2 ;;
		*.c|*.h) for base in $ac; do
				[ "$f" = "$base.c" ] || [ "$f" = "$base.h" ] && n=2
			done ;;
		esac
		echo "b43/$f $B43DIR/$f $n"
	done
}

carry_filter() {
	sed -e 's/CONFIG_B43/CPTCFG_B43/g'
}

# Le patch del pacchetto nell'ordine di Build/Patch, fino a brcm/$SERIES
# escluso; quelle dopo che toccano b43 si segnalano.
openwrt_patches() {
	before=1
	for d in $(sed -n 's/.*PatchDir,$(PKG_BUILD_DIR),$(PATCH_DIR)\/\([^,]*\),.*/\1/p' \
			"$MAC80211/Makefile"); do
		for p in "$MAC80211/patches/$d"/*.patch; do
			[ -e "$p" ] || continue
			case $d/$(basename "$p") in
			brcm/$SERIES-*) before= ; continue ;;
			esac
			grep -q "^+++ b/$B43DIR/" "$p" || continue
			if [ -n "$before" ]; then
				echo "$p"
			else
				echo "attenzione: $d/$(basename "$p") tocca b43 dopo le $SERIES" >&2
			fi
		done
	done
}

carry_base_files() {
	cat > /dev/null
	tarball=$TARGET_DIR/dl/$SOURCE
	if [ ! -f "$tarball" ]; then
		tarball=$WORK/$SOURCE
		curl -sfL -o "$tarball" "$SOURCE_URL/$SOURCE" ||
			carry_die "download di $SOURCE_URL/$SOURCE fallito"
	fi
	if [ -n "$HASH" ] && [ "$HASH" != skip ]; then
		echo "$HASH  $tarball" | sha256sum -c --status ||
			carry_die "$tarball: sha256 diverso da PKG_HASH"
	fi
	PATH=$PATH:$TARGET_DIR/staging_dir/host/bin \
		tar -xf "$tarball" -C "$WORK/tree" --strip-components=1 \
			--wildcards "*/$B43DIR/*"
	for p in $(openwrt_patches); do
		git -C "$WORK/tree" apply --include="$B43DIR/*" "$p" ||
			carry_die "$p non si applica a backports $VERSION"
	done
}

if [ -n "${CARRY_RESUME:-}" ]; then
	carry_resume
else
	carry_run
fi

set -- "$WORK"/out/*.patch
for p in $CARRY_PATCHES; do
	mv "$1" "$p"
	shift
done
carry_record "backports $VERSION"
carry_done

mkdir -p "$OUT_DIR"
for p in $CARRY_PATCHES; do
	cp "$p" "$OUT_DIR/"
	echo "  $(basename "$p")  ->  $OUT_DIR/"
done
