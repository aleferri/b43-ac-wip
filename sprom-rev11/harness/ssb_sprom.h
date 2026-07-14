/*
 * ssb_sprom.h — minimal mock of struct ssb_sprom and struct bcma_bus
 * sufficient to compile bcma_sprom_extract_r11() in userspace.
 *
 * Only the fields actually referenced by extract_r11 (and the rev-8
 * shared-header fields it reuses) are declared. Field names, types
 * and array sizes match the rev-11-extended struct ssb_sprom from
 * the patch (include/linux/ssb/ssb.h). The struct order is not
 * meaningful for the parser (no offset arithmetic on struct
 * members); we keep declaration order similar for review readability.
 *
 * Intentionally NOT included: the rev <= 10 fields the parser does
 * not touch. This keeps the harness focused on what the patch
 * actually exercises.
 */

#ifndef SSB_SPROM_H
#define SSB_SPROM_H

#include "kernel_shim.h"

/* Per-band rxgains triplet, one entry per RF chain.
 * Verbatim from the patch (include/linux/ssb/ssb.h). */
struct ssb_sprom_rxgains {
	u8 elnagain[3];
	u8 triso[3];
	u8 trelnabyp[3];
};

/* Per-chain power info — only the rev-11-shaped fields the extractor
 * fills are kept. Verbatim names from the patch. */
struct ssb_sprom_core_pwr_info {
	u8 maxp2ga;
	u8 maxp5ga[4];
	u16 pa2ga[3];
	u16 pa5ga[12];
};

struct ssb_sprom {
	/* --- Header (shared rev-8 layout, used by extract_r11) ------- */
	u8 il0mac[6];
	u16 board_rev;
	u16 board_type;
	u16 country_code;
	u8 ant_available_a;
	u8 ant_available_bg;
	u8 txchain;
	u8 rxchain;
	u8 antswitch;

	/* --- rev-11 additions -------------------------------------- */
	u8 subband5gver;

	struct ssb_sprom_rxgains rxgains_2g;
	struct ssb_sprom_rxgains rxgains_5gl;
	struct ssb_sprom_rxgains rxgains_5gm;
	struct ssb_sprom_rxgains rxgains_5gh;

	u16 pdoffset40ma[3];
	u16 pdoffset80ma[3];	/* 5 GHz 80 MHz; 0xD0..0xD4 (words 104..106) */

	u16 cckbw202gpo;
	u16 cckbw20ul2gpo;
	u32 mcsbw202gpo;
	u32 mcsbw402gpo;
	u16 dot11agofdmhrbw202gpo;
	u16 ofdmlrbw202gpo;
	u32 mcsbw205glpo;
	u32 mcsbw405glpo;
	u32 mcsbw805glpo;
	u32 mcsbw1605glpo;
	u32 mcsbw205gmpo;
	u32 mcsbw405gmpo;
	u32 mcsbw805gmpo;
	u32 mcsbw1605gmpo;
	u32 mcsbw205ghpo;
	u32 mcsbw405ghpo;
	u32 mcsbw805ghpo;
	u32 mcsbw1605ghpo;

	struct ssb_sprom_core_pwr_info core_pwr_info[4];
};

/* The kernel's struct bcma_bus is large; the parser only reaches
 * bus->sprom. A single embedded sprom instance is all we need. */
struct bcma_bus {
	struct ssb_sprom sprom;
};

#endif /* SSB_SPROM_H */
