#!/bin/sh
# Rigenera patches/0005 e patches/0006 dai sorgenti in src/.
#
# src/ e' la fonte di verita'; questo script materializza la procedura
# descritta in docs/driver-status.md: applica la serie su uno sparse
# checkout del kernel, sovrascrive i file coperti da src/, e riemette
# le due patch con git format-patch. Messaggi e author sono presi dalle
# patch correnti (via git am), quindi per cambiarli basta editare la
# patch e rilanciare.
#
# Uso:    scripts/regen-patches.sh
# Env:    KDIR    dir del checkout kernel (default /tmp/linux-b43-regen,
#                 riusata se gia' presente)
#         KREPO   remote del kernel (default torvalds/linux su GitHub)
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KDIR=${KDIR:-/tmp/linux-b43-regen}
KREPO=${KREPO:-https://github.com/torvalds/linux.git}
B43DIR=drivers/net/wireless/broadcom/b43
NPATCH=$(ls "$REPO"/patches/00*.patch | wc -l)

if [ ! -d "$KDIR/.git" ]; then
	git clone --depth=1 --filter=blob:none --sparse "$KREPO" "$KDIR"
	git -C "$KDIR" sparse-checkout set "$B43DIR"
fi
git -C "$KDIR" config user.name  >/dev/null 2>&1 || \
	git -C "$KDIR" config user.name  "b43-ac regen"
git -C "$KDIR" config user.email >/dev/null 2>&1 || \
	git -C "$KDIR" config user.email "regen@localhost"
git -C "$KDIR" am --abort 2>/dev/null || true
git -C "$KDIR" reset --hard -q origin/HEAD 2>/dev/null || \
	git -C "$KDIR" reset --hard -q origin/master
git -C "$KDIR" clean -fdq "$B43DIR"

# 0002-0004: base della serie dentro b43/ (0001 tocca solo ssb/bcma).
for n in 2 3 4; do
	git -C "$KDIR" am -3 -q "$REPO"/patches/000$n-*.patch
done

# 0005: radio 2069. Contenuto autoritativo: src/radio_2069.{c,h} + la
# sola riga Makefile radio_2069.o (rxiqcal/helpers appartengono alla 0006).
git -C "$KDIR" am -3 -q "$REPO"/patches/0005-*.patch
cp "$REPO"/src/radio_2069.c "$REPO"/src/radio_2069.h "$KDIR/$B43DIR/"
sed -i '/rxiqcal_phy_ac\.o/d;/helpers_phy_ac\.o/d' "$KDIR/$B43DIR/Makefile"
git -C "$KDIR" add -A "$B43DIR"
git -C "$KDIR" commit -q --amend --no-edit

# 0006: PHY ops. Contenuto autoritativo: src/phy_ac.{c,h},
# helpers_phy_ac.c, rxiqcal_phy_ac.{c,h} + le loro righe Makefile.
git -C "$KDIR" am -3 -q "$REPO"/patches/0006-*.patch
cp "$REPO"/src/phy_ac.c "$REPO"/src/phy_ac.h "$REPO"/src/helpers_phy_ac.c \
   "$REPO"/src/rxiqcal_phy_ac.c "$REPO"/src/rxiqcal_phy_ac.h "$KDIR/$B43DIR/"
grep -q 'rxiqcal_phy_ac\.o' "$KDIR/$B43DIR/Makefile" || \
	sed -i '/+= radio_2069\.o/a b43-$(CONFIG_B43_PHY_AC)\t+= rxiqcal_phy_ac.o\nb43-$(CONFIG_B43_PHY_AC)\t+= helpers_phy_ac.o' \
	    "$KDIR/$B43DIR/Makefile"
git -C "$KDIR" add -A "$B43DIR"
git -C "$KDIR" commit -q --amend --no-edit

# Riemissione con numerazione N/serie e verifica che 0008 applichi ancora.
OUT=$(mktemp -d)
git -C "$KDIR" format-patch -q --start-number=5 -2 -o "$OUT"
sed -i "s|^Subject: \[PATCH \([56]\)/[0-9]*\]|Subject: [PATCH \1/$NPATCH]|" "$OUT"/000*.patch
git -C "$KDIR" apply --3way "$REPO"/patches/0008-*.patch
git -C "$KDIR" reset --hard -q HEAD && git -C "$KDIR" clean -fdq "$B43DIR"
cp "$OUT"/0005-*.patch "$OUT"/0006-*.patch "$REPO"/patches/
rm -rf "$OUT"
echo "regen-patches: 0005 e 0006 rigenerate in $REPO/patches/"
