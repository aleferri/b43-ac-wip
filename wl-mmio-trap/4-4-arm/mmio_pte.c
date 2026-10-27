// SPDX-License-Identifier: GPL-2.0
/*
 * Target: kernel 4.4, ARMv7 Cortex-A9, 2-level MMU (no LPAE).
 *
 * The structure is the MIPS mmio_pte's. What changes on ARM:
 *   - the bits that make a page reachable are L_PTE_PRESENT | L_PTE_VALID
 *     (both bit 0 on this layout) and L_PTE_YOUNG; as_set_pte_ext() (the
 *     processor's set_pte_ext, resolved in arm_syms, not set_pte_at which
 *     pulls the unexported __sync_icache_dcache) mirrors the Linux pte into
 *     the hardware pt 2 KB above it, so clearing them stops the MMU from
 *     translating the page while the PFN stays recoverable;
 *   - init_mm and the TLB flush come from arm_syms.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <asm/pgtable.h>

#include "mmio_pte.h"
#include "arm_syms.h"

struct mmio_page {
	unsigned long va;
	pte_t *ptep;
	pte_t orig;
};

struct mmio_window {
	unsigned long base;
	size_t len;
	unsigned int npages;
	bool closed;
	struct mmio_page pages[];
};

static pte_t pte_make_absent(pte_t p)
{
	pte_val(p) &= ~(L_PTE_PRESENT | L_PTE_VALID | L_PTE_YOUNG);
	return p;
}

pte_t *mmio_pte_walk(unsigned long addr)
{
	struct mm_struct *mm = as_init_mm();
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	if (!mm || !mm->pgd)
		return NULL;
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return NULL;
	pud = pud_offset(pgd, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;
	return pte_offset_kernel(pmd, addr);
}

struct mmio_window *mmio_pte_arm(unsigned long va, size_t len)
{
	unsigned int npages, i;
	struct mmio_window *w;

	if (!len || (va & ~PAGE_MASK))
		return NULL;
	npages = (unsigned int)DIV_ROUND_UP(len, PAGE_SIZE);

	w = kzalloc(sizeof(*w) + npages * sizeof(w->pages[0]), GFP_KERNEL);
	if (!w)
		return NULL;
	w->base = va;
	w->len = (size_t)npages * PAGE_SIZE;
	w->npages = npages;

	for (i = 0; i < npages; i++) {
		unsigned long pva = va + i * PAGE_SIZE;
		pte_t *ptep = mmio_pte_walk(pva);

		if (!ptep || !pte_present(*ptep)) {
			kfree(w);
			return NULL;
		}
		w->pages[i].va = pva;
		w->pages[i].ptep = ptep;
		w->pages[i].orig = *ptep;
	}
	return w;
}

phys_addr_t mmio_pte_phys(const struct mmio_window *w)
{
	if (!w || !w->npages)
		return 0;
	return (phys_addr_t)pte_pfn(w->pages[0].orig) << PAGE_SHIFT;
}

unsigned long mmio_pte_base(const struct mmio_window *w)
{
	return w ? w->base : 0;
}

size_t mmio_pte_len(const struct mmio_window *w)
{
	return w ? w->len : 0;
}

bool mmio_pte_is_closed(const struct mmio_window *w)
{
	return w && w->closed;
}

void mmio_pte_close(struct mmio_window *w)
{
	unsigned int i;

	if (!w || w->closed)
		return;
	for (i = 0; i < w->npages; i++)
		as_set_pte_ext(w->pages[i].ptep,
			       pte_make_absent(w->pages[i].orig));
	w->closed = true;
	as_flush_tlb_kernel(w->base, w->base + w->len);
}

void mmio_pte_open(struct mmio_window *w)
{
	unsigned int i;

	if (!w || !w->closed)
		return;
	for (i = 0; i < w->npages; i++)
		as_set_pte_ext(w->pages[i].ptep, w->pages[i].orig);
	w->closed = false;
	as_flush_tlb_kernel(w->base, w->base + w->len);
}

void mmio_pte_release(struct mmio_window *w)
{
	if (!w)
		return;
	mmio_pte_open(w);
	kfree(w);
}
