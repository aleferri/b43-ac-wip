/*
 * test.c — drive bcma_sprom_extract_r11() against a real-board test
 * vector and diff every populated field against the corresponding
 * nominal value from `wl nvram_dump`.
 *
 * Two classes of check:
 *
 *  HARD — fields whose SROM bytes are the authoritative source on
 *         this board. Mismatch is a parser bug.
 *
 *  INFO — fields whose NVRAM value is sourced from a separate
 *         CFE/NVRAM store on the reference board, so the SROM bytes
 *         legitimately diverge from `wl nvram_dump`. Currently:
 *           - il0mac:       SROM region zeroed in this dump
 *           - country_code: NVRAM `ccode` empty (regrev=0)
 *
 * To onboard a new test vector (a `wl srdump` + `wl nvram_dump` pair
 * from a different rev-11 board), drop the two files in data/ and
 * pass them on the command line. The HARD checks will run as-is;
 * any new SROM-vs-NVRAM divergences should be added to INFO with a
 * comment explaining why.
 */

#include "data_load.h"
#include "synth_srom.h"
#include "ssb_sprom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

extern void run_extract_r11(struct bcma_bus *bus, const u16 *sprom);

static int g_pass = 0, g_fail = 0, g_info = 0;

static void hard_pass(const char *name)
{
	g_pass++;
	printf("  PASS  %s\n", name);
}

static void hard_fail(const char *name, const char *fmt, ...)
{
	g_fail++;
	printf("  FAIL  %s — ", name);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void info_note(const char *name, const char *fmt, ...)
{
	g_info++;
	printf("  INFO  %s — ", name);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* Parse "0x..." or decimal into u64. Caller asserts range. */
static int parse_u64(const char *s, uint64_t *out)
{
	if (!s || !*s)
		return -1;
	char *end;
	uint64_t v = strtoull(s, &end, 0);
	if (*end != 0)
		return -1;
	*out = v;
	return 0;
}

/* Parse a comma-separated list of 0xNNNN tokens into u16 array.
 * Returns count parsed, or -1 on error. */
static int parse_u16_list(const char *s, u16 *out, int max)
{
	int n = 0;
	while (*s && n < max) {
		while (*s == ',' || *s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		char *end;
		unsigned long v = strtoul(s, &end, 0);
		if (end == s)
			return -1;
		out[n++] = (u16)v;
		s = end;
	}
	return n;
}

/* Same for u8 (decimal common in NVRAM, e.g. "76,76,76,76"). */
static int parse_u8_list(const char *s, u8 *out, int max)
{
	int n = 0;
	while (*s && n < max) {
		while (*s == ',' || *s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		char *end;
		unsigned long v = strtoul(s, &end, 0);
		if (end == s)
			return -1;
		out[n++] = (u8)v;
		s = end;
	}
	return n;
}

static int parse_mac(const char *s, u8 mac[6])
{
	unsigned m[6];
	int n = sscanf(s, "%x:%x:%x:%x:%x:%x",
		       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
	if (n != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		mac[i] = (u8)m[i];
	return 0;
}

/* --- Field checks. Each helper looks up the nvram key, parses, and
 * compares against the parser-populated struct field. -------------- */

static void check_u_hard(const struct nvram *nv, const char *key,
			 uint64_t got, const char *display_name)
{
	const char *raw = nvram_get(nv, key);
	if (!raw) {
		info_note(display_name, "nvram key '%s' missing", key);
		return;
	}
	uint64_t want;
	if (parse_u64(raw, &want) < 0) {
		hard_fail(display_name, "cannot parse nvram '%s' = '%s'", key, raw);
		return;
	}
	if (got == want)
		hard_pass(display_name);
	else
		hard_fail(display_name,
			  "got 0x%" PRIx64 " (%" PRIu64 "), nvram %s = 0x%" PRIx64
			  " (%" PRIu64 ")",
			  got, got, key, want, want);
}

static void check_u16_list_hard(const struct nvram *nv, const char *key,
				const u16 *got, int n, const char *display_name)
{
	const char *raw = nvram_get(nv, key);
	if (!raw) {
		info_note(display_name, "nvram key '%s' missing", key);
		return;
	}
	u16 want[16];
	int got_n = parse_u16_list(raw, want, 16);
	if (got_n != n) {
		hard_fail(display_name, "nvram %s has %d items, expected %d",
			  key, got_n, n);
		return;
	}
	for (int i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			hard_fail(display_name,
				  "[%d]: got 0x%04x, nvram = 0x%04x",
				  i, got[i], want[i]);
			return;
		}
	}
	hard_pass(display_name);
}

static void check_u8_list_hard(const struct nvram *nv, const char *key,
			       const u8 *got, int n, const char *display_name)
{
	const char *raw = nvram_get(nv, key);
	if (!raw) {
		info_note(display_name, "nvram key '%s' missing", key);
		return;
	}
	u8 want[16];
	int got_n = parse_u8_list(raw, want, 16);
	if (got_n != n) {
		hard_fail(display_name, "nvram %s has %d items, expected %d",
			  key, got_n, n);
		return;
	}
	for (int i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			hard_fail(display_name,
				  "[%d]: got %u, nvram = %u",
				  i, got[i], want[i]);
			return;
		}
	}
	hard_pass(display_name);
}

/* --- main: load data, run parser, run all checks ------------------ */

int main(int argc, char **argv)
{
	int synth_mode = 0;
	const char *srom_path  = NULL;
	const char *nvram_path = NULL;

	if (argc > 1 && strcmp(argv[1], "--synth") == 0) {
		/* NVRAM-only round-trip: synthesize a raw SROM from the
		 * NVRAM using the patch's offsets/encoding, then run the
		 * parser against it. The check sequence below is identical;
		 * fields the NVRAM does not declare emit INFO and skip.
		 *
		 * Caveat: by construction, synth and parse share offsets,
		 * so this mode validates structural completeness and
		 * encoding self-consistency, NOT external offset truth.
		 * For canonical cross-checks, see ../cross_check.md. */
		synth_mode = 1;
		nvram_path = argc > 2 ? argv[2] : "vectors/bcm4360usb.nvram";
	} else {
		srom_path  = argc > 1 ? argv[1] : "../../../router-data/dsl3580l/wl1_srom_raw.txt";
		nvram_path = argc > 2 ? argv[2] : "../../../router-data/dsl3580l/wl1_nvram.txt";
	}

	printf("=== bcma_sprom_extract_r11 offline harness ===\n");
	if (synth_mode)
		printf("  mode : synth (NVRAM round-trip)\n");
	else
		printf("  srom : %s\n", srom_path);
	printf("  nvram: %s\n\n", nvram_path);

	u16 srom[SROM_MAX_WORDS] = {0};
	if (synth_mode) {
		struct nvram nv_in;
		if (nvram_load(nvram_path, &nv_in) < 0)
			return 2;
		if (synth_srom_from_nvram(&nv_in, srom, SROM_MAX_WORDS) < 0) {
			fprintf(stderr, "synth_srom_from_nvram failed\n");
			return 2;
		}
		printf("synthesized SROM from %zu NVRAM entries\n", nv_in.n);
	} else {
		int n = srom_load(srom_path, srom, SROM_MAX_WORDS);
		if (n < 0)
			return 2;
		printf("loaded %d SROM words\n", n);
	}

	struct nvram nv;
	if (nvram_load(nvram_path, &nv) < 0)
		return 2;
	printf("loaded %zu NVRAM entries\n\n", nv.n);

	struct bcma_bus bus = {0};
	run_extract_r11(&bus, srom);

	/* === Header (HARD where SROM is authoritative) =============== */
	printf("[ Header ]\n");
	check_u_hard(&nv, "boardrev",   bus.sprom.board_rev,        "board_rev");
	check_u_hard(&nv, "boardtype",  bus.sprom.board_type,       "board_type");
	check_u_hard(&nv, "aa2g",       bus.sprom.ant_available_bg, "ant_available_bg");
	check_u_hard(&nv, "aa5g",       bus.sprom.ant_available_a,  "ant_available_a");
	check_u_hard(&nv, "txchain",    bus.sprom.txchain,          "txchain");
	check_u_hard(&nv, "rxchain",    bus.sprom.rxchain,          "rxchain");
	check_u_hard(&nv, "antswitch",  bus.sprom.antswitch,        "antswitch");

	/* il0mac: SROM and NVRAM may be different sources (factory chip
	 * MAC in SROM vs OEM CFE-overridden MAC in NVRAM). Report
	 * relationship — PASS if they coincide, INFO otherwise. */
	{
		u8 want[6];
		const char *raw = nvram_get(&nv, "macaddr");
		if (raw && parse_mac(raw, want) == 0) {
			int srom_zero =
				(bus.sprom.il0mac[0] | bus.sprom.il0mac[1] |
				 bus.sprom.il0mac[2] | bus.sprom.il0mac[3] |
				 bus.sprom.il0mac[4] | bus.sprom.il0mac[5]) == 0;
			if (memcmp(bus.sprom.il0mac, want, 6) == 0) {
				hard_pass("il0mac");
			} else if (srom_zero) {
				info_note("il0mac",
					  "SROM region zero (NVRAM macaddr=%s comes from a separate CFE store)",
					  raw);
			} else {
				const u8 *m = bus.sprom.il0mac;
				info_note("il0mac",
					  "SROM-derived MAC %02x:%02x:%02x:%02x:%02x:%02x (OUI %02x:%02x:%02x), "
					  "nvram macaddr=%s — likely OEM CFE override",
					  m[0], m[1], m[2], m[3], m[4], m[5],
					  m[0], m[1], m[2], raw);
			}
		}
	}

	/* country_code: SROM and NVRAM diverge legitimately. */
	{
		const char *cc_raw = nvram_get(&nv, "ccode");
		info_note("country_code",
			  "SROM=0x%04x, nvram ccode='%s' (legitimate divergence: ccode is NVRAM-overridable)",
			  bus.sprom.country_code, cc_raw ? cc_raw : "");
	}

	/* === 5 GHz sub-band layout selector =========================== */
	printf("\n[ 5 GHz layout ]\n");
	check_u_hard(&nv, "subband5gver", bus.sprom.subband5gver, "subband5gver");

	/* === Per-chain pdoffset40ma triplet =========================== */
	printf("\n[ pdoffset40ma ]\n");
	check_u_hard(&nv, "pdoffset40ma0", bus.sprom.pdoffset40ma[0], "pdoffset40ma[0]");
	check_u_hard(&nv, "pdoffset40ma1", bus.sprom.pdoffset40ma[1], "pdoffset40ma[1]");
	check_u_hard(&nv, "pdoffset40ma2", bus.sprom.pdoffset40ma[2], "pdoffset40ma[2]");

	/* === Per-chain power info blocks ============================== */
	for (int chain = 0; chain < 3; chain++) {
		printf("\n[ Chain %d power info ]\n", chain);
		struct ssb_sprom_core_pwr_info *p = &bus.sprom.core_pwr_info[chain];
		char key[32], name[64];

		snprintf(key,  sizeof(key),  "maxp2ga%d", chain);
		snprintf(name, sizeof(name), "maxp2ga[%d]", chain);
		check_u_hard(&nv, key, p->maxp2ga, name);

		snprintf(key,  sizeof(key),  "pa2ga%d", chain);
		snprintf(name, sizeof(name), "pa2ga[%d]", chain);
		check_u16_list_hard(&nv, key, p->pa2ga, 3, name);

		snprintf(key,  sizeof(key),  "maxp5ga%d", chain);
		snprintf(name, sizeof(name), "maxp5ga[%d]", chain);
		check_u8_list_hard(&nv, key, p->maxp5ga, 4, name);

		snprintf(key,  sizeof(key),  "pa5ga%d", chain);
		snprintf(name, sizeof(name), "pa5ga[%d]", chain);
		check_u16_list_hard(&nv, key, p->pa5ga, 12, name);

		/* rxgains: 4 sub-bands × 3 fields = 12 keys per chain.
		 * NVRAM "rxgains5g..." (without l/m/h) is the UNII-1 sub-band,
		 * mapped by the parser into bus->sprom.rxgains_5gl. See the
		 * byte-to-sub-band assignment comment in extract_r11.c. */
		struct {
			const char *band_nv;	/* nvram key segment */
			const char *band_disp;	/* display name */
			struct ssb_sprom_rxgains *rx;
		} bands[] = {
			{"2g",  "2g",  &bus.sprom.rxgains_2g},
			{"5g",  "5gl", &bus.sprom.rxgains_5gl},  /* UNII-1 */
			{"5gm", "5gm", &bus.sprom.rxgains_5gm},
			{"5gh", "5gh", &bus.sprom.rxgains_5gh},
		};
		for (size_t b = 0; b < sizeof(bands)/sizeof(bands[0]); b++) {
			struct {
				const char *field_nv;
				const char *field_disp;
				u8 got;
			} fields[] = {
				{"elnagain",  "elnagain",  bands[b].rx->elnagain[chain]},
				{"triso",     "triso",     bands[b].rx->triso[chain]},
				{"trelnabyp", "trelnabyp", bands[b].rx->trelnabyp[chain]},
			};
			for (size_t fi = 0; fi < sizeof(fields)/sizeof(fields[0]); fi++) {
				snprintf(key, sizeof(key),
					 "rxgains%s%sa%d",
					 bands[b].band_nv, fields[fi].field_nv, chain);
				snprintf(name, sizeof(name),
					 "rxgains_%s.%s[%d]",
					 bands[b].band_disp, fields[fi].field_disp, chain);
				check_u_hard(&nv, key, fields[fi].got, name);
			}
		}
	}

	/* === Power-per-rate region ==================================== */
	printf("\n[ Power-per-rate ]\n");
	check_u_hard(&nv, "cckbw202gpo",          bus.sprom.cckbw202gpo,          "cckbw202gpo");
	check_u_hard(&nv, "cckbw20ul2gpo",        bus.sprom.cckbw20ul2gpo,        "cckbw20ul2gpo");
	check_u_hard(&nv, "mcsbw202gpo",          bus.sprom.mcsbw202gpo,          "mcsbw202gpo");
	check_u_hard(&nv, "mcsbw402gpo",          bus.sprom.mcsbw402gpo,          "mcsbw402gpo");
	check_u_hard(&nv, "dot11agofdmhrbw202gpo",bus.sprom.dot11agofdmhrbw202gpo,"dot11agofdmhrbw202gpo");
	check_u_hard(&nv, "ofdmlrbw202gpo",       bus.sprom.ofdmlrbw202gpo,       "ofdmlrbw202gpo");
	check_u_hard(&nv, "mcsbw205glpo",         bus.sprom.mcsbw205glpo,         "mcsbw205glpo");
	check_u_hard(&nv, "mcsbw405glpo",         bus.sprom.mcsbw405glpo,         "mcsbw405glpo");
	check_u_hard(&nv, "mcsbw805glpo",         bus.sprom.mcsbw805glpo,         "mcsbw805glpo");
	check_u_hard(&nv, "mcsbw1605glpo",        bus.sprom.mcsbw1605glpo,        "mcsbw1605glpo");
	check_u_hard(&nv, "mcsbw205gmpo",         bus.sprom.mcsbw205gmpo,         "mcsbw205gmpo");
	check_u_hard(&nv, "mcsbw405gmpo",         bus.sprom.mcsbw405gmpo,         "mcsbw405gmpo");
	check_u_hard(&nv, "mcsbw805gmpo",         bus.sprom.mcsbw805gmpo,         "mcsbw805gmpo");
	check_u_hard(&nv, "mcsbw1605gmpo",        bus.sprom.mcsbw1605gmpo,        "mcsbw1605gmpo");
	check_u_hard(&nv, "mcsbw205ghpo",         bus.sprom.mcsbw205ghpo,         "mcsbw205ghpo");
	check_u_hard(&nv, "mcsbw405ghpo",         bus.sprom.mcsbw405ghpo,         "mcsbw405ghpo");
	check_u_hard(&nv, "mcsbw805ghpo",         bus.sprom.mcsbw805ghpo,         "mcsbw805ghpo");
	check_u_hard(&nv, "mcsbw1605ghpo",        bus.sprom.mcsbw1605ghpo,        "mcsbw1605ghpo");

	/* === Summary ================================================== */
	printf("\n=== Summary ===\n");
	printf("  PASS: %d\n", g_pass);
	printf("  FAIL: %d\n", g_fail);
	printf("  INFO: %d\n", g_info);
	return g_fail == 0 ? 0 : 1;
}
