// SPDX-License-Identifier: GPL-2.0
#include "mips_opcode.h"

static u32 enc_r(u32 op, u32 rs, u32 rt, u32 rd, u32 sh, u32 fn)
{
	return ((op & 0x3f) << 26) | ((rs & 0x1f) << 21) | ((rt & 0x1f) << 16) |
	       ((rd & 0x1f) << 11) | ((sh & 0x1f) << 6) | (fn & 0x3f);
}

static u32 enc_i(u32 op, u32 rs, u32 rt, u32 imm16)
{
	return ((op & 0x3f) << 26) | ((rs & 0x1f) << 21) | ((rt & 0x1f) << 16) |
	       (imm16 & 0xffff);
}

u32 mips_op_break(unsigned int code)
{
	/* SPECIAL / code[19:0] / BREAK. do_bp() reads the code from bits
	 * 6..25 and shifts it right by 10 when it does not fit in 10 bits,
	 * so a code below 1024 must sit at bit 6 to come back unchanged. */
	return 0x0000000du | ((code & 0xfffff) << 6);
}

u32 mips_op_nop(void)
{
	return 0;
}

u32 mips_op_jr(unsigned int rs)
{
	return enc_r(0, rs, 0, 0, 0, 0x08);
}

u32 mips_op_jalr(unsigned int rd, unsigned int rs)
{
	return enc_r(0, rs, 0, rd, 0, 0x09);
}

u32 mips_op_move(unsigned int rd, unsigned int rs)
{
	return enc_r(0, rs, 0, rd, 0, 0x21);	/* addu rd, rs, $zero */
}

u32 mips_op_addiu(unsigned int rt, unsigned int rs, s16 imm)
{
	return enc_i(0x09, rs, rt, (u16)imm);
}

u32 mips_op_sw(unsigned int rt, unsigned int base, s16 off)
{
	return enc_i(0x2b, base, rt, (u16)off);
}

u32 mips_op_lw(unsigned int rt, unsigned int base, s16 off)
{
	return enc_i(0x23, base, rt, (u16)off);
}

u32 mips_op_lui(unsigned int rt, u16 imm)
{
	return enc_i(0x0f, 0, rt, imm);
}

u32 mips_op_ori(unsigned int rt, unsigned int rs, u16 imm)
{
	return enc_i(0x0d, rs, rt, imm);
}

u32 mips_op_li_small(unsigned int rt, u16 imm)
{
	return mips_op_ori(rt, MIPS_R_ZERO, imm);
}

u32 mips_op_j(unsigned long target_va)
{
	return (0x02u << 26) | (u32)((target_va >> 2) & 0x03ffffffUL);
}

bool mips_op_j_ok(unsigned long from_va, unsigned long target_va)
{
	/* The jump keeps PC[31:28] of the delay slot, i.e. of from_va + 4. */
	unsigned long pc = from_va + 4;

	return ((pc ^ target_va) & 0xf0000000UL) == 0;
}

void mips_op_load32(u32 *words, unsigned int rt, u32 val)
{
	words[0] = mips_op_lui(rt, (u16)(val >> 16));
	words[1] = mips_op_ori(rt, rt, (u16)(val & 0xffff));
}

bool mips_insn_is_branch(u32 insn)
{
	u32 op = insn >> 26;

	if (op == 0x00) {			/* SPECIAL: jr, jalr */
		u32 fn = insn & 0x3f;

		return fn == 0x08 || fn == 0x09;
	}
	if (op == 0x01)				/* REGIMM: bltz/bgez/bal/... */
		return true;
	if (op == 0x02 || op == 0x03)		/* j / jal */
		return true;
	if (op >= 0x04 && op <= 0x07)		/* beq/bne/blez/bgtz */
		return true;
	if (op >= 0x14 && op <= 0x17)		/* beql/bnel/blezl/bgtzl */
		return true;
	if (op == 0x11) {			/* COP1: bc1f/bc1t and likely */
		u32 rs = (insn >> 21) & 0x1f;

		return rs == 0x08;
	}
	return false;
}
