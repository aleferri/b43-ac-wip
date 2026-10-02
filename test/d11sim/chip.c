#include "chip.h"
#include <string.h>

/* ChipCommon PMU indirect ports and the PCIe2 config window. */
#define CC_CHIPCTL_ADDR	0x650
#define CC_CHIPCTL_DATA	0x654
#define CC_REGCTL_ADDR	0x658
#define CC_REGCTL_DATA	0x65c
#define CC_PLLCTL_ADDR	0x660
#define CC_PLLCTL_DATA	0x664
#define PCIE2_CFG_ADDR	0x120
#define PCIE2_CFG_DATA	0x124

void chip_init(struct d11_chip *c)
{
	memset(c, 0, sizeof(*c));
}

static void space_set(struct chip_space *s, uint32_t idx, uint32_t limit,
		      uint32_t val)
{
	if (idx >= limit)
		return;
	s->val[idx] = val;
	s->written[idx] = 1;
}

void chip_cc_write(struct d11_chip *c, uint32_t off, uint32_t val)
{
	switch (off) {
	case CC_CHIPCTL_ADDR:
		c->chipctl_addr = val;
		return;
	case CC_REGCTL_ADDR:
		c->regctl_addr = val;
		return;
	case CC_PLLCTL_ADDR:
		c->pll_addr = val;
		return;
	case CC_CHIPCTL_DATA:
		space_set(&c->chipctl, c->chipctl_addr, CHIP_PMU_WORDS, val);
		return;
	case CC_REGCTL_DATA:
		space_set(&c->regctl, c->regctl_addr, CHIP_PMU_WORDS, val);
		return;
	case CC_PLLCTL_DATA:
		space_set(&c->pll, c->pll_addr, CHIP_PMU_WORDS, val);
		return;
	}
	space_set(&c->cc, off / 4, CHIP_REGS, val);
}

void chip_pcie_write(struct d11_chip *c, uint32_t off, uint32_t val)
{
	if (off == PCIE2_CFG_ADDR) {
		c->pciecfg_addr = val;
		return;
	}
	if (off == PCIE2_CFG_DATA) {
		space_set(&c->pciecfg, c->pciecfg_addr / 4, CHIP_CFG_WORDS, val);
		return;
	}
	space_set(&c->pcie, off / 4, CHIP_REGS, val);
}

void chip_wrap_write(struct d11_chip *c, uint32_t off, uint32_t val)
{
	space_set(&c->wrap, off / 4, CHIP_REGS, val);
}

/* Keys are byte offsets for the windows and indices for the PMU arrays. */
static void space_dump(FILE *out, const char *name, const struct chip_space *s,
		       uint32_t n, uint32_t scale)
{
	for (uint32_t i = 0; i < n; i++)
		if (s->written[i])
			fprintf(out, "%s 0x%04x 0x%08x\n", name, i * scale,
				s->val[i]);
}

void chip_snapshot_write(const struct d11_chip *c, FILE *out)
{
	space_dump(out, "CC", &c->cc, CHIP_REGS, 4);
	space_dump(out, "PMUPLL", &c->pll, CHIP_PMU_WORDS, 1);
	space_dump(out, "PMUREG", &c->regctl, CHIP_PMU_WORDS, 1);
	space_dump(out, "PMUCHIP", &c->chipctl, CHIP_PMU_WORDS, 1);
	space_dump(out, "PCIE", &c->pcie, CHIP_REGS, 4);
	space_dump(out, "PCIECFG", &c->pciecfg, CHIP_CFG_WORDS, 4);
	space_dump(out, "WRAP", &c->wrap, CHIP_REGS, 4);
}
