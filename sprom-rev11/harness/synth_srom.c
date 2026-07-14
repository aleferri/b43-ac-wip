/*
 * synth_srom.c — encoder counterpart of extract_r11.c.
 *
 * For each NVRAM key the parser reads, write the corresponding bytes
 * to the SROM buffer at the SAME byte offset, using the SAME bit-packing
 * the parser decodes. This is by construction symmetric with
 * extract_r11.c; the round-trip's value lies in:
 *
 *   - Structural coverage: synth and parse together exhaustively map
 *     the NVRAM key set onto the SROM byte footprint. A missing key
 *     here flags a parser field that has no NVRAM source.
 *   - Encoding self-consistency: triplet packs/unpacks survive
 *     reflection (catches off-by-one bit shifts).
 *   - Endianness / array-stride correctness: pa2ga/pa5ga and maxp5ga
 *     storage layouts are exercised by every per-chain block.
 *
 * What this does NOT do is cross-check offsets against an external
 * source. If the patch's SSB_SPROM11_* values are wrong (e.g.
 * IL0MAC=0x90 misplaced relative to canonical Broadcom layout), this
 * harness still passes — the synth and parse agree by construction.
 * For external offset cross-validation, see cross_check.md.
 *
 * Style: deliberately mirrors extract_r11.c's hand-written, bcma-style
 * field-per-field code path. The patch series is committing to the
 * hand-written populator convention; introducing a table-driven
 * intermediate here would diverge from what is upstreamed.
 */

#include "synth_srom.h"
#include "ssb_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* SPOFF: byte offset → word index. Patch's macro lives in kernel_shim.h. */
#include "kernel_shim.h"

/* === Low-level u8/u16/u32 placement at byte offsets ====================== */

static inline void write_u16_at_byte(u16 *srom, u16 byte_off, u16 val)
{
	/* All rev-11 fields the patch addresses are word-aligned. */
	srom[byte_off >> 1] = val;
}

static inline void write_u8_at_byte(u16 *srom, u16 byte_off, u8 val)
{
	u16 w = srom[byte_off >> 1];
	if (byte_off & 1)
		w = (w & 0x00ff) | ((u16)val << 8);
	else
		w = (w & 0xff00) | val;
	srom[byte_off >> 1] = w;
}

/* Write a u32 packed as two consecutive u16 words at byte_off (low word
 * first, matching how SPEX32() reconstructs `(hi<<16)|lo` from the
 * parser's POV). Used for the mcsbw20/40/80/160 region. */
static inline void write_u32_at_byte(u16 *srom, u16 byte_off, u32 val)
{
	srom[byte_off >> 1]       = val & 0xffff;
	srom[(byte_off >> 1) + 1] = (val >> 16) & 0xffff;
}

/* === NVRAM helpers ======================================================= */

static int nv_u32(const struct nvram *nv, const char *key, u32 *out)
{
	const char *s = nvram_get(nv, key);
	if (!s)
		return -1;
	char *end;
	unsigned long v = strtoul(s, &end, 0);
	if (*end != 0)
		return -1;
	*out = (u32)v;
	return 0;
}

static int nv_u16(const struct nvram *nv, const char *key, u16 *out)
{
	u32 v;
	if (nv_u32(nv, key, &v) < 0)
		return -1;
	*out = (u16)v;
	return 0;
}

static int nv_u8(const struct nvram *nv, const char *key, u8 *out)
{
	u32 v;
	if (nv_u32(nv, key, &v) < 0)
		return -1;
	*out = (u8)v;
	return 0;
}

/* Parse "0xNN,0xNN,..." into u16[count]. Returns count parsed (≤ count_max),
 * or 0 if the key is absent. Negative on parse error. */
static int nv_u16_list(const struct nvram *nv, const char *key,
		       u16 *out, int count_max)
{
	const char *s = nvram_get(nv, key);
	if (!s)
		return 0;
	int n = 0;
	while (*s && n < count_max) {
		while (*s == ' ' || *s == '\t')
			s++;
		char *end;
		unsigned long v = strtoul(s, &end, 0);
		if (end == s)
			return -1;
		out[n++] = (u16)v;
		s = end;
		if (*s == ',')
			s++;
	}
	return n;
}

/* Parse "DD,DD,DD,DD" or "0xNN,..." into u8[count]. Same conventions. */
static int nv_u8_list(const struct nvram *nv, const char *key,
		      u8 *out, int count_max)
{
	const char *s = nvram_get(nv, key);
	if (!s)
		return 0;
	int n = 0;
	while (*s && n < count_max) {
		while (*s == ' ' || *s == '\t')
			s++;
		char *end;
		unsigned long v = strtoul(s, &end, 0);
		if (end == s)
			return -1;
		out[n++] = (u8)v;
		s = end;
		if (*s == ',')
			s++;
	}
	return n;
}

static int parse_mac(const char *s, u8 mac[6])
{
	unsigned int a[6];
	if (!s)
		return -1;
	if (sscanf(s, "%x:%x:%x:%x:%x:%x",
		   &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++) {
		if (a[i] > 0xff)
			return -1;
		mac[i] = (u8)a[i];
	}
	return 0;
}

/* === rxgains pack: inverse of bcma_sprom_unpack_rxgains() ================ */

static u8 pack_rxgains(u8 elnagain, u8 triso, u8 trelnabyp)
{
	return ((trelnabyp & 0x01) << 7) |
	       ((triso     & 0x0f) << 3) |
	        (elnagain  & 0x07);
}

/* Read the (elnagain, triso, trelnabyp) triplet for chain `c` band `b`
 * (b is the NVRAM key segment: "2g", "5g", "5gm", "5gh") and pack into
 * a u8. Triplet absent → 0. */
static u8 nv_rxgains_byte(const struct nvram *nv, const char *band, int c)
{
	char k[32];
	u8 e = 0, t = 0, b = 0;

	snprintf(k, sizeof(k), "rxgains%selnagaina%d", band, c);
	(void)nv_u8(nv, k, &e);
	snprintf(k, sizeof(k), "rxgains%strisoa%d", band, c);
	(void)nv_u8(nv, k, &t);
	snprintf(k, sizeof(k), "rxgains%strelnabypa%d", band, c);
	(void)nv_u8(nv, k, &b);
	return pack_rxgains(e, t, b);
}

/* === Synth ============================================================== */

int synth_srom_from_nvram(const struct nvram *nv, u16 *srom, size_t words)
{
	/* Footprint: highest byte the patch reads is mcsbw1605ghpo at 0x18C
	 * occupying 4 bytes → last touched word is byte 0x18E = word 0xC7
	 * (200 dec). Anything below that is fine for our footprint. */
	if (words < 0x100)
		return -1;

	/* --- Header ------------------------------------------------------ */
	{
		u8 mac[6];
		const char *s = nvram_get(nv, "macaddr");
		if (s && parse_mac(s, mac) == 0) {
			/* il0mac stored as 3 big-endian u16 starting at IL0MAC,
			 * mirroring extract_r11.c's cpu_to_be16 round-trip. */
			for (int i = 0; i < 3; i++) {
				u16 w = ((u16)mac[2*i] << 8) | mac[2*i + 1];
				write_u16_at_byte(srom, SSB_SPROM11_IL0MAC + 2*i, w);
			}
		}
	}

	{ u16 v = 0; if (nv_u16(nv, "boardrev",  &v) == 0)
		write_u16_at_byte(srom, SSB_SPROM8_BOARDREV, v); }
	{ u16 v = 0; if (nv_u16(nv, "boardtype", &v) == 0)
		write_u16_at_byte(srom, SSB_SPROM1_SPID, v); }
	{ u16 v = 0; if (nv_u16(nv, "ccode",     &v) == 0)
		write_u16_at_byte(srom, SSB_SPROM11_CCODE, v); }

	/* ANTAVAIL packs aa5g (high byte) | aa2g (low byte) at 0xA0. */
	{
		u8 aa2g = 0, aa5g = 0;
		(void)nv_u8(nv, "aa2g", &aa2g);
		(void)nv_u8(nv, "aa5g", &aa5g);
		write_u16_at_byte(srom, SSB_SPROM11_ANTAVAIL,
				  ((u16)aa5g << SSB_SPROM8_ANTAVAIL_A_SHIFT) |
				  (aa2g & SSB_SPROM8_ANTAVAIL_BG));
	}

	/* TXRXC packs antswitch (hi byte) | rxchain<<4 | txchain at 0xA8. */
	{
		u8 tx = 0, rx = 0, sw = 0;
		(void)nv_u8(nv, "txchain",   &tx);
		(void)nv_u8(nv, "rxchain",   &rx);
		(void)nv_u8(nv, "antswitch", &sw);
		write_u16_at_byte(srom, SSB_SPROM11_TXRXC,
				  ((u16)sw << SSB_SPROM8_TXRXC_SWITCH_SHIFT) |
				  ((rx & 0x0f) << SSB_SPROM8_TXRXC_RXCHAIN_SHIFT) |
				  (tx & 0x0f));
	}

	/* subband5gver: low byte at 0xD6. */
	{ u8 v = 0; if (nv_u8(nv, "subband5gver", &v) == 0)
		write_u8_at_byte(srom, SSB_SPROM11_SUBBAND5GVER, v); }

	/* --- Per-chain power info blocks (stride 0x28) ------------------- */
	static const u16 pwr_off[] = {
		SSB_SPROM11_PWR_INFO_CORE0,
		SSB_SPROM11_PWR_INFO_CORE1,
		SSB_SPROM11_PWR_INFO_CORE2,
	};
	for (int c = 0; c < 3; c++) {
		char k[32];
		u16 base = pwr_off[c];

		/* maxp2ga: low byte at base+0x00. */
		snprintf(k, sizeof(k), "maxp2ga%d", c);
		{ u8 v = 0; if (nv_u8(nv, k, &v) == 0)
			write_u8_at_byte(srom, base + SSB_SPROM11_PWR_MAXP2GA, v); }

		/* pa2ga: 3 u16 starting at base+0x02. */
		snprintf(k, sizeof(k), "pa2ga%d", c);
		{
			u16 v[3] = {0};
			(void)nv_u16_list(nv, k, v, 3);
			for (int j = 0; j < 3; j++)
				write_u16_at_byte(srom,
						  base + SSB_SPROM11_PWR_PA2GA + 2*j,
						  v[j]);
		}

		/* maxp5ga[4] packed le into 2 words at base+0x0C. */
		snprintf(k, sizeof(k), "maxp5ga%d", c);
		{
			u8 m[4] = {0};
			(void)nv_u8_list(nv, k, m, 4);
			write_u16_at_byte(srom,
					  base + SSB_SPROM11_PWR_MAXP5GA,
					  ((u16)m[1] << 8) | m[0]);
			write_u16_at_byte(srom,
					  base + SSB_SPROM11_PWR_MAXP5GA + 2,
					  ((u16)m[3] << 8) | m[2]);
		}

		/* pa5ga: 12 u16 starting at base+0x10. */
		snprintf(k, sizeof(k), "pa5ga%d", c);
		{
			u16 v[12] = {0};
			(void)nv_u16_list(nv, k, v, 12);
			for (int j = 0; j < 12; j++)
				write_u16_at_byte(srom,
						  base + SSB_SPROM11_PWR_PA5GA + 2*j,
						  v[j]);
		}

		/* rxgains: 4 sub-band bytes, two u16 at base+0x08/0x0A.
		 * Byte-to-sub-band assignment from extract_r11.c:
		 *   RXGAINS0 lo = 5gm, hi = 5gh
		 *   RXGAINS1 lo = 2g,  hi = 5gl  (NVRAM key segment "5g") */
		{
			u8 b5gm = nv_rxgains_byte(nv, "5gm", c);
			u8 b5gh = nv_rxgains_byte(nv, "5gh", c);
			u8 b2g  = nv_rxgains_byte(nv, "2g",  c);
			u8 b5gl = nv_rxgains_byte(nv, "5g",  c);
			write_u16_at_byte(srom, base + SSB_SPROM11_PWR_RXGAINS0,
					  ((u16)b5gh << 8) | b5gm);
			write_u16_at_byte(srom, base + SSB_SPROM11_PWR_RXGAINS1,
					  ((u16)b5gl << 8) | b2g);
		}
	}

	/* --- pdoffset40ma triplet --------------------------------------- */
	for (int i = 0; i < 3; i++) {
		char k[32];
		u16 v = 0;
		snprintf(k, sizeof(k), "pdoffset40ma%d", i);
		if (nv_u16(nv, k, &v) == 0)
			write_u16_at_byte(srom,
					  SSB_SPROM11_PDOFFSET40MA + 2*i, v);
	}

	/* --- Power-per-rate region 0x150..0x190 ------------------------- */
	struct { const char *key; u16 off; int is_u32; } ppr[] = {
		{"cckbw202gpo",            SSB_SPROM11_CCKBW202GPO,           0},
		{"cckbw20ul2gpo",          SSB_SPROM11_CCKBW20UL2GPO,         0},
		{"mcsbw202gpo",            SSB_SPROM11_MCSBW202GPO,           1},
		{"mcsbw402gpo",            SSB_SPROM11_MCSBW402GPO,           1},
		{"dot11agofdmhrbw202gpo",  SSB_SPROM11_DOT11AGOFDMHRBW202GPO, 0},
		{"ofdmlrbw202gpo",         SSB_SPROM11_OFDMLRBW202GPO,        0},
		{"mcsbw205glpo",           SSB_SPROM11_MCSBW205GLPO,          1},
		{"mcsbw405glpo",           SSB_SPROM11_MCSBW405GLPO,          1},
		{"mcsbw805glpo",           SSB_SPROM11_MCSBW805GLPO,          1},
		{"mcsbw1605glpo",          SSB_SPROM11_MCSBW1605GLPO,         1},
		{"mcsbw205gmpo",           SSB_SPROM11_MCSBW205GMPO,          1},
		{"mcsbw405gmpo",           SSB_SPROM11_MCSBW405GMPO,          1},
		{"mcsbw805gmpo",           SSB_SPROM11_MCSBW805GMPO,          1},
		{"mcsbw1605gmpo",          SSB_SPROM11_MCSBW1605GMPO,         1},
		{"mcsbw205ghpo",           SSB_SPROM11_MCSBW205GHPO,          1},
		{"mcsbw405ghpo",           SSB_SPROM11_MCSBW405GHPO,          1},
		{"mcsbw805ghpo",           SSB_SPROM11_MCSBW805GHPO,          1},
		{"mcsbw1605ghpo",          SSB_SPROM11_MCSBW1605GHPO,         1},
	};
	for (size_t i = 0; i < sizeof(ppr)/sizeof(ppr[0]); i++) {
		u32 v = 0;
		if (nv_u32(nv, ppr[i].key, &v) != 0)
			continue;
		if (ppr[i].is_u32)
			write_u32_at_byte(srom, ppr[i].off, v);
		else
			write_u16_at_byte(srom, ppr[i].off, (u16)v);
	}

	return 0;
}
