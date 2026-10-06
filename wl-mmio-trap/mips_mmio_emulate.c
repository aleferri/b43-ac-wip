// SPDX-License-Identifier: GPL-2.0
/*
 * Target: kernels 2.6.30 and 3.4.x, MIPS32R1, big-endian, o32.
 *
 * MIPS32 I-type layout:
 *
 *   31      26 25    21 20    16 15                            0
 *  |  opcode  |   rs   |   rt   |            immediate           |
 *
 * effective address = regs[rs] + sign_extend(immediate)
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include <asm/branch.h>
#include <asm/inst.h>

#include "kern_syms.h"
#include "mips_mmio_emulate.h"

#define OP_LB	0x20
#define OP_LH	0x21
#define OP_LWL	0x22
#define OP_LW	0x23
#define OP_LBU	0x24
#define OP_LHU	0x25
#define OP_LWR	0x26
#define OP_SB	0x28
#define OP_SH	0x29
#define OP_SWL	0x2a
#define OP_SW	0x2b
#define OP_SWR	0x2e

static inline u32 fetch_insn(unsigned long addr)
{
	/* Kernel or module text, never the trapped window: a plain load,
	 * with no re-entrant fault to worry about. */
	return *(const u32 *)addr;
}

static inline unsigned long gpr_get(const struct pt_regs *regs, unsigned int n)
{
	return n ? regs->regs[n] : 0;
}

static inline void gpr_put(struct pt_regs *regs, unsigned int n, unsigned long v)
{
	if (n)		/* $0 is hardwired; a write through it is discarded */
		regs->regs[n] = v;
}

/* The kernel's branch evaluator handles every branch on this
 * architecture, but two families pull in state that does not belong to the
 * faulting context: the COP1 condition branches read fcr31 out of the
 * current thread's FPU state, and bposge32 needs the DSP ASE (it calls
 * force_sig() when the ASE is absent). Neither can come out of an
 * R_REG/W_REG sequence, so they are refused up front rather than handled. */
static bool branch_is_computable(u32 binsn)
{
	u32 op = binsn >> 26;
	u32 rt = (binsn >> 16) & 0x1f;

	if (op == 0x11)				/* COP1: bc1f/bc1t/... */
		return false;
	if (op == 0x01 && rt == 0x1c)		/* REGIMM bposge32 */
		return false;
	return true;
}

enum mmio_emu_status mips_mmio_emulate_one(struct pt_regs *regs,
					   unsigned long fault_va,
					   void __iomem *access_va,
					   struct mmio_emu_result *out)
{
	bool bd = delay_slot(regs) != 0;
	unsigned long ls_pc = exception_epc(regs);
	union mips_instruction binsn;
	unsigned long ea;
	u32 insn, opcode, rs, rt;
	s16 imm;
	bool is_write;
	u8 width;
	u32 val = 0;

	insn = fetch_insn(ls_pc);
	opcode = insn >> 26;
	rs = (insn >> 21) & 0x1f;
	rt = (insn >> 16) & 0x1f;
	imm = (s16)(insn & 0xffff);

	switch (opcode) {
	case OP_LB: case OP_LBU: case OP_SB: width = 1; break;
	case OP_LH: case OP_LHU: case OP_SH: width = 2; break;
	case OP_LW: case OP_SW:              width = 4; break;
	case OP_LWL: case OP_LWR:
	case OP_SWL: case OP_SWR:
		return MMIO_EMU_UNALIGNED_LR;
	default:
		return MMIO_EMU_NOT_LOADSTORE;
	}
	is_write = (opcode == OP_SB || opcode == OP_SH || opcode == OP_SW);

	ea = gpr_get(regs, rs) + (unsigned long)(long)imm;
	if (ea != fault_va)
		return MMIO_EMU_ADDR_MISMATCH;

	binsn.word = 0;
	if (bd) {
		binsn.word = fetch_insn(regs->cp0_epc);
		if (!branch_is_computable(binsn.word))
			return MMIO_EMU_DELAY_SLOT;
	}

	/* Resume address first, access second. A linking branch writes $ra
	 * when it executes, before its delay slot, so computing it after a
	 * delay-slot load into $ra would clobber the loaded value; this
	 * order reproduces the hardware's. It also means nothing has been
	 * written to the bus if the branch turns out to be uncomputable. */
	if (bd) {
		if (ks_compute_return_epc(regs, binsn.word) < 0)
			return MMIO_EMU_DELAY_SLOT;
	} else {
		regs->cp0_epc += 4;
	}

	if (is_write) {
		val = (u32)gpr_get(regs, rt);
		switch (width) {
		case 1: __raw_writeb((u8)val, access_va); break;
		case 2: __raw_writew((u16)val, access_va); break;
		case 4: __raw_writel(val, access_va); break;
		}
	} else {
		switch (width) {
		case 1:
			val = __raw_readb(access_va);
			if (opcode == OP_LB)
				val = (u32)(s32)(s8)val;
			break;
		case 2:
			val = __raw_readw(access_va);
			if (opcode == OP_LH)
				val = (u32)(s32)(s16)val;
			break;
		case 4:
			val = __raw_readl(access_va);
			break;
		}
		gpr_put(regs, rt, (unsigned long)(s32)val);
	}

	out->value = val;
	out->width = width;
	out->gpr = (u8)rt;
	out->is_write = is_write;
	out->delay_slot = bd;
	return MMIO_EMU_OK;
}
