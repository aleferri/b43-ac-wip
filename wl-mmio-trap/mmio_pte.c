// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "mmio_pte.h"
#include "kern_syms.h"

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

/* Clears the bits that make a page reachable, keeping the PFN and
 * _PAGE_GLOBAL. pte_val() is an rvalue in the 64-bit-physical layout, where
 * a pte is a low/high pair, so the two cases are spelled out rather than
 * folded into one expression. */
static pte_t pte_make_absent(pte_t p)
{
	const unsigned long clear = _PAGE_PRESENT | _PAGE_VALID | _PAGE_ACCESSED;

#if defined(CONFIG_64BIT_PHYS_ADDR) && defined(CONFIG_CPU_MIPS32)
	p.pte_low &= ~clear;
#else
	pte_val(p) &= ~clear;
#endif
	return p;
}

pte_t *mmio_pte_walk_mm(struct mm_struct *mm, unsigned long addr)
{
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

pte_t *mmio_pte_walk(unsigned long addr)
{
	return mmio_pte_walk_mm(ks_init_mm(), addr);
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
		set_pte_at(ks_init_mm(), w->pages[i].va, w->pages[i].ptep,
			   pte_make_absent(w->pages[i].orig));
	w->closed = true;
	ks_flush_tlb_kernel(w->base, w->base + w->len);
}

void mmio_pte_open(struct mmio_window *w)
{
	unsigned int i;

	if (!w || !w->closed)
		return;
	for (i = 0; i < w->npages; i++)
		set_pte_at(ks_init_mm(), w->pages[i].va, w->pages[i].ptep,
			   w->pages[i].orig);
	w->closed = false;
	ks_flush_tlb_kernel(w->base, w->base + w->len);
}

void mmio_pte_release(struct mmio_window *w)
{
	if (!w)
		return;
	mmio_pte_open(w);
	kfree(w);
}
