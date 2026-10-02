#include "psm42.h"
#include <stdio.h>
#include <string.h>

void psm42_attach(struct psm42 *p, struct d11_core *core)
{
	p->core = core;
}

size_t psm42_load_blob(struct psm42 *p, const uint8_t *blob, size_t nbytes)
{
	struct d11_core *c = p->core;
	size_t words = nbytes / 4;
	if (words > D11_UCODE_WORDS)
		words = D11_UCODE_WORDS;
	for (size_t i = 0; i < words; i++) {
		/* The blob is big-endian, as stored in the MSB wl object and as
		 * b43 uploads it (be32). */
		c->ucode[i] = ((uint32_t)blob[i * 4]     << 24) |
			      ((uint32_t)blob[i * 4 + 1] << 16) |
			      ((uint32_t)blob[i * 4 + 2] << 8)  |
			       (uint32_t)blob[i * 4 + 3];
	}
	c->ucode_words = words;
	return words;
}

int psm42_boot(struct psm42 *p)
{
	return core_ucode_boot(p->core);
}

void psm42_report_inventory(const struct psm42 *p, void *FILEp)
{
	FILE *f = FILEp;
	const struct d11_core *c = p->core;
	unsigned long hist[256] = { 0 };

	for (size_t i = 0; i < c->ucode_words; i++)
		hist[(c->ucode[i] >> 24) & 0xff]++;

	fprintf(f, "ucode words loaded : %zu\n", c->ucode_words);
	fprintf(f, "ucode bytes        : %zu\n", c->ucode_words * 4);
	fprintf(f, "top-byte frequency (opaque, not an opcode decode):\n");

	/* print the five most frequent high-bytes */
	for (int shown = 0; shown < 5; shown++) {
		int best = -1;
		for (int b = 0; b < 256; b++)
			if (hist[b] && (best < 0 || hist[b] > hist[best]))
				best = b;
		if (best < 0)
			break;
		fprintf(f, "  0x%02x : %lu\n", best, hist[best]);
		hist[best] = 0;
	}
}
