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
set -e
HERE=$(dirname "$0")
DIR=$HERE/b43-upstream
PATCHES=$(cd "$HERE/../../patches" && pwd)
STRIP=drivers/net/wireless/broadcom/b43/

test -f "$DIR/main.c" || { echo "prima: make fetch" >&2; exit 1; }

# I due header ssb di patches/0001: aggiungono i campi della SROM rev11
# (pa5ga, maxp5ga, femctrl, subband5gver, rxgains) che mainline non ha e senza
# cui phy_ac.c non compila. Generati qui, non committati: sono header del
# kernel toppati, non nostro codice.
KVER=$(ls /usr/src/ | grep -oE '^linux-headers-[0-9.]+-[0-9]+$' | head -1 |
       sed 's/linux-headers-//')
KH=/usr/src/linux-headers-$KVER/include/linux/ssb
if [ -d "$KH" ]; then
	mkdir -p "$HERE/kinc/linux/ssb"
	cp "$KH/ssb.h" "$KH/ssb_regs.h" "$HERE/kinc/linux/ssb/"
	# La glob va risolta fuori dalla redirezione: dash non la espande la'.
	srom=$(ls "$PATCHES"/0001-*.patch 2>/dev/null | head -1)
	if [ -n "$srom" ]; then
		(cd "$HERE/kinc" && patch --batch -p2 -N -r /dev/null \
		    < "$srom" >/dev/null 2>&1) || true
	fi
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
