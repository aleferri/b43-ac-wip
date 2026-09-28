/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Plain integer encoders for the handful of MIPS32 o32 instructions the
 * relocation trampolines need, plus the one decode predicate that decides
 * whether a word may be relocated at all.
 *
 * Pure arithmetic: the only kernel header it needs is linux/types.h, which
 * tools/shim/ supplies on the host, so tools/test_opcodes.c checks THIS
 * file -- not a copy of it -- against known-reference machine words before
 * any of it is trusted on target hardware.
 *
 * MIPS o32 register numbers:
 *   0 zero  1 at   2 v0   3 v1   4 a0   5 a1   6 a2   7 a3
 *   8 t0    9 t1  10 t2  11 t3  12 t4  13 t5  14 t6  15 t7
 *  16 s0   17 s1  18 s2  19 s3  20 s4  21 s5  22 s6  23 s7
 *  24 t8   25 t9  26 k0  27 k1  28 gp  29 sp  30 fp  31 ra
 */
#ifndef MIPS_OPCODE_H
#define MIPS_OPCODE_H

#include <linux/types.h>

#define MIPS_R_ZERO 0
#define MIPS_R_AT   1
#define MIPS_R_V0   2
#define MIPS_R_V1   3
#define MIPS_R_A0   4
#define MIPS_R_A1   5
#define MIPS_R_A2   6
#define MIPS_R_A3   7
#define MIPS_R_T0   8
#define MIPS_R_T9   25
#define MIPS_R_SP   29
#define MIPS_R_RA   31

/* Break code 515 on this architecture (BRK_KPROBE_BP in asm/break.h). The
 * value is passed in rather than hardcoded so the host test can check the
 * encoding without pulling in kernel headers. */
u32 mips_op_break(unsigned int code);

u32 mips_op_nop(void);
u32 mips_op_jr(unsigned int rs);
u32 mips_op_jalr(unsigned int rd, unsigned int rs);
u32 mips_op_move(unsigned int rd, unsigned int rs);	/* addu rd, rs, $zero */
u32 mips_op_addiu(unsigned int rt, unsigned int rs, s16 imm);
u32 mips_op_sw(unsigned int rt, unsigned int base, s16 off);
u32 mips_op_lw(unsigned int rt, unsigned int base, s16 off);
u32 mips_op_lui(unsigned int rt, u16 imm);
u32 mips_op_ori(unsigned int rt, unsigned int rs, u16 imm);
u32 mips_op_li_small(unsigned int rt, u16 imm);		/* ori rt, $zero, imm */

/* The 26-bit target field keeps the top 4 bits of the PC, so the encoded
 * word is only correct when executed from the same 256 MB region as
 * target_va. mips_op_j_ok() is that check; the caller must run it on the
 * address the word will be STORED at, not on the target. */
u32 mips_op_j(unsigned long target_va);
bool mips_op_j_ok(unsigned long from_va, unsigned long target_va);

/* Writes the lui/ori pair loading `val` into rt, at words[0] and words[1]. */
void mips_op_load32(u32 *words, unsigned int rt, u32 val);

/* True for every instruction whose execution depends on where it sits:
 * all branches and jumps. Such a word cannot be relocated into a
 * trampoline, so a prologue starting with one is not patchable. */
bool mips_insn_is_branch(u32 insn);

#endif /* MIPS_OPCODE_H */
