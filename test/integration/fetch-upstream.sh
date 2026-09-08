#!/bin/sh
# I sorgenti di b43 dal tag che combacia con gli header installati.
#
# La versione conta: b43 da master include linux/unaligned.h, che esiste da
# 6.12, e contro header 6.8 da un errore che non e' del codice.
#
# Un fetch parziale NON passa in silenzio. Prima questo script segnalava il
# file mancante con un echo e continuava, e il make falliva molti passi dopo
# con "rfkill.h: No such file or directory" -- lontano dalla causa, che era un
# curl andato male una volta. Ora si ritenta e, se non basta, si esce.
set -e
KVER=${1:?uso: fetch-upstream.sh <versione-header, es. 6.8.0-139>}
TAG=v$(echo "$KVER" | cut -d. -f1,2)
# La destinazione e' sovrascrivibile: scripts/regen-patches.sh ha bisogno di
# un albero vanilla a parte e non deve toccare quello della suite, che ha le
# patch del port applicate sopra.
DIR=${OUTDIR:-$(dirname "$0")/b43-upstream}
BASE=https://raw.githubusercontent.com/torvalds/linux/$TAG/drivers/net/wireless/broadcom/b43

FILES="main.c phy_common.c bus.c xmit.c phy_ac.c dma.c
       Makefile Kconfig
       b43.h main.h phy_common.h bus.h debugfs.h leds.h rfkill.h pio.h
       dma.h xmit.h lo.h wa.h sdio.h sysfs.h tables.h ppr.h
       phy_a.h phy_g.h phy_n.h phy_lp.h phy_ht.h phy_lcn.h phy_ac.h
       radio_2055.h radio_2056.h radio_2057.h radio_2059.h"

mkdir -p "$DIR"
echo "b43 da $TAG"
missing=
for f in $FILES; do
	for try in 1 2 3; do
		if curl -sfL -o "$DIR/$f" "$BASE/$f"; then
			continue 2
		fi
		sleep 1
	done
	rm -f "$DIR/$f"
	missing="$missing $f"
done

if [ -n "$missing" ]; then
	echo "fetch incompleto, mancano:$missing" >&2
	echo "se sono assenti a monte a $TAG, la lista in questo script e' da" >&2
	echo "aggiornare; se no, e' la rete." >&2
	exit 1
fi
echo "fatto: $(ls "$DIR" | wc -l) file"
