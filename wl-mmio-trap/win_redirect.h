/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hands the driver a TLB-backed mapping of its register window instead of
 * the CKSEG1 address __ioremap() would have returned.
 *
 * Why: __ioremap() short-circuits any uncached mapping that lies entirely
 * inside the low 512 MB of physical space to a bare CKSEG1ADDR. That is a
 * fixed translation with no page table entry, so there is nothing for
 * mmio_pte.c to invalidate and nothing traps. The CP0 Watch registers,
 * which fault on a virtual address without involving the TLB, would be the
 * other way in, but cores that report "hardware watchpoint : no" do not
 * have them.
 *
 * The way round it is __ioremap() itself. Its shortcut tests flags for
 * EQUALITY with _CACHE_UNCACHED, while the path it is avoiding ORs flags
 * into _PAGE_GLOBAL | _PAGE_PRESENT | __READABLE | __WRITEABLE. Asking for
 * _CACHE_UNCACHED | _PAGE_GLOBAL therefore fails the equality, takes the
 * mapping path, and produces exactly the pte the shortcut was avoiding.
 * The generic ioremap_page_range() would have been the obvious tool and is
 * not available: lib/ioremap.o sits in lib-y and MIPS never refers to it,
 * so the linker drops the object and its export along with it.
 *
 * The mapping must stay uncached. A cached mapping of a device window is
 * broken with or without a tracer watching -- reads come from a line
 * instead of the register, writes sit in a line instead of reaching the
 * device, and a line fill drags in the neighbouring registers as a side
 * effect.
 *
 * The mapping is built ahead of time, in process context, because
 * __ioremap() can sleep and the breakpoint handler that answers the
 * driver's call cannot. The handler only compares the request against the
 * range and returns a pointer.
 *
 * This is the one part of this module that gives the driver something it
 * did not ask for. If the pointer is wrong, the driver writes through it
 * anyway. It is off unless redirect=1.
 */
#ifndef WIN_REDIRECT_H
#define WIN_REDIRECT_H

#include <linux/types.h>

/* Builds an uncached, page-table-backed mapping covering [phys, phys+len).
 * Sleeps; process context only. */
int win_redirect_create(phys_addr_t phys, size_t len);

/* Tears it down. The caller must have restored any pte it changed first:
 * this unmaps the range and frees the area. */
void win_redirect_destroy(void);

unsigned long win_redirect_base(void);	/* page-aligned VA, 0 when absent */
size_t win_redirect_len(void);
phys_addr_t win_redirect_phys(void);

/* The two questions the breakpoint handlers ask. Both are called from
 * exception context and neither allocates.
 *
 * _answer: is [phys, phys+size) inside the redirected range, and if so
 * what address should the driver be given? Mirrors __ioremap()'s own
 * handling of a request that does not start on a page boundary.
 *
 * _owns: does this VA fall in the area, i.e. is an iounmap of it ours to
 * refuse? */
bool win_redirect_answer(phys_addr_t phys, size_t size, unsigned long *va);
bool win_redirect_owns(unsigned long va);

#endif /* WIN_REDIRECT_H */
