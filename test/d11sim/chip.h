/*
 * chip.h - the backplane around the d11 core: ChipCommon with the PMU's
 * indirect PLL, regulator and chip control words, the PCIe2 core with its
 * indirect configuration space, and the d11 core's agent (wrapper).
 *
 * Registers here have no side effect the comparison needs: a write is the
 * state. Only written cells are kept, so a dump shows what each driver set.
 */
#ifndef CHIP_H
#define CHIP_H

#include <stdint.h>
#include <stdio.h>

#define CHIP_REGS	(0x1000 / 4)	/* 32-bit words of one core's window */
#define CHIP_PMU_WORDS	32		/* indirect words per PMU array */
#define CHIP_CFG_WORDS	(0x2000 / 4)	/* PCIe2 indirect config space */

struct chip_space {
	uint32_t val[CHIP_CFG_WORDS];
	uint8_t written[CHIP_CFG_WORDS];
};

struct d11_chip {
	struct chip_space cc, pll, regctl, chipctl;
	struct chip_space pcie, pciecfg, wrap;
	uint32_t pll_addr, regctl_addr, chipctl_addr, pciecfg_addr;
};

void chip_init(struct d11_chip *c);
void chip_cc_write(struct d11_chip *c, uint32_t off, uint32_t val);
void chip_pcie_write(struct d11_chip *c, uint32_t off, uint32_t val);
void chip_wrap_write(struct d11_chip *c, uint32_t off, uint32_t val);
void chip_snapshot_write(const struct d11_chip *c, FILE *out);

#endif /* CHIP_H */
