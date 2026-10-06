// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <asm/ptrace.h>
#include <asm/branch.h>
#include <asm/break.h>

#include "bp_hook.h"
#include "compat.h"
#include "mips_opcode.h"
#include "kern_syms.h"

#define BP_MAX_SITES 4
#define TRAMP_WORDS 3

struct bp_site {
	unsigned long addr;
	u32 orig;
	u32 *tramp;
	bp_fn_t fn;
	void *ctx;
	bool live;		/* the break word is in place */
};

static struct bp_site sites[BP_MAX_SITES];
static DEFINE_MUTEX(sites_lock);
static bool notifier_on;

static struct bp_site *find_site(unsigned long epc)
{
	unsigned int i;

	for (i = 0; i < BP_MAX_SITES; i++)
		if (sites[i].live && sites[i].addr == epc)
			return &sites[i];
	return NULL;
}

static int bp_notify(struct notifier_block *nb, unsigned long val, void *data)
{
	struct die_args *args = data;
	struct pt_regs *regs;
	struct bp_site *s;

	if (val != MMIO_BP_DIE_EVENT || !args || !args->regs)
		return NOTIFY_DONE;
	regs = args->regs;
	if (user_mode(regs))
		return NOTIFY_DONE;

	/* Our sites are always the first word of a function, never a delay
	 * slot, so cp0_epc is the break itself. If BD is set this break is
	 * somebody else's. */
	if (delay_slot(regs))
		return NOTIFY_DONE;

	s = find_site(regs->cp0_epc);
	if (!s)
		return NOTIFY_DONE;	/* another module's break site */

	if (s->fn(regs, s->ctx) == BP_PASS)
		regs->cp0_epc = (unsigned long)s->tramp;

	return NOTIFY_STOP;
}

static struct notifier_block bp_nb = {
	.notifier_call = bp_notify,
};

int bp_hook_init(void)
{
	int ret;

	if (notifier_on)
		return 0;
	ret = register_die_notifier(&bp_nb);
	if (ret)
		return ret;
	notifier_on = true;
	return 0;
}

void bp_hook_exit(void)
{
	unsigned int i;

	for (i = 0; i < BP_MAX_SITES; i++)
		bp_hook_remove(&sites[i]);

	if (notifier_on) {
		unregister_die_notifier(&bp_nb);
		notifier_on = false;
	}

	/* The trampolines outlive the break word on purpose: a cpu that took
	 * the break just before it was removed can still be on its way into
	 * one. They go only once the notifier is gone and both a handler
	 * grace period (rcu) and a scheduling one (sched, for the cpus that
	 * are running the trampoline code itself rather than the handler)
	 * have passed. */
	synchronize_rcu();
	synchronize_sched();
	for (i = 0; i < BP_MAX_SITES; i++) {
		kfree(sites[i].tramp);
		sites[i].tramp = NULL;
	}
}

struct bp_site *bp_hook_add(unsigned long addr, bp_fn_t fn, void *ctx)
{
	struct bp_site *s = NULL;
	u32 *tramp;
	unsigned int i;
	int err = -ENOSPC;

	if (!addr || (addr & 3) || !fn)
		return ERR_PTR(-EINVAL);
	if (!notifier_on)
		return ERR_PTR(-EAGAIN);

	mutex_lock(&sites_lock);
	for (i = 0; i < BP_MAX_SITES; i++) {
		if (sites[i].live && sites[i].addr == addr) {
			err = -EEXIST;
			goto out;
		}
		if (!sites[i].live && !s)
			s = &sites[i];
	}
	if (!s)
		goto out;

	s->orig = *(volatile u32 *)addr;
	if (mips_insn_is_branch(s->orig)) {
		pr_err("wl_mmio_trap: %p starts with a branch (%08x): not patchable\n",
		       (void *)addr, s->orig);
		err = -EOPNOTSUPP;
		goto out;
	}

	/* kfree'd only in bp_hook_exit(); see there. Reused across
	 * add/remove cycles on the same slot only after the size check
	 * below, which is constant, so a stale allocation is fine. */
	tramp = s->tramp;
	if (!tramp) {
		tramp = kmalloc(TRAMP_WORDS * sizeof(u32), GFP_KERNEL);
		if (!tramp) {
			err = -ENOMEM;
			goto out;
		}
		s->tramp = tramp;
	}

	if (!mips_op_j_ok((unsigned long)&tramp[1], addr + 4)) {
		pr_err("wl_mmio_trap: trampoline %p and target %p are in different 256MB regions\n",
		       tramp, (void *)(addr + 4));
		err = -ERANGE;
		goto out;
	}

	tramp[0] = s->orig;
	tramp[1] = mips_op_j(addr + 4);
	tramp[2] = mips_op_nop();
	ks_flush_icache((unsigned long)tramp,
			(unsigned long)tramp + TRAMP_WORDS * sizeof(u32));

	s->addr = addr;
	s->fn = fn;
	s->ctx = ctx;
	smp_wmb();
	s->live = true;

	/* Single aligned store: the target reads either the original word or
	 * the break, never anything in between. */
	*(volatile u32 *)addr = mips_op_break(BRK_KPROBE_BP);
	ks_flush_icache(addr, addr + 4);

	mutex_unlock(&sites_lock);
	pr_info("wl_mmio_trap: breakpoint armed at %p (orig %08x, trampoline %p)\n",
		(void *)addr, s->orig, tramp);
	return s;

out:
	mutex_unlock(&sites_lock);
	return ERR_PTR(err);
}

void bp_hook_remove(struct bp_site *s)
{
	if (!s)
		return;

	mutex_lock(&sites_lock);
	if (!s->live) {
		mutex_unlock(&sites_lock);
		return;
	}

	*(volatile u32 *)s->addr = s->orig;
	ks_flush_icache(s->addr, s->addr + 4);
	s->live = false;
	mutex_unlock(&sites_lock);

	/* A cpu that already took the break is inside bp_notify(). The die
	 * chain is an atomic notifier, so handlers run under rcu_read_lock()
	 * and synchronize_rcu() is what waits for them -- synchronize_sched()
	 * would not, on a PREEMPT_RCU kernel. */
	synchronize_rcu();
}

unsigned long bp_hook_addr(const struct bp_site *s)
{
	return s ? s->addr : 0;
}
