/*
 * d11sim - run a driver's init op stream against the modelled d11 core and
 * dump the final core state, or inventory an extracted ucode blob.
 *
 *   d11sim replay --ops FILE [--blob FILE]
 *                 [--ucode-in-dump] --dump OUT [--label NAME]
 *   d11sim inventory --blob FILE
 *
 * The dump is the artifact the equivalence check compares: produce one for the
 * wl stream and one for the b43 stream, then diff them with compare_state.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d11_core.h"
#include "fakephy.h"
#include "psm42.h"
#include "ops.h"
#include "snapshot.h"

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return NULL; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(n > 0 ? n : 1);
	if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	*len = n;
	return buf;
}

static int cmd_inventory(const char *blob_path)
{
	size_t n;
	uint8_t *blob = read_file(blob_path, &n);
	if (!blob)
		return 1;
	struct d11_core core;
	struct psm42 psm;
	core_init(&core);
	psm42_attach(&psm, &core);
	size_t words = psm42_load_blob(&psm, blob, n);
	fprintf(stderr, "loaded %zu words from %s\n", words, blob_path);
	psm42_report_inventory(&psm, stdout);
	free(blob);
	return 0;
}

static int cmd_replay(int argc, char **argv)
{
	const char *ops_path = NULL, *blob_path = NULL, *dump_path = NULL;
	const char *label = "stream";
	int include_ucode = 0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--ops") && i + 1 < argc) ops_path = argv[++i];
		else if (!strcmp(argv[i], "--blob") && i + 1 < argc) blob_path = argv[++i];
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_path = argv[++i];
		else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
		else if (!strcmp(argv[i], "--ucode-in-dump")) include_ucode = 1;
		else {
			fprintf(stderr, "unknown arg: %s\n", argv[i]);
			return 2;
		}
	}
	if (!ops_path || !dump_path) {
		fprintf(stderr, "replay needs --ops and --dump\n");
		return 2;
	}

	struct d11_core core;
	struct fakephy phy;
	struct psm42 psm;
	core_init(&core);
	fakephy_init(&phy, 0x0000, 0x0000);
	psm42_attach(&psm, &core);

	if (blob_path) {
		size_t n;
		uint8_t *blob = read_file(blob_path, &n);
		if (!blob)
			return 1;
		size_t w = psm42_load_blob(&psm, blob, n);
		fprintf(stderr, "[%s] preloaded ucode blob: %zu words\n", label, w);
		free(blob);
	}

	FILE *in = fopen(ops_path, "r");
	if (!in) { perror(ops_path); return 1; }
	struct replay_stats st;
	replay_stream(in, &core, &phy, &st);
	fclose(in);

	FILE *out = fopen(dump_path, "w");
	if (!out) { perror(dump_path); return 1; }
	snapshot_write(&core, out, include_ucode);
	fclose(out);

	fprintf(stderr,
		"[%s] lines=%lu applied=%lu ignored=%lu parse_err=%lu\n"
		"      REG wr=%lu rd=%lu | OBJ wr=%lu rd=%lu | MAC mctrl=%lu mcmd=%lu\n"
		"      PHY wr=%lu rd=%lu (readback-diverge=%lu) | RAD wr=%lu rd=%lu\n"
		"      ucode words=%zu | psm booted=%s | bulk-folded(skipped)=%lu\n"
		"      dump -> %s%s\n",
		label, st.lines, st.applied, st.ignored, st.parse_errors,
		st.reg_wr, st.reg_rd, st.obj_wr, st.obj_rd, st.mac_mctrl, st.mac_mcmd,
		st.phy_wr, st.phy_rd, phy.phy_readback_diverge, st.rad_wr, st.rad_rd,
		core.ucode_words, core.psm_booted ? "yes" : "no", st.bulk_skipped,
		dump_path, include_ucode ? " (with ucode)" : "");

	if (st.bulk_skipped)
		fprintf(stderr,
			"[%s] WARNING: %lu folded OBJ.BULK lines had no values; "
			"re-run mmio2ops.py with --no-bulk\n", label, st.bulk_skipped);

	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "inventory") && argc >= 4 &&
	    !strcmp(argv[2], "--blob"))
		return cmd_inventory(argv[3]);
	if (argc >= 2 && !strcmp(argv[1], "replay"))
		return cmd_replay(argc - 2, argv + 2);

	fprintf(stderr,
		"usage:\n"
		"  %s replay --ops FILE [--blob FILE]\n"
		"            [--ucode-in-dump] --dump OUT [--label NAME]\n"
		"  %s inventory --blob FILE\n", argv[0], argv[0]);
	return 2;
}
