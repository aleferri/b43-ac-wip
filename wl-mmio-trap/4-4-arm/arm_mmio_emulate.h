/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Decode-and-emulate exactly one trapped ARM load/store, in place of
 * hardware single-step. The MIPS variant's contract, verbatim:
 *
 *   - fault_va is the address wl's own instruction tried to touch, inside
 *     wl's permanently-invalidated mapping.
 *   - access_va is the SAME physical location reached through this module's
 *     own alias, which stays valid; wl's mapping is never made valid again
 *     for the duration of an access, so another cpu cannot slip an unseen
 *     access through an open window.
 *   - On ARM_MMIO_OK the bus access has happened through __raw_read/write,
 *     so no byte swap or barrier beyond wl's own ldr/str, and regs has been
 *     advanced past the instruction (pc += 4, A32 only).
 *   - On anything else NOTHING has been touched: no bus access, no register
 *     write, pc unmoved. The caller lets the fault surface.
 *
 * Scope: the A32 single-register forms that readl/writel and the R_REG /
 * W_REG byte and halfword accessors compile to -- LDR/STR/LDRB/STRB and
 * LDRH/STRH/LDRSB/LDRSH with an immediate offset, no write-back, pc
 * neither base nor data. Everything else is refused with a distinct status
 * and counted; a register window is touched only by these forms, so a
 * refusal means the address bookkeeping is wrong or something that is not a
 * plain MMIO access faulted, which wants a human.
 *
 * Thumb (CONFIG_THUMB2_KERNEL) is out: the Northstar wl is A32. A Thumb
 * faulting instruction is refused by status, not mis-decoded, so the
 * module can say so instead of guessing.
 */
#ifndef ARM_MMIO_EMULATE_H
#define ARM_MMIO_EMULATE_H

#include <linux/types.h>

struct arm_emu_regs {
	u32 r[16];	/* r0..r15; r15 is the pc of the faulting insn */
	u32 cpsr;
};

struct arm_emu_result {
	u32 value;	/* value read (load) or written (store) */
	u8 width;	/* 1, 2 or 4 */
	u8 rt;		/* destination (load) or source (store) register */
	bool is_write;
	bool sign_extend;	/* LDRSB/LDRSH */
};

enum arm_emu_status {
	ARM_MMIO_OK = 0,
	/* Not one of the handled single-register immediate-offset forms:
	 * register offset, write-back, pc operand, a block transfer, a
	 * coprocessor or SIMD access, or a Thumb instruction. */
	ARM_MMIO_NOT_SIMPLE_LS,
	/* LDRD/STRD: two registers at once. R_REG/W_REG do not emit them;
	 * counted until a capture shows one. */
	ARM_MMIO_DOUBLEWORD,
	/* The condition code failed: the instruction would not have
	 * executed, so there is nothing to emulate and the fault should not
	 * have happened on our window. Counted, refused. */
	ARM_MMIO_COND_FALSE,
	/* base + offset != fault_va: the decode disagrees with the address
	 * that trapped. */
	ARM_MMIO_ADDR_MISMATCH,
};

/*
 * fetch is read from regs->r[15] by the caller and passed in, so this unit
 * touches no memory and the host test can drive it with literal words.
 */
enum arm_emu_status arm_mmio_emulate_one(struct arm_emu_regs *regs, u32 insn,
					 unsigned long fault_va,
					 void __iomem *access_va,
					 struct arm_emu_result *out);

/* Whether @insn's condition field is satisfied by @cpsr. Exposed for the
 * host test; arm_mmio_emulate_one() applies it itself. */
bool arm_cond_passes(u32 insn, u32 cpsr);

#endif /* ARM_MMIO_EMULATE_H */
