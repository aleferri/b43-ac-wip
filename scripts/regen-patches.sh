#!/bin/sh
# Rigenera patches/0006 dai sorgenti in src/.
#
# src/ e' la fonte di verita'; questo script materializza la procedura
# descritta in docs/driver-status.md: ricostruisce l'albero che la 0006 ha per
# base, ci sovrascrive i file coperti da src/, e riemette la patch con
# git format-patch. Messaggio e author vengono dalla patch corrente (via
# git am), quindi per cambiarli basta editare la patch e rilanciare.
#
# Il base non e' mainline HEAD ma il tag da cui la suite di integrazione
# prende b43, che e' quello degli header kernel installati: la 0006 applica
# con `git am` senza -3 su vanilla + 0003 + 0004, e riemetterla su un altro
# tag la rebaserebbe in silenzio. Quindi si riusa fetch-upstream.sh invece di
# clonare il kernel: scarica i 35 file di b43/ al tag giusto, che e' tutto
# quello che serve.
#
# Uso:    scripts/regen-patches.sh
# Env:    KVER    versione degli header (default: la prima in /usr/src)
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
B43DIR=drivers/net/wireless/broadcom/b43
KVER=${KVER:-$(ls /usr/src/ | grep -oE '^linux-headers-[0-9.]+-[0-9]+$' |
                head -1 | sed 's/linux-headers-//')}
[ -n "$KVER" ] || { echo "nessun header kernel in /usr/src" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# b43 vanilla al tag degli header, con lo stesso script della suite ma in una
# dir a parte: l'albero di test/integration/ ha le patch del port applicate
# sopra e non va toccato, o il make successivo non trova piu' i file.
mkdir -p "$WORK/$(dirname $B43DIR)"
OUTDIR="$WORK/$B43DIR" sh "$REPO/test/integration/fetch-upstream.sh" \
	"$KVER" >/dev/null

cd "$WORK"
git init -q .
git config user.name  "b43-ac regen"
git config user.email "regen@localhost"
git add -A
git commit -qm "b43 vanilla $KVER"

# 0003 e 0004 sono il base della 0006 dentro b43/; 0001 tocca solo ssb/bcma.
for n in 0003 0004; do
	git am -q "$REPO"/patches/$n-*.patch
done

# 0006: tutto src/ tranne src/Makefile, che e' il makefile fuori albero e non
# quello del kernel -- le righe del kernel stanno gia' nella patch. Se la 0006
# non applica senza -3, il base e' cambiato e va capito perche' prima di
# riemettere: un merge a tre vie qui rebaserebbe la serie in silenzio.
git am -q "$REPO"/patches/0006-*.patch
for f in "$REPO"/src/*.c "$REPO"/src/*.h; do
	cp "$f" "$WORK/$B43DIR/"
done
git add -A "$B43DIR"
git commit -q --amend --no-edit

git format-patch -q -1 -o "$WORK/out"
cp "$WORK"/out/0001-*.patch \
   "$REPO"/patches/0006-b43-AC-PHY-bring-up-from-the-reverse-engineered-port.patch
echo "regen-patches: 0006 rigenerata su vanilla $KVER + 0003 + 0004"
