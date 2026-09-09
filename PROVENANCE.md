# BLOBS PROVENANCE

## DSL-3580L — `wlDSL-3580_EU.o_save`

GPL source release D-Link per il DSL-3580L, firmware v1.01, tarball
`DSL-3580_EU_1.01_05072014_GPL.tar.gz` (SDK Broadcom BCM963xx). Mirror:
https://github.com/aleferri/dsl-3580l-sources (release `gpl-release`).
Build di riferimento del release: `make PROFILE=DSL-3580_EU`, toolchain
uClibc crosstools gcc-4.4.2 su Ubuntu 10.04.

## D6220 — `wlD6220.o_save`

Object del driver `wl` (rev `0x70e590e` ≈ 7.14.89.14) estratto dal GPL
firmware release Netgear per il D6220, firmware V1.0.0.76, archivio
`D6220-V1.0.0.76_GPL_Src_full.zip` nella collezione GPL Netgear su Internet
Archive (mirror del download center ufficiale `downloads.netgear.com/files/GDC`):
https://archive.org/download/netgearfirmwaresgpl/D6220-V1.0.0.76_GPL_Src_full.zip

## AGSOT — `wl.ko` da `AGSOT_1_0_8.img`

Modulo `wl` estratto dall'immagine firmware `AGSOT_1_0_8.img`, board Sercomm,
senza rapporto con Netgear né con D-Link. È il terzo ramo di blob su cui è
verificata la tabella TX-gain: il simbolo `acphy_txgain_epa_5g_2069rev4`
combacia 128 voci su 128 con quello del D6220 (7.14.89), mentre il ramo 6.30
del DSL diverge dall'indice 31 in poi. Due board fisicamente indipendenti che
portano lo stesso valore è ciò che esclude la calibrazione per-board — vedi
`b43_acphy_txgain_epa_5g_2069rev4` in `src/phy_ac.c`.

TODO: origine e mirror dell'immagine, e la revisione del driver `wl` che
contiene. A differenza dei due blob sopra, questa voce non porta un modo di
riottenere il file, quindi la verifica 128/128 non è oggi riproducibile da
questo albero.
