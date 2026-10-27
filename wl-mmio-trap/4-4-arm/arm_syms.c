// SPDX-License-Identifier: GPL-2.0
/*
 * Resolve the kernel symbols the ARM trap needs.
 *
 * flush_tlb_kernel_range and fsr_info come by name through
 * kallsyms_lookup_name, which ARM exports to modules from 2.6.33 on. init_mm
 * is reached through the exported init_task, so it needs no lookup and no
 * signature scan -- unlike the MIPS side, where init_mm had to be found by
 * other means.
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

#include "arm_syms.h"

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
	/* init_task.active_mm is &init_mm; init_task is exported. */
	return init_task.active_mm;
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

int as_init(unsigned long fsr_info_fn7_hint)
{
	unsigned long a;

	if (IS_ENABLED(CONFIG_ARM_LPAE)) {
		pr_err("wl_mmio_trap: built for a 2-level MMU; CONFIG_ARM_LPAE is on, where the fault codes differ\n");
		return -ENOTSUPP;
	}

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
