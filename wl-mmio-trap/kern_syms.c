// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <asm/pgtable.h>
#include <asm/branch.h>
#include <asm/inst.h>

#include "compat.h"
#include "mmio_pte.h"

#include "kern_syms.h"

typedef void (*range_fn_t)(unsigned long, unsigned long);
typedef unsigned long (*lookup_fn_t)(const char *);

static struct mm_struct *p_init_mm;
static range_fn_t p_flush_icache;
static range_fn_t p_flush_tlb;
static unsigned long p_fixup_exception;
static lookup_fn_t p_lookup;

unsigned long ks_lookup(const char *name)
{
	return p_lookup ? p_lookup(name) : 0;
}

#ifdef MMIO_RETURN_EPC_FOR_INSN
int ks_compute_return_epc(struct pt_regs *regs, u32 insn)
{
	union mips_instruction i = { .word = insn };

	return __compute_return_epc_for_insn(regs, i);
}
#else
typedef int (*return_epc_fn_t)(struct pt_regs *);

static return_epc_fn_t p_return_epc;

int ks_compute_return_epc(struct pt_regs *regs, u32 insn)
{
	(void)insn;		/* __compute_return_epc() fetches it again */
	return p_return_epc(regs);
}
#endif

static range_fn_t resolve_range_fn(const char * const *cands, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned long a = ks_lookup(cands[i]);

		if (a) {
			pr_info("wl_mmio_trap: %s -> %p\n", cands[i], (void *)a);
			return (range_fn_t)a;
		}
	}
	return NULL;
}

/* Walking a page table that is not the kernel's does not fail loudly: a
 * stale or absent pgd entry yields a plausible-looking pte. So a candidate
 * is adopted only if the pte it produces for a fresh vmalloc page carries
 * the pfn vmalloc_to_pfn() reports for the same page. Two pages, because
 * one accidental match is cheaper to get than two. */
static bool mm_is_the_kernels(struct mm_struct *mm)
{
	unsigned int i;

	if (!mm || !mm->pgd)
		return false;

	for (i = 0; i < 2; i++) {
		unsigned long want, got;
		pte_t *ptep;
		void *p;

		p = vmalloc(PAGE_SIZE);
		if (!p)
			return false;
		*(volatile u32 *)p = 0;		/* fault it in */
		want = vmalloc_to_pfn(p);
		ptep = mmio_pte_walk_mm(mm, (unsigned long)p);
		got = (ptep && pte_present(*ptep)) ? pte_pfn(*ptep) : 0;
		vfree(p);

		if (!want || want != got)
			return false;
	}
	return true;
}

/* init_mm is a static object inside the kernel image, so it sits near the
 * other static kernel data; init_task is one such object whose address a
 * module knows, being exported. A borrowed user mm_struct comes from the
 * slab and lands nowhere near it.
 *
 * This matters because the pfn check alone does not catch a borrowed page
 * table: on two-level MIPS a pgd entry points straight at the pte page, and
 * vmalloc_fault() copies whole pgd entries, so a user mm that has faulted
 * on a 4 MB region shares that region's ptes with the kernel and walks it
 * correctly. It is the regions the borrowed mm never faulted on that come
 * back empty -- and an empty walk is indistinguishable from a window the
 * driver has not mapped yet. */
static bool mm_looks_static(const struct mm_struct *mm)
{
	unsigned long anchor = (unsigned long)&init_task;
	unsigned long a = (unsigned long)mm;

	return (a > anchor ? a - anchor : anchor - a) < (8UL << 20);
}

/* init_mm's static initialiser leaves a signature that nothing in the slab
 * shares: mmlist is a LIST_HEAD_INIT of itself, so two pointers at a known
 * offset point back at their own address; mmap is unset; and pgd is
 * swapper_pg_dir, a page-aligned array inside the kernel image and so in
 * KSEG0. The scan walks the kernel's static data around init_task -- which
 * is exported, and is the only address in there a module knows -- and hands
 * each hit to the same pfn check as every other candidate, so the signature
 * only has to narrow the field, not prove anything.
 *
 * Reading unmapped memory is not a risk here: the whole span is KSEG0, a
 * fixed translation over low RAM, and the end is clamped to high_memory.
 *
 * It fails, and the module falls back to the borrowed-mm candidates, if
 * anything has been added to init_mm.mmlist -- which happens when an mm is
 * queued for swap, and does not on a box with no swap. */
#define INIT_MM_SCAN_SPAN (4UL << 20)

static struct mm_struct *scan_for_init_mm(void)
{
	unsigned long anchor = (unsigned long)&init_task;
	unsigned long lo = anchor - INIT_MM_SCAN_SPAN;
	unsigned long hi = anchor + INIT_MM_SCAN_SPAN;
	unsigned long a;

	if (lo < PAGE_OFFSET || lo > anchor)
		lo = PAGE_OFFSET;
	if (high_memory && hi > (unsigned long)high_memory - sizeof(struct mm_struct))
		hi = (unsigned long)high_memory - sizeof(struct mm_struct);
	lo = ALIGN(lo, sizeof(long));

	for (a = lo; a < hi; a += sizeof(long)) {
		struct mm_struct *mm = (struct mm_struct *)a;
		unsigned long ml = (unsigned long)&mm->mmlist;

		if ((unsigned long)mm->mmlist.next != ml ||
		    (unsigned long)mm->mmlist.prev != ml)
			continue;
		if (mm->mmap)
			continue;
		if (!mm->pgd || ((unsigned long)mm->pgd & ~PAGE_MASK))
			continue;
		if ((unsigned long)mm->pgd < PAGE_OFFSET)
			continue;
		if (atomic_read(&mm->mm_users) != 2)
			continue;
		if (!mm_is_the_kernels(mm))
			continue;
		return mm;
	}
	return NULL;
}

static struct mm_struct *resolve_init_mm(unsigned long hint)
{
	struct {
		const char *how;
		struct mm_struct *mm;
	} cand[] = {
		{ "init_mm_addr=",        (struct mm_struct *)hint },
		{ "kallsyms",             (struct mm_struct *)ks_lookup("init_mm") },
		{ "signature scan",       scan_for_init_mm() },
		{ "init_task.active_mm",  init_task.active_mm },
		{ "current->active_mm",   current->active_mm },
	};
	struct mm_struct *borrowed = NULL;
	const char *borrowed_how = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cand); i++) {
		if (!cand[i].mm)
			continue;
		if (!mm_is_the_kernels(cand[i].mm)) {
			pr_info("wl_mmio_trap: %s gives %p, which does not walk to the right ptes -- rejected\n",
				cand[i].how, cand[i].mm);
			continue;
		}
		if (mm_looks_static(cand[i].mm)) {
			pr_info("wl_mmio_trap: kernel mm %p, from %s\n",
				cand[i].mm, cand[i].how);
			return cand[i].mm;
		}
		if (!borrowed) {
			borrowed = cand[i].mm;
			borrowed_how = cand[i].how;
		}
	}

	if (borrowed) {
		pr_warn("wl_mmio_trap: kernel mm %p, from %s -- but that is nowhere near the kernel's static data, so it is a page table borrowed from a user process, not init_mm. It walks correctly for the 4MB regions that process has already faulted on and comes back empty for the others, which looks exactly like a window the driver has not mapped yet. Pass init_mm_addr= from System.map to remove the doubt.\n",
			borrowed, borrowed_how);
		return borrowed;
	}
	return NULL;
}

int ks_init(unsigned long init_mm_hint, unsigned long klookup)
{
	/* The R4K back end of the flush_icache_range pointer, in preference
	 * order: the SMP-aware one first, so a patched word is visible to
	 * the other cpu's icache too. */
	static const char * const icache[] = {
		"r4k_flush_icache_range",
		"local_r4k_flush_icache_range",
		"local_flush_icache_range",
	};
	/* SMP builds have the IPI version; a UP build folds the macro onto
	 * the local one. */
	static const char * const tlb[] = {
		"flush_tlb_kernel_range",
		"local_flush_tlb_kernel_range",
	};

	if (klookup) {
		p_lookup = (lookup_fn_t)klookup;
	} else {
#ifdef MMIO_KALLSYMS_EXPORTED
		p_lookup = kallsyms_lookup_name;
#else
		pr_err("wl_mmio_trap: this kernel does not export kallsyms_lookup_name. Pass its address from /proc/kallsyms as klookup=0x...\n");
		return -EINVAL;
#endif
	}

	p_init_mm = resolve_init_mm(init_mm_hint);
	if (!p_init_mm) {
		pr_err("wl_mmio_trap: cannot find init_mm. It is a data symbol, so a kallsyms without KALLSYMS_ALL does not carry it. Get it from the kernel build: grep ' init_mm$' System.map, and pass init_mm_addr=0x...\n");
		return -ENOENT;
	}

	p_flush_icache = resolve_range_fn(icache, ARRAY_SIZE(icache));
	if (!p_flush_icache) {
		pr_err("wl_mmio_trap: no flush_icache_range back end found (CONFIG_KALLSYMS off?)\n");
		return -ENOENT;
	}

	p_flush_tlb = resolve_range_fn(tlb, ARRAY_SIZE(tlb));
	if (!p_flush_tlb) {
		pr_err("wl_mmio_trap: no flush_tlb_kernel_range found\n");
		return -ENOENT;
	}

	p_fixup_exception = ks_lookup("fixup_exception");
	if (!p_fixup_exception) {
		pr_err("wl_mmio_trap: fixup_exception not resolvable\n");
		return -ENOENT;
	}

#ifndef MMIO_RETURN_EPC_FOR_INSN
	p_return_epc = (return_epc_fn_t)ks_lookup("__compute_return_epc");
	if (!p_return_epc) {
		pr_err("wl_mmio_trap: __compute_return_epc not resolvable\n");
		return -ENOENT;
	}
#endif
	pr_info("wl_mmio_trap: fixup_exception -> %p, init_mm -> %p\n",
		(void *)p_fixup_exception, p_init_mm);
	return 0;
}

struct mm_struct *ks_init_mm(void)
{
	return p_init_mm;
}

unsigned long ks_fixup_exception(void)
{
	return p_fixup_exception;
}

void ks_flush_icache(unsigned long start, unsigned long end)
{
	if (p_flush_icache)
		p_flush_icache(start, end);
}

void ks_flush_tlb_kernel(unsigned long start, unsigned long end)
{
	if (p_flush_tlb)
		p_flush_tlb(start, end);
}
