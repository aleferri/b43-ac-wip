/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The kernel symbols the ARM trap needs that are not exported to modules.
 * Far fewer than the MIPS side: this variant hooks the data-abort dispatch
 * table rather than fixup_exception, and single-steps nothing, so no
 * __compute_return_epc; and kallsyms_lookup_name is exported from 2.6.33 on,
 * so klookup is not needed either.
 *
 *   - flush_tlb_kernel_range: on a uniprocessor build it is a static inline
 *     over cpu_tlb, but on the SMP Northstar it is an out-of-line function,
 *     and either way it is not exported;
 *   - fsr_info[7].fn: the data-abort handler table, a static array in
 *     arch/arm/mm/fault.c, reached by its symbol. Entry 7 is "page
 *     translation fault", where an invalidated-but-present-PFN pte lands.
 *
 * init_mm: by name, or init_mm_addr= from System.map (see arm_syms.c; it is
 * not init_task.active_mm, which the idle thread lends out).
 */
#ifndef ARM_SYMS_H
#define ARM_SYMS_H

#include <linux/types.h>
#include <asm/pgtable.h>	/* pte_t, for as_set_pte_ext */

struct mm_struct;
struct pt_regs;

/* Resolve flush_tlb_kernel_range and the fault slot. fsr_info_fn7_hint, if
 * non-zero, is the address of fsr_info[7].fn, for a kernel without
 * KALLSYMS_ALL. Returns 0, or a negative errno naming what is missing in
 * the kernel log. */
int as_init(unsigned long fsr_info_fn7_hint);

struct mm_struct *as_init_mm(void);
void as_flush_tlb_kernel(unsigned long start, unsigned long end);

/* Write a kernel pte through the processor's set_pte_ext, with ext 0. This
 * is what set_pte_ext() expands to, called directly so the module does not
 * pull set_pte_at()'s __sync_icache_dcache() -- that one is for user exec
 * pages and is not exported. On a CONFIG_MULTI_CPU kernel set_pte_ext()
 * goes through the exported `processor` struct, but the DD-WRT Northstar
 * build is single-CPU, where it inlines a call to the unexported
 * cpu_v7_set_pte_ext; as_init() resolves it by name either way. */
void as_set_pte_ext(pte_t *ptep, pte_t pte);

/* The address of fsr_info[7].fn, so the caller can swap it and put it back.
 * NULL if fsr_info was not resolved. */
typedef int (*fault_fn_t)(unsigned long addr, unsigned int fsr,
			  struct pt_regs *regs);
fault_fn_t *as_fault_slot(void);

unsigned long as_lookup(const char *name);

#endif /* ARM_SYMS_H */
