# Provenance of the stock-driver binaries

## DSL-3580L — `wlDSL-3580_EU.o_save`

From the D-Link GPL source release for the DSL-3580L, firmware v1.01, tarball
`DSL-3580_EU_1.01_05072014_GPL.tar.gz` (Broadcom BCM963xx SDK). Mirror:
https://github.com/aleferri/dsl-3580l-sources (release `gpl-release`).
Reference build of that release: `make PROFILE=DSL-3580_EU`, uClibc crosstools
gcc-4.4.2 toolchain on Ubuntu 10.04.

## D6220 — `wlD6220.o_save`

The `wl` driver object (rev `0x70e590e` ≈ 7.14.89.14) from Netgear's GPL
firmware release for the D6220, firmware V1.0.0.76, archive
`D6220-V1.0.0.76_GPL_Src_full.zip`. It is in the Netgear GPL collection on the
Internet Archive, which mirrors the official download centre
`downloads.netgear.com/files/GDC`:
https://archive.org/download/netgearfirmwaresgpl/D6220-V1.0.0.76_GPL_Src_full.zip

## AGSOT — `wl.ko` from `AGSOT_1_0_8.img`

The `wl` module extracted from the firmware image `AGSOT_1_0_8.img`, a Sercomm
board unrelated to Netgear or D-Link. It is the third blob branch on which the
TX-gain table is checked. Its `acphy_txgain_epa_5g_2069rev4` matches the
D6220's (7.14.89) on 128 of 128 entries, while the DSL's 6.30 branch diverges
from index 31 on. Two physically independent boards carrying the same values
rules out per-board calibration; see `b43_acphy_txgain_epa_5g_2069rev4` in
`src/phy_ac.c`.

**TODO:** origin and mirror of the image, and the `wl` revision it contains.
Unlike the two blobs above, this entry does not say how to obtain the file
again, so the 128/128 check cannot be reproduced from this tree today.
