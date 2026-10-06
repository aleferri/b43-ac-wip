/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel API differences between the two targets, 2.6.30 (DSL-3580L) and
 * 3.4. The rest of the module is written against one API; the version tests
 * are here, and in the few places where the mechanism differs rather than
 * the name (kern_syms.c, bp_hook.c).
 */
#ifndef WL_MMIO_COMPAT_H
#define WL_MMIO_COMPAT_H

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <asm/break.h>

#ifndef pr_warn
#define pr_warn pr_warning
#endif

/* kallsyms_lookup_name() is exported to modules from 2.6.33; before that it
 * is reachable only by address (klookup=). */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
#define MMIO_KALLSYMS_EXPORTED
#endif

/* __compute_return_epc_for_insn() is exported from 3.3. Before that only
 * __compute_return_epc() exists, not exported, and it reads the branch at
 * cp0_epc itself. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
#define MMIO_RETURN_EPC_FOR_INSN
#endif

/* A kernel break reaches the die chain as DIE_BREAK from 2.6.36 on. Before
 * that do_bp() gets there only through do_trap_or_bp(), as DIE_TRAP. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
#define MMIO_BP_DIE_EVENT	DIE_BREAK
#else
#define MMIO_BP_DIE_EVENT	DIE_TRAP
#endif

#ifndef BRK_KPROBE_BP
#define BRK_KPROBE_BP		515
#endif

/* Before 2.6.33 raw_spinlock_t is the arch lock, and spinlock_t is already
 * a spinning lock on a kernel without PREEMPT_RT. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#define DEFINE_RAW_SPINLOCK(x)			DEFINE_SPINLOCK(x)
#define raw_spin_lock_irqsave(l, f)		spin_lock_irqsave(l, f)
#define raw_spin_unlock_irqrestore(l, f)	spin_unlock_irqrestore(l, f)
#endif

/* cpu_clock() is exported on 2.6.30; sched_clock() on 3.4. */
static inline u64 mmio_now_ns(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 4, 0)
	return sched_clock();
#else
	return cpu_clock(raw_smp_processor_id());
#endif
}

#endif /* WL_MMIO_COMPAT_H */
