// SPDX-License-Identifier: GPL-2.0
/*
 * Target: kernel 4.4, ARMv7 (Cortex-A9), little-endian, A32.
 *
 * Two A32 encodings carry every form readl/writel and the byte/halfword
 * R_REG/W_REG accessors produce. Both have the condition in [31:28] and the
 * base register Rn in [19:16] and the data register Rt in [15:12]; U [23] is
 * the add/subtract direction of the immediate offset.
 *
 *   Load/store word and unsigned byte (A5.3):
 *     cond 010 P U B W L  Rn Rt            imm12
 *   here with P=1 (offset addressing) and W=0 (no write-back), which is what
 *   an "ldr r0, [rn, #imm]" is. B selects byte, L selects load.
 *
 *   Load/store halfword, signed byte, signed halfword, doubleword (A5.4,
 *   "extra load/store"):
 *     cond 000 P U I W L  Rn Rt  immH 1 S H 1  immL
 *   with the S/H pair naming the width and signedness and I the immediate
 *   form. Again P=1, W=0 for the offset form.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <asm/io.h>

#include "arm_mmio_emulate.h"

/* CPSR condition flags. */
#define PSR_N	(1u << 31)
#define PSR_Z	(1u << 30)
#define PSR_C	(1u << 29)
#define PSR_V	(1u << 28)

bool arm_cond_passes(u32 insn, u32 cpsr)
{
	u32 cond = insn >> 28;
	bool N = cpsr & PSR_N, Z = cpsr & PSR_Z, C = cpsr & PSR_C, V = cpsr & PSR_V;
	bool r;

	switch (cond >> 1) {
	case 0: r = Z; break;			/* EQ / NE */
	case 1: r = C; break;			/* CS / CC */
	case 2: r = N; break;			/* MI / PL */
	case 3: r = V; break;			/* VS / VC */
	case 4: r = C && !Z; break;		/* HI / LS */
	case 5: r = N == V; break;		/* GE / LT */
	case 6: r = !Z && (N == V); break;	/* GT / LE */
	default: return true;			/* AL (and the unconditional
						 * 0b1111, handled by the caller
						 * as not-a-simple-LS) */
	}
	return (cond & 1) ? !r : r;
}

enum arm_emu_status arm_mmio_emulate_one(struct arm_emu_regs *regs, u32 insn,
					 unsigned long fault_va,
					 void __iomem *access_va,
					 struct arm_emu_result *out)
{
	u32 cond = insn >> 28;
	u32 op1 = (insn >> 25) & 0x7;
	u32 Rn = (insn >> 16) & 0xf;
	u32 Rt = (insn >> 12) & 0xf;
	bool U = insn & (1u << 23);
	bool P = insn & (1u << 24);
	bool W = insn & (1u << 21);
	bool L = insn & (1u << 20);
	unsigned long base, ea;
	u32 off, val;

	memset(out, 0, sizeof(*out));

	/* 0b1111 is unconditional space: SIMD/clrex/pld, never a plain LS. */
	if (cond == 0xf)
		return ARM_MMIO_NOT_SIMPLE_LS;
	if (!arm_cond_passes(insn, regs->cpsr))
		return ARM_MMIO_COND_FALSE;

	/* Only the offset form: pre-indexed add (P=1) without write-back
	 * (W=0). Post-indexed (P=0) and pre-indexed-with-write-back (W=1)
	 * change the base, which readl/writel never do, and letting them
	 * through would corrupt Rn. */
	if (!P || W)
		return ARM_MMIO_NOT_SIMPLE_LS;
	/* pc as base or data is a literal/computed form, out of scope. */
	if (Rn == 15 || Rt == 15)
		return ARM_MMIO_NOT_SIMPLE_LS;

	base = regs->r[Rn];

	if (op1 == 0x2) {
		/* LDR/STR/LDRB/STRB, immediate (bit 25 == 0). bit 25 == 1 is
		 * the register-offset form, refused. */
		bool B = insn & (1u << 22);

		off = insn & 0xfff;
		out->width = B ? 1 : 4;
		out->sign_extend = false;
	} else if (op1 == 0x0) {
		/* Extra load/store. The offset is immediate only when bit 22
		 * is set; the "1SH1" selector sits in bits 7,6,5,4. */
		u32 sh;

		if (!(insn & (1u << 22)))	/* register offset form */
			return ARM_MMIO_NOT_SIMPLE_LS;
		if ((insn & 0x90) != 0x90)	/* not an extra-LS (bits 7,4) */
			return ARM_MMIO_NOT_SIMPLE_LS;
		sh = (insn >> 5) & 0x3;
		off = ((insn >> 4) & 0xf0) | (insn & 0xf);
		switch (sh) {
		case 0:				/* SWP/multiply space */
			return ARM_MMIO_NOT_SIMPLE_LS;
		case 1:				/* STRH / LDRH */
			out->width = 2;
			out->sign_extend = false;
			break;
		case 2:				/* LDRD / LDRSB */
			if (!L)			/* STRD */
				return ARM_MMIO_DOUBLEWORD;
			out->width = 1;		/* LDRSB */
			out->sign_extend = true;
			break;
		default:			/* 3: STRD / LDRSH */
			if (!L)			/* STRD */
				return ARM_MMIO_DOUBLEWORD;
			out->width = 2;		/* LDRSH */
			out->sign_extend = true;
			break;
		}
	} else {
		return ARM_MMIO_NOT_SIMPLE_LS;
	}

	ea = U ? base + off : base - off;
	if (ea != fault_va)
		return ARM_MMIO_ADDR_MISMATCH;

	out->rt = Rt;
	out->is_write = !L;

	if (out->is_write) {
		val = regs->r[Rt];
		switch (out->width) {
		case 1: __raw_writeb((u8)val, access_va); break;
		case 2: __raw_writew((u16)val, access_va); break;
		default: __raw_writel(val, access_va); break;
		}
		out->value = val;
	} else {
		switch (out->width) {
		case 1:
			val = __raw_readb(access_va);
			if (out->sign_extend)
				val = (u32)(s32)(s8)val;
			break;
		case 2:
			val = __raw_readw(access_va);
			if (out->sign_extend)
				val = (u32)(s32)(s16)val;
			break;
		default:
			val = __raw_readl(access_va);
			break;
		}
		regs->r[Rt] = val;
		out->value = val;
	}

	regs->r[15] += 4;	/* A32: next instruction */
	return ARM_MMIO_OK;
}
