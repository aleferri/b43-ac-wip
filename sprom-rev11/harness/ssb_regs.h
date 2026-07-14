/*
 * ssb_regs.h — SROM byte offsets used by bcma_sprom_extract_r11().
 *
 * Two groups:
 *
 *  (a) The SSB_SPROM8_* offsets reused by the rev-11 extractor for
 *      the shared header. These values are taken VERBATIM from
 *      include/linux/ssb/ssb_regs.h upstream (verified via sparse
 *      checkout of torvalds/linux at master).
 *
 *  (b) The SSB_SPROM11_* offsets introduced by the patch. These are
 *      identical to the values in the patch's diff against
 *      include/linux/ssb/ssb_regs.h.
 *
 * If either source changes upstream/in the patch, this file must be
 * updated to match.
 */

#ifndef SSB_REGS_H
#define SSB_REGS_H

/* --- (a) Pre-existing rev-8 offsets reused by extract_r11 -----
 * Verified verbatim from include/linux/ssb/ssb_regs.h master. */

#define SSB_SPROM_REVISION		0x007E
#define SSB_SPROM_REVISION_REV		0x00FF

#define SSB_SPROM1_SPID			0x0004

#define SSB_SPROM8_BOARDREV		0x0082
#define SSB_SPROM8_IL0MAC		0x008C
#define SSB_SPROM8_CCODE		0x0092
#define SSB_SPROM8_ANTAVAIL		0x009C
#define SSB_SPROM8_ANTAVAIL_A		0xFF00
#define SSB_SPROM8_ANTAVAIL_A_SHIFT	8
#define SSB_SPROM8_ANTAVAIL_BG		0x00FF
#define SSB_SPROM8_ANTAVAIL_BG_SHIFT	0
#define SSB_SPROM8_TXRXC		0x00A2
#define SSB_SPROM8_TXRXC_TXCHAIN	0x000F
#define SSB_SPROM8_TXRXC_TXCHAIN_SHIFT	0
#define SSB_SPROM8_TXRXC_RXCHAIN	0x00F0
#define SSB_SPROM8_TXRXC_RXCHAIN_SHIFT	4
#define SSB_SPROM8_TXRXC_SWITCH		0xFF00
#define SSB_SPROM8_TXRXC_SWITCH_SHIFT	8

/* --- (b) New rev-11 offsets, verbatim from the patch -----------
 *
 * Plus three corrections introduced by this harness's test result
 * (see the note at the top of extract_r11.c): IL0MAC, ANTAVAIL and
 * TXRXC are NOT at their rev-8 byte offsets on a rev-11 SROM. The
 * patch's claim that header fields share their offsets with rev 8
 * is falsified by the DSL-3580L test vector. Their value-matched
 * rev-11 positions are defined here.
 */

#define SSB_SPROM11_IL0MAC		0x0090
#define SSB_SPROM11_ANTAVAIL		0x00A0
#define SSB_SPROM11_TXRXC		0x00A8

/* rev-11 has dedicated CCODE/REGREV offsets distinct from the rev-8
 * reuse, per Broadcom bcmsrom_tbl.h: SROM11_CCODE = word 75, SROM11_
 * REGREV = word 76. Reusing SSB_SPROM8_CCODE = 0x92 collides with the
 * MACMID word (the middle u16 of IL0MAC at 0x90+2). */
#define SSB_SPROM11_CCODE		0x0096	/* word 75 */
#define SSB_SPROM11_REGREV		0x0098	/* word 76, mask 0x00ff */

#define SSB_SPROM11_SUBBAND5GVER	0x00D6

#define SSB_SPROM11_PDOFFSET40MA	0x00CA
/*
 * pdoffset80ma triplet, byte 0xD0 = word 104..106 (SPOFF divides by 2, so
 * 0xCA->word 101: the 40ma triplet is words 101..103). Offset pinned by the
 * neighbour boundary: word 107 (byte 0xD6) is subband5gver, leaving exactly
 * the three words 104..106 for 80ma. 5 GHz only — no 2.4 GHz 80 MHz pdoffset.
 * Value reads 0 on every dump in router-data/ (DSL-3580L, D6220 byte-identical
 * here; agcombo blank), so the offset is confirmed by layout, not by value.
 */
#define SSB_SPROM11_PDOFFSET80MA	0x00D0

#define SSB_SPROM11_PWR_INFO_CORE0	0x00D8
#define SSB_SPROM11_PWR_INFO_CORE1	0x0100
#define SSB_SPROM11_PWR_INFO_CORE2	0x0128

#define SSB_SPROM11_PWR_MAXP2GA		0x0000
#define SSB_SPROM11_PWR_PA2GA		0x0002
#define SSB_SPROM11_PWR_RXGAINS0	0x0008
#define SSB_SPROM11_PWR_RXGAINS1	0x000A
#define SSB_SPROM11_PWR_MAXP5GA		0x000C
#define SSB_SPROM11_PWR_PA5GA		0x0010

#define SSB_SPROM11_CCKBW202GPO		0x0150
#define SSB_SPROM11_CCKBW20UL2GPO	0x0152
#define SSB_SPROM11_MCSBW202GPO		0x0154
#define SSB_SPROM11_MCSBW402GPO		0x0158
#define SSB_SPROM11_DOT11AGOFDMHRBW202GPO 0x015C
#define SSB_SPROM11_OFDMLRBW202GPO	0x015E
#define SSB_SPROM11_MCSBW205GLPO	0x0160
#define SSB_SPROM11_MCSBW405GLPO	0x0164
#define SSB_SPROM11_MCSBW805GLPO	0x0168
#define SSB_SPROM11_MCSBW1605GLPO	0x016C
#define SSB_SPROM11_MCSBW205GMPO	0x0170
#define SSB_SPROM11_MCSBW405GMPO	0x0174
#define SSB_SPROM11_MCSBW805GMPO	0x0178
#define SSB_SPROM11_MCSBW1605GMPO	0x017C
#define SSB_SPROM11_MCSBW205GHPO	0x0180
#define SSB_SPROM11_MCSBW405GHPO	0x0184
#define SSB_SPROM11_MCSBW805GHPO	0x0188
#define SSB_SPROM11_MCSBW1605GHPO	0x018C

#endif /* SSB_REGS_H */
