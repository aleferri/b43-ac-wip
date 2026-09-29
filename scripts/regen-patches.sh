#!/bin/sh
# Rigenera patches/ dagli alberi bcma/ e b43/.
#
# bcma/ e b43/ sono la fonte di verita': file del kernel interi, modificati o
# nuovi, sopra il tag vanilla. Le patch sono un prodotto e non si editano a
# mano, tranne il messaggio: messaggio, autore e data vengono dalla patch
# corrente (git mailinfo), quindi per cambiarli si edita la patch e si
# rilancia.
#
#   0001  bcma/drivers/**, bcma/include/**  ai loro percorsi nel kernel
#   0002  b43/ tranne i file della PHY AC    drivers/net/wireless/broadcom/b43/
#   0003  i file della PHY AC                drivers/net/wireless/broadcom/b43/
#
# I file della PHY AC sono il Makefile e quelli che il Makefile compila sotto
# CONFIG_B43_PHY_AC, con i loro .h: un file nuovo entra nella 0003 dalla sua
# riga nel Makefile, e tutto il resto di b43/ va nella 0002.
#
# La base e' il tag degli header installati, come per test/integration: un
# file che al tag non esiste (404) e' nuovo, ogni altro errore di rete ferma
# lo script, o un fetch andato male diventerebbe un file nuovo in silenzio.
#
# Uso:    scripts/regen-patches.sh
# Env:    KVER    versione degli header (default: la prima in /usr/src)
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PATCHES=$REPO/patches
B43DIR=drivers/net/wireless/broadcom/b43
KVER=${KVER:-$(ls /usr/src/ | grep -oE '^linux-headers-[0-9.]+-[0-9]+$' |
                head -1 | sed 's/linux-headers-//')}
[ -n "$KVER" ] || { echo "nessun header kernel in /usr/src" >&2; exit 1; }
TAG=v$(echo "$KVER" | cut -d. -f1,2)
ROOT=https://raw.githubusercontent.com/torvalds/linux/$TAG

for n in 0001 0002 0003; do
	ls "$PATCHES/$n"-*.patch >/dev/null 2>&1 ||
		{ echo "patches/$n-*.patch assente: serve per il messaggio" >&2; exit 1; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ac_files=$(sed -n 's/^b43-$(CONFIG_B43_PHY_AC)[[:space:]]*+=//p' \
		"$REPO/b43/Makefile" | tr ' \t' '\n\n' | sed -n 's/\.o$//p' |
	while read -r base; do
		for ext in c h; do
			[ -f "$REPO/b43/$base.$ext" ] && echo "$base.$ext"
		done
	done)
ac_files="Makefile $ac_files"

is_ac() {
	for a in $ac_files; do
		[ "$a" = "$1" ] && return 0
	done
	return 1
}

# Elenco "sorgente percorso-nel-kernel" per ciascuna patch.
(cd "$REPO/bcma" && find drivers include -type f | sort) |
	sed 's|.*|bcma/& &|' > "$WORK/list.0001"
: > "$WORK/list.0002"
: > "$WORK/list.0003"
for f in $(cd "$REPO/b43" && ls); do
	if is_ac "$f"; then n=0003; else n=0002; fi
	echo "b43/$f $B43DIR/$f" >> "$WORK/list.$n"
done

mkdir "$WORK/tree"
cd "$WORK/tree"
git init -q .
git config user.name  "b43-ac regen"
git config user.email "regen@localhost"

cat "$WORK"/list.* | while read -r src dst; do
	mkdir -p "$(dirname "$dst")"
	code=$(curl -sL -o "$dst" -w '%{http_code}' "$ROOT/$dst") || code=000
	case $code in
	200) ;;
	404) rm -f "$dst" ;;
	*) echo "fetch di $dst a $TAG fallito ($code)" >&2; exit 1 ;;
	esac
done
git add -A
git commit -qm "vanilla $TAG"

for n in 0001 0002 0003; do
	old=$(ls "$PATCHES/$n"-*.patch)
	git mailinfo "$WORK/msg" /dev/null < "$old" > "$WORK/info"
	field() { sed -n "s/^$1: //p" "$WORK/info"; }
	while read -r src dst; do
		mkdir -p "$(dirname "$dst")"
		cp "$REPO/$src" "$dst"
	done < "$WORK/list.$n"
	git add -A
	{ field Subject; echo; cat "$WORK/msg"; } > "$WORK/commitmsg"
	GIT_AUTHOR_NAME=$(field Author) GIT_AUTHOR_EMAIL=$(field Email) \
	GIT_AUTHOR_DATE=$(field Date) GIT_COMMITTER_DATE=$(field Date) \
		git commit -q -F "$WORK/commitmsg"
done

rm -f "$PATCHES"/*.patch
git format-patch -q --zero-commit --no-signature -o "$PATCHES" HEAD~3
echo "rigenerate: $(cd "$PATCHES" && ls | tr '\n' ' ')"
