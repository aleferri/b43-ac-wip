/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One-word `break BRK_KPROBE_BP` sites in kernel text, dispatched from a
 * single die notifier.
 *
 * Why a break and not a jump detour: do_bp() calls
 * notify_die(DIE_BREAK, ...) unconditionally on this architecture -- it is
 * NOT inside the CONFIG_KPROBES guard the way do_page_fault()'s
 * DIE_PAGE_FAULT is -- and NOTIFY_STOP makes it return without signalling.
 * So a break is the one instrumentation point on a CONFIG_KPROBES=n 3.4
 * MIPS kernel from which execution can be resumed at an address the
 * handler chooses. wl_diag.c already relies on exactly this for the
 * prologues it cannot detour.
 *
 * A one-word patch also removes the whole multi-word patch-ordering
 * problem: a single aligned 32-bit store has no intermediate state, so a
 * concurrent caller either sees the original word or the break, never
 * half a prologue. Nothing here needs stop_machine().
 *
 * The original word is relocated into a 3-word trampoline
 * (orig ; j addr+4 ; nop) so a handler that decides the event is not its
 * business can resume the target unchanged. That costs one restriction:
 * the word being replaced must not be a branch, because a relocated
 * branch resolves against the wrong PC. bp_hook_add() refuses those.
 */
#ifndef BP_HOOK_H
#define BP_HOOK_H

#include <linux/types.h>

struct pt_regs;
struct bp_site;

enum bp_action {
	/* The handler has set regs->cp0_epc itself; resume there. */
	BP_RESUMED,
	/* Not ours after all: run the displaced original word and continue
	 * into the target at addr+4. */
	BP_PASS,
};

typedef enum bp_action (*bp_fn_t)(struct pt_regs *regs, void *ctx);

int bp_hook_init(void);
void bp_hook_exit(void);

/* addr must be a 4-byte-aligned, currently-writable kernel text address.
 * ERR_PTR on failure (unpatchable prologue, no memory, no free slot). */
struct bp_site *bp_hook_add(unsigned long addr, bp_fn_t fn, void *ctx);

/* Restores the original word and waits for in-flight handlers before the
 * site can be reused. Safe to call on a NULL or already-removed site. */
void bp_hook_remove(struct bp_site *s);

unsigned long bp_hook_addr(const struct bp_site *s);

#endif /* BP_HOOK_H */
