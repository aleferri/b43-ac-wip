/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The four kernel facilities this module needs that a 3.4 MIPS build does
 * not export to modules:
 *
 *   init_mm                 a data symbol, so invisible to a kallsyms built
 *                           without KALLSYMS_ALL, and not exported on 3.4.
 *                           init_task.active_mm is NOT a way to reach it:
 *                           INIT_TASK sets it to &init_mm, but
 *                           context_switch() overwrites it with the mm the
 *                           idle task borrows from the last user process
 *                           (and clears it to NULL on the way out). It is
 *                           found instead by its static initialiser's
 *                           signature, scanned for around init_task. Every
 *                           candidate, however it was arrived at, is
 *                           CHECKED against vmalloc_to_pfn() before being
 *                           adopted: walking the wrong page table does not
 *                           fail, it silently returns a plausible wrong
 *                           pfn.
 *   flush_icache_range      a function POINTER in .bss on MIPS
 *                           (arch/mips/mm/cache.c), so also invisible; the
 *                           R4K implementation behind it is a text symbol
 *                           and is what gets resolved here, same route
 *                           wl_diag.c already takes.
 *   flush_tlb_kernel_range  text, SMP-only, not exported.
 *   fixup_exception         text, not exported; the address the D11 trap
 *                           plants its breakpoint on.
 *
 * ks_init() resolves all of them and fails if any is missing, so every
 * later caller can assume they are there.
 */
#ifndef KERN_SYMS_H
#define KERN_SYMS_H

#include <linux/types.h>

struct mm_struct;

/* init_mm_hint: the address of init_mm, from System.map, when neither
 * kallsyms nor the runtime candidates can supply it. 0 to leave it out. */
int ks_init(unsigned long init_mm_hint);

struct mm_struct *ks_init_mm(void);
unsigned long ks_fixup_exception(void);

void ks_flush_icache(unsigned long start, unsigned long end);
void ks_flush_tlb_kernel(unsigned long start, unsigned long end);

/* For the optional, name-resolved extras; returns 0 when not found. */
unsigned long ks_lookup(const char *name);

#endif /* KERN_SYMS_H */
