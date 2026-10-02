#include "snapshot.h"

void snapshot_write(const struct d11_core *c, FILE *out, int include_ucode)
{
	/* REG: 32-bit words at 4-byte steps */
	for (uint32_t off = 0; off + 4 <= D11_REG_BYTES; off += 4) {
		uint32_t v = (uint32_t)c->reg[off] |
			     ((uint32_t)c->reg[off + 1] << 8) |
			     ((uint32_t)c->reg[off + 2] << 16) |
			     ((uint32_t)c->reg[off + 3] << 24);
		if (v)
			fprintf(out, "REG 0x%04x 0x%08x\n", off, v);
	}

	/* SHM: 16-bit words at 2-byte steps */
	for (uint32_t off = 0; off + 2 <= D11_SHM_BYTES; off += 2) {
		uint16_t v = core_shm16_read(c, off);
		if (v)
			fprintf(out, "SHM 0x%04x 0x%04x\n", off, v);
	}

	for (uint32_t i = 0; i < D11_SCR_WORDS; i++)
		if (c->scr[i])
			fprintf(out, "SCR 0x%04x 0x%08x\n", i, c->scr[i]);

	for (uint32_t i = 0; i < D11_HW_WORDS; i++)
		if (c->hw[i])
			fprintf(out, "HW 0x%04x 0x%08x\n", i, c->hw[i]);

	for (uint32_t i = 0; i < D11_RCMTA_WORDS; i++)
		if (c->rcmta[i])
			fprintf(out, "RCMTA 0x%04x 0x%08x\n", i, c->rcmta[i]);

	if (include_ucode)
		for (size_t i = 0; i < c->ucode_words; i++)
			if (c->ucode[i])
				fprintf(out, "UCODE 0x%04zx 0x%08x\n", i, c->ucode[i]);
}
