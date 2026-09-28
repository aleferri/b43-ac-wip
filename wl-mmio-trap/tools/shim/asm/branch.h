/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SHIM_ASM_BRANCH_H
#define SHIM_ASM_BRANCH_H

#include <asm/ptrace.h>
#include <asm/inst.h>

static inline int delay_slot(struct pt_regs *regs)
{
	return regs->cp0_cause & CAUSEF_BD;
}

static inline unsigned long exception_epc(struct pt_regs *regs)
{
	if (!delay_slot(regs))
		return regs->cp0_epc;
	return regs->cp0_epc + 4;
}

/* Supplied by the test, which checks that it is called exactly when the
 * access sat in a delay slot and never otherwise. */
int __compute_return_epc_for_insn(struct pt_regs *regs,
				  union mips_instruction insn);

#endif
