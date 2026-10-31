// SPDX-License-Identifier: GPL-2.0
/*
 * Resolve the kernel symbols the ARM trap needs.
 *
 * flush_tlb_kernel_range and fsr_info come by name through
 * kallsyms_lookup_name, which ARM exports to modules from 2.6.33 on.
 *
 * init_mm is neither exported nor, being data, in kallsyms without
 * KALLSYMS_ALL. init_task.active_mm is not a way in: init_task is CPU 0's
 * idle thread, which borrows the previous task's mm on every switch (lazy
 * TLB), and a user pgd only carries the vmalloc/ioremap L1 entries that
 * existed when it was made or that it has faulted in since. So it is looked
 * up by name, or passed as init_mm_addr= from System.map.
 *
 * fsr_info is a static array (arch/arm/mm/fault.c), so its symbol is in
 * kallsyms only with KALLSYMS_ALL. Most kernels are built without it, so the
 * slot of entry 7 can also be passed as fsr_info_fn7=; either way the caller
 * checks the slot points at do_page_fault before touching it (verify_slot()
 * in the main file), so a wrong address is refused, not followed.
 */
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>

#include "arm_syms.h"

static ulong init_mm_addr;
module_param(init_mm_addr, ulong, 0444);
MODULE_PARM_DESC(init_mm_addr, "address of init_mm, for a kernel without KALLSYMS_ALL (from System.map)");

static struct mm_struct *p_init_mm;

/* Mirror of struct fsr_info in arch/arm/mm/fault.c: the .fn pointer is the
 * first member, so &fsr_info[7] is the address of its fn slot. */
struct as_fsr_info {
	fault_fn_t fn;
	int sig;
	int code;
	const char *name;
};

/* FSR 7 on the 2-level (non-LPAE) MMU of the Cortex-A9 is "page translation
 * fault", where a present-PFN pte with its valid bits cleared lands. LPAE
 * renumbers the codes, so it is refused in as_init(). */
#define AS_FSR_PAGE_TRANSLATION 7

typedef void (*as_range_fn_t)(unsigned long, unsigned long);
typedef void (*as_set_pte_fn_t)(pte_t *, pte_t, unsigned int);

static as_range_fn_t p_flush_tlb;
static as_set_pte_fn_t p_set_pte_ext;
static fault_fn_t *p_fault_slot;

unsigned long as_lookup(const char *name)
{
	return kallsyms_lookup_name(name);
}

struct mm_struct *as_init_mm(void)
{
	return p_init_mm;
}

void as_flush_tlb_kernel(unsigned long start, unsigned long end)
{
	if (p_flush_tlb)
		p_flush_tlb(start, end);
}

void as_set_pte_ext(pte_t *ptep, pte_t pte)
{
	p_set_pte_ext(ptep, pte, 0);
}

fault_fn_t *as_fault_slot(void)
{
	return p_fault_slot;
}

/*
 * A wrong init_mm_addr= is refused before it is followed: the address must
 * be in the linear map, so must its pgd, and the pgd's L1 entry for
 * PAGE_OFFSET must be the one every other pgd copies from init_mm's at
 * pgd_alloc(), here the insmod process's own.
 */
static int as_check_init_mm(unsigned long a)
{
	struct mm_struct *mm = (struct mm_struct *)a;

	if (!IS_ALIGNED(a, sizeof(long)) || !virt_addr_valid((void *)a) ||
	    !virt_addr_valid((void *)(a + sizeof(*mm) - 1))) {
		pr_err("wl_mmio_trap: init_mm at 0x%lx is not a lowmem address; wrong init_mm_addr?\n",
		       a);
		return -EINVAL;
	}
	if (!virt_addr_valid(mm->pgd)) {
		pr_err("wl_mmio_trap: init_mm at 0x%lx has pgd %p, not a lowmem address; wrong init_mm_addr?\n",
		       a, mm->pgd);
		return -EINVAL;
	}
	if (pgd_val(*pgd_offset(mm, PAGE_OFFSET)) !=
	    pgd_val(*pgd_offset(current->active_mm, PAGE_OFFSET))) {
		pr_err("wl_mmio_trap: init_mm at 0x%lx has a pgd whose kernel half is not the one of every pgd; wrong init_mm_addr?\n",
		       a);
		return -EINVAL;
	}
	return 0;
}

int as_init(unsigned long fsr_info_fn7_hint)
{
	unsigned long a;
	int err;

	if (IS_ENABLED(CONFIG_ARM_LPAE)) {
		pr_err("wl_mmio_trap: built for a 2-level MMU; CONFIG_ARM_LPAE is on, where the fault codes differ\n");
		return -ENOTSUPP;
	}

	a = kallsyms_lookup_name("init_mm");
	if (a && init_mm_addr && a != init_mm_addr) {
		pr_err("wl_mmio_trap: init_mm_addr=0x%lx, kallsyms has init_mm at 0x%lx\n",
		       init_mm_addr, a);
		return -EINVAL;
	}
	if (!a)
		a = init_mm_addr;
	if (!a) {
		pr_err("wl_mmio_trap: init_mm is not resolvable without KALLSYMS_ALL; pass init_mm_addr= from System.map\n");
		return -ENOENT;
	}
	err = as_check_init_mm(a);
	if (err)
		return err;
	p_init_mm = (struct mm_struct *)a;

	a = kallsyms_lookup_name("flush_tlb_kernel_range");
	if (!a) {
		pr_err("wl_mmio_trap: flush_tlb_kernel_range not resolvable\n");
		return -ENOENT;
	}
	p_flush_tlb = (as_range_fn_t)a;

	/* The processor's set_pte_ext. set_pte_at() is not usable from a
	 * module here: it references __sync_icache_dcache, unexported, even
	 * though that branch is user-address only. cpu_v7_set_pte_ext is the
	 * V7 implementation; on a single-CPU build set_pte_ext() inlines a
	 * call to it, on MULTI_CPU it goes through `processor`, and either
	 * symbol would do, but resolving the concrete one by name works on
	 * both and matches how flush_tlb is handled above. */
	a = kallsyms_lookup_name("cpu_v7_set_pte_ext");
	if (!a) {
		pr_err("wl_mmio_trap: cpu_v7_set_pte_ext not resolvable; is this a CPU_V7 build?\n");
		return -ENOENT;
	}
	p_set_pte_ext = (as_set_pte_fn_t)a;

	if (fsr_info_fn7_hint) {
		p_fault_slot = (fault_fn_t *)fsr_info_fn7_hint;
	} else {
		struct as_fsr_info *fsr_info =
			(struct as_fsr_info *)kallsyms_lookup_name("fsr_info");

		if (!fsr_info) {
			pr_err("wl_mmio_trap: fsr_info is a static symbol; a kernel without KALLSYMS_ALL does not carry it. Pass fsr_info_fn7=&fsr_info[%d].fn, from System.map plus %zu bytes.\n",
			       AS_FSR_PAGE_TRANSLATION,
			       AS_FSR_PAGE_TRANSLATION * sizeof(*fsr_info));
			return -ENOENT;
		}
		p_fault_slot = &fsr_info[AS_FSR_PAGE_TRANSLATION].fn;
	}

	return 0;
}
