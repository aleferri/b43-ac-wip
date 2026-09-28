// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <asm/addrspace.h>
#include <asm/pgtable.h>

#include "win_redirect.h"

static void __iomem *area_va;
static phys_addr_t area_phys;		/* page-aligned */
static size_t area_len;			/* page-aligned */

int win_redirect_create(phys_addr_t phys, size_t len)
{
	phys_addr_t pa = phys & PAGE_MASK;
	size_t size = PAGE_ALIGN((size_t)(phys - pa) + len);
	void __iomem *va;

	if (area_va)
		return -EEXIST;
	if (!len)
		return -EINVAL;

	/* The shortcut that hands back a bare CKSEG1 address tests flags for
	 * EQUALITY with _CACHE_UNCACHED, while the mapping path ORs flags
	 * into _PAGE_GLOBAL | _PAGE_PRESENT | __READABLE | __WRITEABLE. So
	 * asking for a bit that path is going to set anyway fails the
	 * equality, takes the mapping path, and arrives at exactly the pte
	 * the shortcut was avoiding. The generic ioremap_page_range() is not
	 * an option here: lib/ioremap.o is in lib-y and MIPS never refers to
	 * it, so the linker drops it and the export with it. */
	va = __ioremap(pa, size, _CACHE_UNCACHED | _PAGE_GLOBAL);
	if (!va) {
		pr_err("wl_mmio_trap: __ioremap(%08llx, %zu) refused the mapping. A window that falls inside low memory is turned down by the RAM check unless its pages are reserved.\n",
		       (unsigned long long)pa, size);
		return -ENOMEM;
	}

	if (((unsigned long)va & ~0x1fffffffUL) == CKSEG1) {
		pr_err("wl_mmio_trap: __ioremap still came back with the CKSEG1 address %p; the flag did not change its mind\n",
		       va);
		iounmap(va);
		return -ENOTSUPP;
	}

	area_va = va;
	area_phys = pa;
	area_len = size;
	pr_info("wl_mmio_trap: window %08llx +%zu mapped uncached at %p, page tables and all\n",
		(unsigned long long)pa, size, va);
	return 0;
}

void win_redirect_destroy(void)
{
	void __iomem *va = area_va;

	if (!va)
		return;

	/* Cleared first: the breakpoint on __iounmap refuses calls that land
	 * inside this area, and this one is ours. */
	area_va = NULL;
	area_phys = 0;
	area_len = 0;
	iounmap(va);
}

unsigned long win_redirect_base(void)
{
	return (unsigned long)area_va;
}

size_t win_redirect_len(void)
{
	return area_len;
}

phys_addr_t win_redirect_phys(void)
{
	return area_phys;
}

bool win_redirect_answer(phys_addr_t phys, size_t size, unsigned long *va)
{
	if (!area_va || !size)
		return false;
	if (phys < area_phys || phys + size > area_phys + area_len)
		return false;

	/* __ioremap() maps from the containing page and adds the offset back
	 * on the way out; a request that does not start on a page boundary
	 * has to come back pointing at the byte it asked for. */
	*va = (unsigned long)area_va + (unsigned long)(phys - area_phys);
	return true;
}

bool win_redirect_owns(unsigned long va)
{
	unsigned long base = (unsigned long)area_va;

	return base && va >= base && va < base + area_len;
}
