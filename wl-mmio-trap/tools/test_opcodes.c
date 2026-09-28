// SPDX-License-Identifier: GPL-2.0
/*
 * Checks mips_opcode.c against machine words taken from a disassembler,
 * on the host, before anything built from those encoders runs on a MIPS
 * core. Narrow but real: it confirms the bit arithmetic, nothing about
 * execution.
 */
#include <stdio.h>
#include "mips_opcode.h"

static int fails;

static void check(const char *name, u32 got, u32 want)
{
	int ok = (got == want);

	printf("%-30s got=0x%08x want=0x%08x %s\n", name, got, want,
	       ok ? "OK" : "MISMATCH");
	if (!ok)
		fails++;
}

static void check_bool(const char *name, bool got, bool want)
{
	int ok = (got == want);

	printf("%-30s got=%-5s want=%-5s %s\n", name,
	       got ? "true" : "false", want ? "true" : "false",
	       ok ? "OK" : "MISMATCH");
	if (!ok)
		fails++;
}

int main(void)
{
	u32 pair[2];

	check("nop", mips_op_nop(), 0x00000000u);
	check("jr $ra", mips_op_jr(MIPS_R_RA), 0x03e00008u);
	check("jalr $ra,$t9", mips_op_jalr(MIPS_R_RA, MIPS_R_T9), 0x0320f809u);
	check("lui $t9,0x1234", mips_op_lui(MIPS_R_T9, 0x1234), 0x3c191234u);
	check("move $a0,$ra", mips_op_move(MIPS_R_A0, MIPS_R_RA), 0x03e02021u);
	check("addiu $sp,$sp,-16", mips_op_addiu(MIPS_R_SP, MIPS_R_SP, -16), 0x27bdfff0u);
	check("addiu $sp,$sp,16", mips_op_addiu(MIPS_R_SP, MIPS_R_SP, 16), 0x27bd0010u);
	check("sw $a0,0($sp)", mips_op_sw(MIPS_R_A0, MIPS_R_SP, 0), 0xafa40000u);
	check("lw $a0,0($sp)", mips_op_lw(MIPS_R_A0, MIPS_R_SP, 0), 0x8fa40000u);
	check("ori $a0,$zero,7", mips_op_li_small(MIPS_R_A0, 7), 0x34040007u);

	mips_op_load32(pair, MIPS_R_T9, 0x80123456u);
	check("load32 hi(0x80123456)", pair[0], 0x3c198012u);
	check("load32 lo(0x80123456)", pair[1], 0x37393456u);

	/* `break 515` -- BRK_KPROBE_BP on this architecture. The code has to
	 * sit at bit 6 so do_bp()'s "shift right by 10 if it does not fit in
	 * 10 bits" heuristic gives it back unchanged. */
	check("break 515", mips_op_break(515), 0x000080cdu);
	check("j 0x80123456", mips_op_j(0x80123454u), 0x08048d15u);

	/* The jump keeps PC[31:28] of its delay slot. */
	check_bool("j_ok same region", mips_op_j_ok(0x80100000u, 0x80b9a000u), true);
	check_bool("j_ok across regions", mips_op_j_ok(0xc3e5b000u, 0x80b9a000u), false);

	/* Words that may not be relocated into a trampoline. */
	check_bool("is_branch jr $ra", mips_insn_is_branch(0x03e00008u), true);
	check_bool("is_branch jalr", mips_insn_is_branch(0x0320f809u), true);
	check_bool("is_branch j", mips_insn_is_branch(0x08048d15u), true);
	check_bool("is_branch jal", mips_insn_is_branch(0x0c048d15u), true);
	check_bool("is_branch beq", mips_insn_is_branch(0x10850003u), true);
	check_bool("is_branch bnel", mips_insn_is_branch(0x54850003u), true);
	check_bool("is_branch bgez", mips_insn_is_branch(0x04810003u), true);
	check_bool("is_branch bc1t", mips_insn_is_branch(0x45010003u), true);
	check_bool("is_branch addiu", mips_insn_is_branch(0x27bdffe0u), false);
	check_bool("is_branch lui", mips_insn_is_branch(0x3c1c0001u), false);
	check_bool("is_branch sw", mips_insn_is_branch(0xafbf001cu), false);
	check_bool("is_branch nop", mips_insn_is_branch(0x00000000u), false);
	check_bool("is_branch addu", mips_insn_is_branch(0x03e02021u), false);

	printf("\n%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
