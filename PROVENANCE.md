# Provenance of the stock-driver binaries and third-party captures

## DSL-3580L — `wlDSL-3580_EU.o_save`

From the D-Link GPL source release for the DSL-3580L, firmware v1.01, tarball
`DSL-3580_EU_1.01_05072014_GPL.tar.gz` (Broadcom BCM963xx SDK). Mirror:
https://github.com/aleferri/dsl-3580l-sources (release `gpl-release`).
Reference build of that release: `make PROFILE=DSL-3580_EU`, uClibc crosstools
gcc-4.4.2 toolchain on Ubuntu 10.04.

The mirror's release asset is `DSL-3580_EU_1.00_10232013_GPL.tar.gz`, firmware
v1.00, not the v1.01 tarball above. Inside `bcm963xx_.L._consumer.tar.gz` it
has two `wl` objects: `impl14/` is 6.30.102.3, `impl14_v07/` is 6.30.102.7.
`b43/radio_2069.c`'s channel table is `chan_tuning_2069rev4` of the latter,
sha256 `73b58f069e5ce3f3f8b6a111df4a3e6de57ed22761a53c41ba50c05ab0b07233`,
extracted with `reverse-tools/extract_chan_tuning_2069rev4.py --min-freq 2400`:
the 50 5 GHz rows and the 14 2.4 GHz rows.

The TX descriptor words and the TX core table in `docs/retrace-todo.md`
("HT and VHT") are read from the disassembly (`mips-linux-gnu-objdump -d -r`)
of the object with version string `6.30.102.7.cpe4.12L07.0`, sha256
`a68a032673840c17a5981b0d00f16920ce992bca35ecdfba407bd6d488f28221`.

## D6220 — `wlD6220.o_save`

The `wl` driver object (rev `0x70e590e` ≈ 7.14.89.14) from Netgear's GPL
firmware release for the D6220, firmware V1.0.0.76, archive
`D6220-V1.0.0.76_GPL_Src_full.zip`. It is in the Netgear GPL collection on the
Internet Archive, which mirrors the official download centre
`downloads.netgear.com/files/GDC`:
https://archive.org/download/netgearfirmwaresgpl/D6220-V1.0.0.76_GPL_Src_full.zip

The copy the TX status layout in `wl-diag/README.md` is read from has the
version string `7.14.89.14.cpe4.16L03.0-kdb`, sha256
`2bbba860d1282d9d06a02f8297a150b3e38f1e21895ccb33a031f0bf0469c80b`.

## AGSOT — `wl.ko` from `AGSOT_1_0_8.img`

The `wl` module extracted from the firmware image `AGSOT_1_0_8.img`, a Sercomm
board unrelated to Netgear or D-Link. It is the third blob branch on which the
TX-gain table is checked. Its `acphy_txgain_epa_5g_2069rev4` matches the
D6220's (7.14.89) on 128 of 128 entries, while the DSL's 6.30 branch diverges
from index 31 on. Two physically independent boards carrying the same values
rules out per-board calibration; see `b43_acphy_txgain_epa_5g_2069rev4` in
`b43/phy_ac.c`.

**TODO:** origin and mirror of the image, and the `wl` revision it contains.
Unlike the two blobs above, this entry does not say how to obtain the file
again, so the 128/128 check cannot be reproduced from this tree today.

## MacBookAir6,1 capture — `router-data/macbookair6-1/`

Taken by gonsolo, https://github.com/gonsolo/bcm4360-acphy, file
`traces/wl-init-20260926-132021.trace` at commit `2a24fb2`, with his
`wl_full_trace.bt`. The other captures, the register dumps, the SROM and the
two scripts were sent by him to this project on 2026-10-01.

That repository also holds material derived from decompiling `wl.ko`, which
this project does not read. What it does take from there is what his port's
driver code and notes state, at commit `c27dcd8`, as measured on his card
under the 832.127 microcode: the 4-byte TX offload header in front of the
AC TX descriptor and the IV offset in it (`b43-src/xmit.{c,h}`), that bits
23:16 of the third TX status word are not attempts on a first-rate-only
descriptor (`notes/110`), and the per-core receive power in bytes 9 and 10
of the RX header (`b43-src/xmit.c`); see `docs/retrace-todo.md`.

**TODO:** the repository has no licence file; permission to redistribute the
capture is pending.

## Broadcom `d11.h` — TX status field names

`include/d11.h` of the `bcmdhd.101.10.361.x` tree in Google's Nest open-source
manifest:
https://nest-open-source.googlesource.com/manifest_repos/dhd-driver/+/refs/heads/main/bcmdhd.101.10.361.x/include/d11.h
Read for the `TX_STATUS40_*` definitions of the corerev 40-79 TX status
and for the layout and field names of `d11actxh_t`, the corerev 40-63 TX
descriptor `struct b43_txhdr_ac` follows; no code or text of it is in this
tree. Every field `b43_txstatus_read_ac()` decodes is checked against the
archer-t5e and MacBookAir6,1 captures (`docs/retrace-todo.md`, "TX status");
the descriptor is in DMA memory, which no capture records. The file is published in that public
repository but its header carries Broadcom's proprietary notice.
