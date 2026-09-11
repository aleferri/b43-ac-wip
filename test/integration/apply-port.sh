#!/bin/sh
# Le patch di questo repo sopra b43 mainline, dentro b43-upstream/.
#
# Serve perche' il phy_ac.c di mainline e' un segnaposto di 88 righe: una
# vtable minima che non fa il bring-up. Senza queste patch la suite misura
# quella, non il nostro driver, e la probe esce con -EOPNOTSUPP prima di
# emettere una sola op.
#
# Le patch che toccano bcma e ssb si saltano: qui il bus e' finto
# (bcma_stub.c), e quelle modifiche non hanno niente da applicare.
#
# Uso: apply-port.sh [versione-header]
# La versione degli header e' quella con cui il Makefile ha fatto il fetch, e
# va passata: ricalcolarla qui da /usr/src prende la prima installata, che con
# piu' versioni non e' detto sia la stessa.
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DIR=$HERE/b43-upstream
PATCHES=$HERE/../../patches
STRIP=drivers/net/wireless/broadcom/b43/
KVER=${1:-$(ls /usr/src/ | grep -oE '^linux-headers-[0-9.]+-[0-9]+$' | head -1 |
       sed 's/linux-headers-//')}

test -f "$DIR/main.c" || { echo "prima: make fetch" >&2; exit 1; }

# I due header ssb, con sopra ogni patch della serie che li tocca: la 0001
# aggiunge i campi della SROM rev11 (pa5ga, maxp5ga, femctrl, subband5gver,
# rxgains) senza cui phy_ac.c non compila, la 0016 gpio_ext[] senza cui non
# compila leds.c. Generati qui, non committati: sono header del kernel
# toppati, non nostro codice.
KH=/usr/src/linux-headers-$KVER/include/linux/ssb
if [ -d "$KH" ]; then
	mkdir -p "$HERE/kinc/linux/ssb"
	cp "$KH/ssb.h" "$KH/ssb_regs.h" "$HERE/kinc/linux/ssb/"
	for p in "$PATCHES"/*.patch; do
		grep -q '^+++ b/include/linux/ssb/' "$p" || continue
		(cd "$HERE/kinc" && patch --batch -p2 -N -r /dev/null \
		    < "$p" >/dev/null 2>&1) || true
	done
	if grep -q antenna_gain_qdb "$HERE/kinc/linux/ssb/ssb.h"; then
		:
	else
		echo "nota: patches/0001 non aggiunge antenna_gain_qdb, che" >&2
		echo "src/phy_ac.c:1528 usa. Vedi README." >&2
	fi
else
	echo "header ssb assenti, kinc/ non generato" >&2
fi

applied=0
skipped=0
failed=
for p in "$PATCHES"/*.patch; do
	# solo le parti che riguardano b43/: -p6 toglie il prefisso completo
	if ! grep -q "^+++ b/$STRIP" "$p"; then
		skipped=$((skipped + 1))
		continue
	fi
	if (cd "$DIR" && patch -p6 -N -r - --no-backup-if-mismatch \
	    < "$p" >/dev/null 2>&1); then
		applied=$((applied + 1))
	else
		failed="$failed $(basename "$p")"
	fi
done
echo "applicate $applied, saltate $skipped (bcma/ssb, non c'e' bus vero qui)"

# Un albero con una patch in meno compila e linka lo stesso, e la suite
# misura un driver che non e' quello del repo. Come per un fetch parziale,
# non passa in silenzio.
if [ -n "$failed" ]; then
	echo "NON applicano:$failed" >&2
	echo "l'albero in $DIR e' incompleto: rm -rf $DIR e make fetch dopo la correzione" >&2
	exit 1
fi
