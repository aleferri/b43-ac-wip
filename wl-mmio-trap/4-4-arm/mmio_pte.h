/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Invalidate wl's register window at the page-table level, and read back the
 * same pages through this module's own uncached alias, on the 2-level MMU of
 * an ARMv7 Cortex-A9. Same contract and shape as the MIPS mmio_pte, only the
 * pte bits differ.
 */
#ifndef MMIO_PTE_H
#define MMIO_PTE_H

#include <linux/types.h>

struct mmio_window;

/* Walk init_mm's page tables to the pte of a kernel VA; NULL if unmapped.
 * win_find uses it to locate wl's ioremap of the BAR. */
pte_t *mmio_pte_walk(unsigned long va);

struct mmio_window *mmio_pte_arm(unsigned long va, size_t len);
void mmio_pte_release(struct mmio_window *w);
phys_addr_t mmio_pte_phys(const struct mmio_window *w);
void mmio_pte_close(struct mmio_window *w);	/* start faulting */
void mmio_pte_open(struct mmio_window *w);	/* stop faulting */
bool mmio_pte_is_closed(const struct mmio_window *w);
unsigned long mmio_pte_base(const struct mmio_window *w);
size_t mmio_pte_len(const struct mmio_window *w);

#endif /* MMIO_PTE_H */
