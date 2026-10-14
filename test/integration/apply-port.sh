#!/bin/sh
# I sorgenti del repo sopra b43 mainline, dentro b43-upstream/.
#
# Serve perche' il phy_ac.c di mainline e' un segnaposto di 88 righe: una
# vtable minima che non fa il bring-up. Senza b43/ la suite misura quella,
# non il nostro driver, e la probe esce con -EOPNOTSUPP prima di emettere una
# sola op.
#
# b43/ tiene i file interi, quindi qui non si applica niente: si copiano
# sopra quelli vanilla. Di bcma/ servono i due header ssb, con i campi della
# SROM rev 11 senza cui phy_ac.c non compila e gpio_ext[] senza cui non
# compila leds.c, l'header della ChipCommon per i bit di chipstatus del 4360,
# e i file della gestione del core che il port cambia; il resto e' codice del
# bus, che qui e' finto (bcma_stub.c).
#
# Uso: apply-port.sh
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DIR=$HERE/b43-upstream
REPO=$HERE/../..

test -f "$DIR/main.c" || { echo "prima: make fetch" >&2; exit 1; }

cp "$REPO"/b43/* "$DIR/"
for f in core.c driver_chipcommon.c driver_chipcommon_pmu.c driver_pcie2.c; do
	cp "$REPO/bcma/drivers/bcma/$f" "$DIR/bcma/"
done
mkdir -p "$HERE/kinc/linux/ssb"
cp "$REPO"/bcma/include/linux/ssb/ssb.h "$REPO"/bcma/include/linux/ssb/ssb_regs.h \
    "$HERE/kinc/linux/ssb/"
mkdir -p "$HERE/kinc/linux/bcma"
cp "$REPO"/bcma/include/linux/bcma/bcma_driver_chipcommon.h "$HERE/kinc/linux/bcma/"
echo "b43/ copiato su b43-upstream/ ($(ls "$REPO"/b43 | wc -l) file), header ssb e ChipCommon da bcma/"
