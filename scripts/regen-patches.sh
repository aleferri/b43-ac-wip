#!/bin/sh
# Rigenera le patch del kernel, patches/0001-0003, dagli alberi bcma/ e b43/.
#
# bcma/ e b43/ sono la fonte di verita': file del kernel interi, modificati o
# nuovi, sulla versione dei test (test/integration). Le patch sono un prodotto
# e stanno sul tag di patches/regen-base, che e' un altro: le loro differenze
# dall'albero, oltre al port, sono l'adattamento a quel tag, e si conservano.
# Lo script porta sulle patch correnti quello che e' cambiato in bcma/ e b43/
# da quando sono state generate (scripts/carry.sh), e registra in
# patches/regen-base gli alberi di HEAD da cui ha generato.
#
#   0001  bcma/drivers/**, bcma/include/**  ai loro percorsi nel kernel
#   0002  b43/ tranne i file della PHY AC    drivers/net/wireless/broadcom/b43/
#   0003  i file della PHY AC                drivers/net/wireless/broadcom/b43/
#
# I file della PHY AC sono il Makefile e quelli che il Makefile compila sotto
# CONFIG_B43_PHY_AC, con i loro .h: un file nuovo entra nella 0003 dalla sua
# riga nel Makefile, e tutto il resto di b43/ va nella 0002. Messaggio,
# autore e data vengono dalla patch corrente: per cambiarli si edita la
# patch e si rilancia.
#
# bcma/ e b43/ devono essere committati. I file del tag vengono da GitHub; un
# file che al tag non esiste (404) e' nuovo, ogni altro errore di rete ferma
# lo script.
#
# Uso:    scripts/regen-patches.sh
# Env:    CARRY_RESUME  la directory lasciata da un merge in conflitto, dopo
#                       averlo risolto
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$REPO/scripts/carry.sh"

B43DIR=drivers/net/wireless/broadcom/b43
CARRY_BASEFILE=$REPO/patches/regen-base
CARRY_TOPS="bcma/drivers bcma/include b43"
TAG=$(carry_base_get tag)
[ -n "$TAG" ] || carry_die "$CARRY_BASEFILE: manca tag"

CARRY_PATCHES=
for n in 0001 0002 0003; do
	p=$(ls "$REPO/patches/$n"-*.patch 2>/dev/null) ||
		carry_die "patches/$n-*.patch assente: serve per il messaggio"
	CARRY_PATCHES="$CARRY_PATCHES $p"
done

carry_map() {
	for top in drivers include; do
		git -C "$REPO" ls-tree -r --name-only \
			"$(echo "$1" | sed -n "s|^bcma/$top ||p")" |
			sed "s|.*|bcma/$top/& $top/& 1|"
	done
	b43=$(echo "$1" | sed -n 's/^b43 //p')
	ac=$(git -C "$REPO" show "$b43:Makefile" |
		sed -n 's/^b43-$(CONFIG_B43_PHY_AC)[[:space:]]*+=//p' |
		tr ' \t' '\n\n' | sed -n 's/\.o$//p')
	git -C "$REPO" ls-tree --name-only "$b43" | while read -r f; do
		n=2
		case $f in
		Makefile) n=3 ;;
		*.c|*.h) for base in $ac; do
				[ "$f" = "$base.c" ] || [ "$f" = "$base.h" ] && n=3
			done ;;
		esac
		echo "b43/$f $B43DIR/$f $n"
	done
}

carry_filter() {
	cat
}

carry_base_files() {
	root=https://raw.githubusercontent.com/torvalds/linux/$TAG
	while read -r dst; do
		mkdir -p "$WORK/tree/$(dirname "$dst")"
		code=$(curl -sL -o "$WORK/tree/$dst" -w '%{http_code}' "$root/$dst") ||
			code=000
		case $code in
		200) ;;
		404) rm -f "$WORK/tree/$dst" ;;
		*) carry_die "fetch di $dst a $TAG fallito ($code)" ;;
		esac
	done
}

if [ -n "${CARRY_RESUME:-}" ]; then
	carry_resume
else
	carry_run
fi

for p in $CARRY_PATCHES; do
	rm -f "$p"
done
mv "$WORK"/out/*.patch "$REPO/patches/"
carry_record "tag $TAG"
carry_done
echo "rigenerate: $(cd "$REPO/patches" && ls 000[1-3]-*.patch | tr '\n' ' ')"
