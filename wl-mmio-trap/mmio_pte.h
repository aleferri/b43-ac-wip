/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Takes an already-existing kernel mapping -- wl's own ioremap of its
 * register window -- and switches the pages between "wl runs normally" and
 * "every access faults", by editing the pte_t wl is already running with.
 * Never a second mapping established behind wl's back for wl to use: the
 * only thing that changes is whether wl's own pte is valid.
 *
 * The valid/invalid state is per WINDOW, not per access. mmio_pte_close()
 * makes the whole window fault and leaves it that way; nothing in the fault
 * path touches a pte. The emulator reads and writes through a separate
 * alias of the same physical pages (see win_find.c), so the trap never has
 * to be lifted for the duration of an access -- which also closes the hole
 * a per-access lift would open, where the other cpu's access to the same
 * page sails through unseen while the page is momentarily valid.
 *
 * The invalid state keeps the PFN and _PAGE_GLOBAL and clears
 * _PAGE_PRESENT/_PAGE_VALID/_PAGE_ACCESSED. Keeping _PAGE_GLOBAL matters
 * on MIPS: the TLB entry covers an even/odd page pair and its G bit is the
 * AND of the two, so dropping it here would make the neighbouring page
 * ASID-tagged and fault spuriously from other contexts. Clearing
 * _PAGE_PRESENT (and not only _PAGE_VALID) matters too: it means an access
 * this module fails to catch ends in a visible Oops rather than in a silent
 * fault loop.
 */
#ifndef MMIO_PTE_H
#define MMIO_PTE_H

#include <linux/types.h>
#include <asm/pgtable.h>

struct mmio_window;

struct mm_struct;

/* Page-table walk to the leaf pte of va, or NULL if any level is missing.
 * mmio_pte_walk() uses the kernel mm that kern_syms.c resolved; the _mm
 * form takes one explicitly, which is how kern_syms.c checks a candidate
 * before adopting it. Shared with win_find.c, which walks the vmalloc
 * range looking for the mapping of a known physical address. */
pte_t *mmio_pte_walk(unsigned long va);
pte_t *mmio_pte_walk_mm(struct mm_struct *mm, unsigned long va);

/* Walks [va, va+len) and saves the live pte of every page. Touches
 * nothing. NULL if va is not page-aligned, or if any page is not a simple
 * present leaf pte -- a window that cannot be fully accounted for is not
 * half-armed. */
struct mmio_window *mmio_pte_arm(unsigned long va, size_t len);

/* Restores anything still invalid and frees the bookkeeping. */
void mmio_pte_release(struct mmio_window *w);

/* Physical base of the window, taken from the saved pte: what the alias
 * mapping has to cover. */
phys_addr_t mmio_pte_phys(const struct mmio_window *w);

void mmio_pte_close(struct mmio_window *w);	/* start faulting */
void mmio_pte_open(struct mmio_window *w);	/* stop faulting */
bool mmio_pte_is_closed(const struct mmio_window *w);

unsigned long mmio_pte_base(const struct mmio_window *w);
size_t mmio_pte_len(const struct mmio_window *w);

#endif /* MMIO_PTE_H */
