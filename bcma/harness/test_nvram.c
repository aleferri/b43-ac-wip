/*
 * test_nvram.c — drive bcm47xx_fill_sprom(), the tree's
 * drivers/firmware/broadcom/bcm47xx_sprom.c compiled as it is, on a
 * `wl nvram_dump` served as a router's NVRAM under the "0:" prefix, and
 * check every SROM rev 11 field of struct ssb_sprom, and the older ones the
 * AC-PHY reads, against the dump.
 *
 * This is the path of a board whose radios have no SROM and keep their data
 * in NVRAM, as the Northstar routers do. A variable missing from the dump is
 * SKIP; the exit status is 0 if nothing failed.
 */

#include <linux/ssb/ssb.h>
#include <linux/bcm47xx_sprom.h>

#include "nvram_backend.h"

#include <inttypes.h>

static int n_pass, n_fail, n_skip;

static void result(const char *name, bool ok, const char *detail)
{
	if (ok) {
		n_pass++;
		printf("  PASS  %s\n", name);
	} else {
		n_fail++;
		printf("  FAIL  %s — %s\n", name, detail);
	}
}

/* The dump's list, independently of the kernel's parser. */
static int parse_list(const char *s, uint64_t *out, int max)
{
	int n = 0;
	char *end;

	while (n < max) {
		out[n++] = strtoull(s, &end, 0);
		if (end == s)
			return -1;
		if (!*end)
			return n;
		if (*end != ',')
			return -1;
		s = end + 1;
	}
	return -1;
}

static void check_list(const char *key, const uint64_t *got, int n)
{
	const char *raw = nvram_backend_value(key);
	uint64_t want[16];
	char detail[160];
	int i;

	if (!raw) {
		n_skip++;
		printf("  SKIP  %s\n", key);
		return;
	}
	if (parse_list(raw, want, 16) != n) {
		snprintf(detail, sizeof(detail), "'%s' is not %d values", raw, n);
		result(key, false, detail);
		return;
	}
	for (i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			snprintf(detail, sizeof(detail),
				 "[%d] got 0x%" PRIx64 ", nvram 0x%" PRIx64,
				 i, got[i], want[i]);
			result(key, false, detail);
			return;
		}
	}
	result(key, true, NULL);
}

#define CHECK(key, field) do {						\
	uint64_t v_ = (field);						\
	check_list(key, &v_, 1);					\
} while (0)

#define CHECK_LIST(key, arr) do {					\
	uint64_t v_[ARRAY_SIZE(arr)];					\
	size_t i_;							\
	for (i_ = 0; i_ < ARRAY_SIZE(arr); i_++)			\
		v_[i_] = (arr)[i_];					\
	check_list(key, v_, ARRAY_SIZE(arr));				\
} while (0)

#define KEY(fmt, ...) (snprintf(key, sizeof(key), fmt, __VA_ARGS__), key)

/* Whole dB in [5:0], signed, quarter dB in [7:6]. */
static void check_antgain(const char *key, s8 got)
{
	const char *raw = nvram_backend_value(key);
	char detail[80];
	int b, whole;

	if (!raw) {
		n_skip++;
		printf("  SKIP  %s\n", key);
		return;
	}
	b = strtoul(raw, NULL, 0) & 0xff;
	whole = b & 0x3f;
	if (whole & 0x20)
		whole -= 0x40;
	snprintf(detail, sizeof(detail), "got %d qdB, nvram %d qdB", got,
		 4 * whole + (b >> 6));
	result(key, got == 4 * whole + (b >> 6), detail);
}

int main(int argc, char **argv)
{
	struct ssb_sprom sprom;
	const struct {
		const char *band;
		const struct ssb_sprom_rxgains *rx;
	} bands[] = {
		{ "2g",  &sprom.rxgains_2g },
		{ "5g",  &sprom.rxgains_5gl },
		{ "5gm", &sprom.rxgains_5gm },
		{ "5gh", &sprom.rxgains_5gh },
	};
	char key[48];
	int c, b;

	const char *prefix = "";

	if (argc < 2 || argc > 3) {
		fprintf(stderr,
			"usage: %s wl_nvram_dump.txt [prefix]\n"
			"  The repo's dumps carry bare keys, so prefix defaults\n"
			"  to \"\". Pass a raw `nvram show` and its prefix to test\n"
			"  that: \"0:\"/\"1:\" or, on a Netgear board, \"pci/1/1/\" /\n"
			"  \"pci/2/1/\", the devpathN that names the radio's slot.\n",
			argv[0]);
		return 2;
	}
	if (argc == 3)
		prefix = argv[2];
	/* The backend strips @prefix off the keys at load, so from here on
	 * the dump is bare and bcm47xx_fill_sprom() runs with no prefix,
	 * whatever the dump used. */
	if (nvram_backend_load(argv[1], prefix) < 0)
		return 2;
	printf("=== bcm47xx_fill_sprom, dump prefix %s ===\n  nvram: %s\n\n",
	       prefix[0] ? prefix : "(none)", argv[1]);

	memset(&sprom, 0, sizeof(sprom));
	bcm47xx_fill_sprom(&sprom, "", false);

	printf("[ Header ]\n");
	CHECK("sromrev", sprom.revision);
	CHECK("boardflags", sprom.boardflags);
	CHECK("boardflags2", sprom.boardflags2);
	CHECK("boardflags3", sprom.boardflags3);
	CHECK("aa2g", sprom.ant_available_bg);
	CHECK("aa5g", sprom.ant_available_a);
	CHECK("txchain", sprom.txchain);
	CHECK("rxchain", sprom.rxchain);
	CHECK("antswitch", sprom.antswitch);
	CHECK("subband5gver", sprom.subband5gver);

	printf("\n[ FEM ]\n");
	CHECK("femctrl", sprom.femctrl);
	CHECK("tssiposslope2g", sprom.tssiposslope2g);
	CHECK("epagain2g", sprom.epagain2g);
	CHECK("pdgain2g", sprom.pdgain2g);
	CHECK("tworangetssi2g", sprom.tworangetssi2g);
	CHECK("papdcap2g", sprom.papdcap2g);
	CHECK("tssiposslope5g", sprom.tssiposslope5g);
	CHECK("epagain5g", sprom.epagain5g);
	CHECK("pdgain5g", sprom.pdgain5g);
	CHECK("tworangetssi5g", sprom.tworangetssi5g);
	CHECK("papdcap5g", sprom.papdcap5g);
	CHECK("gainctrlsph", sprom.gainctrlsph);
	CHECK("tssifloor2g", sprom.tssifloor2g);
	CHECK_LIST("tssifloor5g", sprom.tssifloor5g);
	check_antgain("agbg0", sprom.antenna_gain_qdb[0]);
	check_antgain("aga0", sprom.antenna_gain_qdb[1]);

	for (c = 0; c < 3; c++) {
		const struct ssb_sprom_core_pwr_info *p = &sprom.core_pwr_info[c];

		printf("\n[ Chain %d ]\n", c);
		CHECK(KEY("maxp2ga%d", c), p->maxp2ga);
		CHECK_LIST(KEY("pa2ga%d", c), p->pa2ga);
		CHECK_LIST(KEY("maxp5ga%d", c), p->maxp5ga);
		CHECK_LIST(KEY("pa5ga%d", c), p->pa5ga);
		CHECK(KEY("pdoffset40ma%d", c), sprom.pdoffset40ma[c]);
		CHECK(KEY("pdoffset80ma%d", c), sprom.pdoffset80ma[c]);
		for (b = 0; b < ARRAY_SIZE(bands); b++) {
			const char *band = bands[b].band;
			const struct ssb_sprom_rxgains *rx = bands[b].rx;

			CHECK(KEY("rxgains%selnagaina%d", band, c),
			      rx->elnagain[c]);
			CHECK(KEY("rxgains%strisoa%d", band, c), rx->triso[c]);
			CHECK(KEY("rxgains%strelnabypa%d", band, c),
			      rx->trelnabyp[c]);
		}
	}

	printf("\n[ Power per rate ]\n");
	CHECK("cckbw202gpo", sprom.cckbw202gpo);
	CHECK("cckbw20ul2gpo", sprom.cckbw20ul2gpo);
	CHECK("mcsbw202gpo", sprom.mcsbw202gpo);
	CHECK("mcsbw402gpo", sprom.mcsbw402gpo);
	CHECK("dot11agofdmhrbw202gpo", sprom.dot11agofdmhrbw202gpo);
	CHECK("ofdmlrbw202gpo", sprom.ofdmlrbw202gpo);
	CHECK("mcsbw205glpo", sprom.mcsbw205glpo);
	CHECK("mcsbw405glpo", sprom.mcsbw405glpo);
	CHECK("mcsbw805glpo", sprom.mcsbw805glpo);
	CHECK("mcsbw205gmpo", sprom.mcsbw205gmpo);
	CHECK("mcsbw405gmpo", sprom.mcsbw405gmpo);
	CHECK("mcsbw805gmpo", sprom.mcsbw805gmpo);
	CHECK("mcsbw205ghpo", sprom.mcsbw205ghpo);
	CHECK("mcsbw405ghpo", sprom.mcsbw405ghpo);
	CHECK("mcsbw805ghpo", sprom.mcsbw805ghpo);
	CHECK("mcslr5glpo", sprom.mcslr5gpo[0]);
	CHECK("mcslr5gmpo", sprom.mcslr5gpo[1]);
	CHECK("mcslr5ghpo", sprom.mcslr5gpo[2]);
	CHECK("sb20in40hrpo", sprom.sb20in40hrpo);
	CHECK("sb20in40lrpo", sprom.sb20in40lrpo);
	CHECK("sb20in80and160hr5glpo", sprom.sb20in80and160hr5gpo[0]);
	CHECK("sb20in80and160hr5gmpo", sprom.sb20in80and160hr5gpo[1]);
	CHECK("sb20in80and160hr5ghpo", sprom.sb20in80and160hr5gpo[2]);
	CHECK("sb20in80and160lr5glpo", sprom.sb20in80and160lr5gpo[0]);
	CHECK("sb20in80and160lr5gmpo", sprom.sb20in80and160lr5gpo[1]);
	CHECK("sb20in80and160lr5ghpo", sprom.sb20in80and160lr5gpo[2]);
	CHECK("sb40and80hr5glpo", sprom.sb40and80hr5gpo[0]);
	CHECK("sb40and80hr5gmpo", sprom.sb40and80hr5gpo[1]);
	CHECK("sb40and80hr5ghpo", sprom.sb40and80hr5gpo[2]);
	CHECK("sb40and80lr5glpo", sprom.sb40and80lr5gpo[0]);
	CHECK("sb40and80lr5gmpo", sprom.sb40and80lr5gpo[1]);
	CHECK("sb40and80lr5ghpo", sprom.sb40and80lr5gpo[2]);
	CHECK("dot11agduphrpo", sprom.dot11agduphrpo);
	CHECK("dot11agduplrpo", sprom.dot11agduplrpo);

	printf("\n[ rpcal ]\n");
	CHECK("rpcal2g", sprom.rpcal2g);
	CHECK("rpcal5gb0", sprom.rpcal5gb[0]);
	CHECK("rpcal5gb1", sprom.rpcal5gb[1]);
	CHECK("rpcal5gb2", sprom.rpcal5gb[2]);
	CHECK("rpcal5gb3", sprom.rpcal5gb[3]);

	printf("\n[ Temperature ]\n");
	CHECK("tempthresh", sprom.tempthresh);
	CHECK("phycal_tempdelta", sprom.phycal_tempdelta);
	CHECK("temps_period", sprom.temps_period);
	CHECK("temps_hysteresis", sprom.temps_hysteresis);

	printf("\n=== Summary ===\n  PASS: %d\n  FAIL: %d\n  SKIP: %d\n",
	       n_pass, n_fail, n_skip);
	return n_fail ? 1 : 0;
}
