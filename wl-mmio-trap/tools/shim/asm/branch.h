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

#endif
