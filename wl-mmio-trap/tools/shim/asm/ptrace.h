/* SPDX-License-Identifier: GPL-2.0 */
/* The three CP0 fields and the GPR array the emulator touches, in the same
 * order of use as the real MIPS struct pt_regs. Layout is irrelevant here:
 * nothing in the tested code depends on offsets. */
#ifndef SHIM_ASM_PTRACE_H
#define SHIM_ASM_PTRACE_H

#include <linux/types.h>

struct pt_regs {
	unsigned long regs[32];
	unsigned long cp0_epc;
	unsigned long cp0_cause;
	unsigned long cp0_badvaddr;
};

#define CAUSEF_BD (1UL << 31)

#endif
