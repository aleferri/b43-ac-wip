/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Decode-and-emulate exactly one trapped MIPS32 o32 load/store, in place of
 * hardware single-step (no arch_has_single_step() on this kernel, and
 * CP0_DEBUG.SSt is commonly unimplemented even on much newer cores).
 *
 * Contract with the caller:
 *   - fault_va is the address wl's own instruction tried to touch, i.e. the
 *     address inside wl's permanently-invalidated mapping.
 *   - access_va is the SAME physical location reached through this module's
 *     own alias, which stays valid. wl's mapping is never made valid again
 *     for the duration of an access: that is what keeps the other cpu from
 *     slipping an unseen access through the open window.
 *   - On MMIO_EMU_OK the bus access has happened (through __raw_read/write,
 *     so no byte swap or barrier is introduced beyond what wl's own
 *     lb/lh/lw/sb/sh/sw would have done) and regs has been advanced past
 *     the instruction, including the branch evaluation when the access sat
 *     in a delay slot.
 *   - On anything else NOTHING has been touched: no bus access, no GPR
 *     write, cp0_epc unmoved. The caller must let the fault surface.
 */
#ifndef MIPS_MMIO_EMULATE_H
#define MIPS_MMIO_EMULATE_H

#include <linux/types.h>
#include <asm/ptrace.h>

struct mmio_emu_result {
	u32 value;	/* value read (load) or written (store) */
	u8 width;	/* 1, 2 or 4 */
	u8 gpr;		/* rt: destination (load) or source (store) */
	bool is_write;
	bool delay_slot;
};

enum mmio_emu_status {
	MMIO_EMU_OK = 0,
	/* Not lb/lbu/lh/lhu/lw/sb/sh/sw. Either the address bookkeeping
	 * upstream is wrong or something that is not a plain register
	 * access hit the window. Needs a human, not a guess. */
	MMIO_EMU_NOT_LOADSTORE,
	/* lwl/lwr/swl/swr: recognised, not performed. The left/right merge
	 * is endian-mirrored and easy to get subtly wrong, and R_REG/W_REG
	 * -style pokes do not compile to these. Counted until a capture
	 * shows one firing. */
	MMIO_EMU_UNALIGNED_LR,
	/* In a delay slot behind a branch whose resume address this unit
	 * will not compute: a COP1 condition branch (needs FPU state that
	 * may not belong to the faulting context) or a DSP bposge32. */
	MMIO_EMU_DELAY_SLOT,
	/* regs[rs] + sign_extend(imm) != fault_va: the decode disagrees
	 * with the address that trapped. */
	MMIO_EMU_ADDR_MISMATCH,
};

enum mmio_emu_status mips_mmio_emulate_one(struct pt_regs *regs,
					   unsigned long fault_va,
					   void __iomem *access_va,
					   struct mmio_emu_result *out);

#endif /* MIPS_MMIO_EMULATE_H */
