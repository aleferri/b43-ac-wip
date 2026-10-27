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
 * init_mm is reached through init_task, which IS exported, so it needs no
 * lookup.
 */
#ifndef ARM_SYMS_H
#define ARM_SYMS_H

#include <linux/types.h>

struct mm_struct;
struct pt_regs;

/* Resolve flush_tlb_kernel_range and the fault slot. fsr_info_fn7_hint, if
 * non-zero, is the address of fsr_info[7].fn, for a kernel without
 * KALLSYMS_ALL. Returns 0, or a negative errno naming what is missing in
 * the kernel log. */
int as_init(unsigned long fsr_info_fn7_hint);

struct mm_struct *as_init_mm(void);
void as_flush_tlb_kernel(unsigned long start, unsigned long end);

/* The address of fsr_info[7].fn, so the caller can swap it and put it back.
 * NULL if fsr_info was not resolved. */
typedef int (*fault_fn_t)(unsigned long addr, unsigned int fsr,
			  struct pt_regs *regs);
fault_fn_t *as_fault_slot(void);

unsigned long as_lookup(const char *name);

#endif /* ARM_SYMS_H */
