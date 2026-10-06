// SPDX-License-Identifier: GPL-2.0
/*
 * Drives mips_mmio_emulate.c on the host against a synthetic instruction
 * stream and a synthetic "device" buffer.
 *
 * What this can check: the decode (opcode, rs, rt, immediate), the
 * effective-address agreement, the widths, the sign extension, that $0
 * discards, that a store takes its value from rt, that cp0_epc advances by
 * one instruction on the straight path and NOT on the delay-slot path
 * (where the branch evaluator owns the resume address), and that every
 * refusal leaves regs and the device untouched.
 *
 * What it cannot check: anything about running under a real exception, and
 * the byte order seen by __raw_* on a big-endian target.
 */
#include <stdio.h>
#include <string.h>

#include <asm/branch.h>
#include "kern_syms.h"
#include "mips_mmio_emulate.h"

static int fails;
static int compute_calls;
static unsigned long compute_target = 0xdead0000;

int ks_compute_return_epc(struct pt_regs *regs, u32 insn)
{
	compute_calls++;
	if ((insn >> 26) == 0x3f)	/* the test's "uncomputable" marker */
		return -14;
	regs->cp0_epc = compute_target;
	return 0;
}

static void fail(const char *what)
{
	printf("  FAIL: %s\n", what);
	fails++;
}

#define EXPECT(cond, what) do { if (!(cond)) fail(what); } while (0)

/* I-type encoder, independent of mips_opcode.c so a bug there cannot hide
 * a bug here. */
static u32 itype(u32 op, u32 rs, u32 rt, s16 imm)
{
	return (op << 26) | (rs << 21) | (rt << 16) | ((u32)(u16)imm);
}

#define OP_LB 0x20u
#define OP_LH 0x21u
#define OP_LWL 0x22u
#define OP_LW 0x23u
#define OP_LBU 0x24u
#define OP_LHU 0x25u
#define OP_SB 0x28u
#define OP_SH 0x29u
#define OP_SW 0x2bu

struct bench {
	struct pt_regs regs;
	u32 text[4];
	unsigned char dev[16];
	unsigned long fault_va;
};

/* base_reg holds a "window" address that is deliberately NOT the address
 * of the device buffer: the emulator must compare the decoded effective
 * address against fault_va, and then access through the alias the caller
 * hands it, never through the decoded address. */
#define WIN_BASE 0x40000000UL

static void setup(struct bench *b, u32 insn, u32 rs_val, s16 imm, bool bd)
{
	memset(b, 0, sizeof(*b));
	b->text[0] = bd ? itype(0x05, 4, 5, 3) : insn;	/* bne as the branch */
	b->text[1] = insn;
	b->regs.cp0_epc = (unsigned long)&b->text[0];
	b->regs.cp0_cause = bd ? CAUSEF_BD : 0;
	b->regs.regs[8] = rs_val;			/* $t0 is the base */
	b->fault_va = rs_val + (unsigned long)(long)imm;
	if (!bd)
		b->text[0] = insn;
}

static void case_load(const char *name, u32 op, s16 imm, u32 devval,
		      unsigned long want_rt, u8 want_width)
{
	struct bench b;
	struct mmio_emu_result res;
	enum mmio_emu_status st;
	u32 insn = itype(op, 8, 2, imm);		/* rt = $v0 */

	printf("%s\n", name);
	setup(&b, insn, WIN_BASE, imm, false);
	memcpy(b.dev, &devval, sizeof(devval));

	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)(b.dev + (imm & 0xf)), &res);

	EXPECT(st == MMIO_EMU_OK, "status");
	EXPECT(res.width == want_width, "width");
	EXPECT(!res.is_write, "direction");
	EXPECT(b.regs.regs[2] == want_rt, "value in rt");
	EXPECT(b.regs.cp0_epc == (unsigned long)&b.text[1], "epc advanced by one");
}

int main(void)
{
	struct bench b;
	struct mmio_emu_result res;
	enum mmio_emu_status st;
	u32 v;

	/* Word load: the whole 32 bits, epc += 4. */
	case_load("lw", OP_LW, 0, 0x12345678u, 0x12345678u, 4);

	/* Half and byte loads, signed and unsigned. The device buffer holds
	 * the bytes; what the test asserts is the width and the sign
	 * extension, both of which are byte-order independent because the
	 * value is read back through the same accessor. */
	{
		u16 h = 0x8001;
		struct bench bb;
		u32 insn = itype(OP_LHU, 8, 2, 0);

		printf("lhu / lh sign extension\n");
		setup(&bb, insn, WIN_BASE, 0, false);
		memcpy(bb.dev, &h, sizeof(h));
		st = mips_mmio_emulate_one(&bb.regs, bb.fault_va,
					   (void __iomem *)bb.dev, &res);
		EXPECT(st == MMIO_EMU_OK, "lhu status");
		EXPECT(bb.regs.regs[2] == 0x8001u, "lhu zero-extends");

		insn = itype(OP_LH, 8, 2, 0);
		setup(&bb, insn, WIN_BASE, 0, false);
		memcpy(bb.dev, &h, sizeof(h));
		st = mips_mmio_emulate_one(&bb.regs, bb.fault_va,
					   (void __iomem *)bb.dev, &res);
		EXPECT(st == MMIO_EMU_OK, "lh status");
		EXPECT(bb.regs.regs[2] == (unsigned long)(long)(int32_t)0xffff8001u,
		       "lh sign-extends");
	}
	{
		u8 by = 0x80;
		struct bench bb;

		printf("lbu / lb sign extension\n");
		setup(&bb, itype(OP_LBU, 8, 2, 0), WIN_BASE, 0, false);
		bb.dev[0] = by;
		st = mips_mmio_emulate_one(&bb.regs, bb.fault_va,
					   (void __iomem *)bb.dev, &res);
		EXPECT(st == MMIO_EMU_OK, "lbu status");
		EXPECT(bb.regs.regs[2] == 0x80u, "lbu zero-extends");

		setup(&bb, itype(OP_LB, 8, 2, 0), WIN_BASE, 0, false);
		bb.dev[0] = by;
		st = mips_mmio_emulate_one(&bb.regs, bb.fault_va,
					   (void __iomem *)bb.dev, &res);
		EXPECT(st == MMIO_EMU_OK, "lb status");
		EXPECT(bb.regs.regs[2] == (unsigned long)(long)(int32_t)0xffffff80u,
		       "lb sign-extends");
	}

	/* A load into $0 is legal and discards. */
	printf("lw into $zero\n");
	setup(&b, itype(OP_LW, 8, 0, 0), WIN_BASE, 0, false);
	v = 0xaabbccddu;
	memcpy(b.dev, &v, sizeof(v));
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	EXPECT(st == MMIO_EMU_OK, "status");
	EXPECT(b.regs.regs[0] == 0, "$zero stays zero");

	/* Stores take the value from rt and reach the device. */
	printf("sw / sh / sb\n");
	setup(&b, itype(OP_SW, 8, 3, 0), WIN_BASE, 0, false);
	b.regs.regs[3] = 0xcafebabeu;
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	memcpy(&v, b.dev, sizeof(v));
	EXPECT(st == MMIO_EMU_OK, "sw status");
	EXPECT(res.is_write && res.width == 4, "sw direction/width");
	EXPECT(v == 0xcafebabeu, "sw reached the device");
	EXPECT(res.value == 0xcafebabeu, "sw reported value");

	setup(&b, itype(OP_SH, 8, 3, 0), WIN_BASE, 0, false);
	b.regs.regs[3] = 0x11112222u;
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	EXPECT(st == MMIO_EMU_OK && res.width == 2, "sh width");
	{
		u16 h;

		memcpy(&h, b.dev, sizeof(h));
		EXPECT(h == 0x2222u, "sh wrote the low half only");
	}

	setup(&b, itype(OP_SB, 8, 3, 0), WIN_BASE, 0, false);
	b.regs.regs[3] = 0x33333344u;
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	EXPECT(st == MMIO_EMU_OK && res.width == 1, "sb width");
	EXPECT(b.dev[0] == 0x44, "sb wrote the low byte only");

	/* A negative immediate has to sign-extend into the effective
	 * address, or the agreement check would reject every access below
	 * the base register. */
	printf("negative immediate\n");
	setup(&b, itype(OP_LW, 8, 2, -8), WIN_BASE, -8, false);
	EXPECT(b.fault_va == WIN_BASE - 8, "test's own address arithmetic");
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	EXPECT(st == MMIO_EMU_OK, "status");

	/* Refusals: nothing may move. */
	printf("refusals leave everything untouched\n");
	{
		unsigned long epc0;

		setup(&b, itype(OP_LWL, 8, 2, 0), WIN_BASE, 0, false);
		epc0 = b.regs.cp0_epc;
		b.dev[0] = 0x5a;
		st = mips_mmio_emulate_one(&b.regs, b.fault_va,
					   (void __iomem *)b.dev, &res);
		EXPECT(st == MMIO_EMU_UNALIGNED_LR, "lwl refused");
		EXPECT(b.regs.cp0_epc == epc0, "lwl left epc alone");
		EXPECT(b.regs.regs[2] == 0, "lwl left rt alone");

		setup(&b, 0x00000000u, WIN_BASE, 0, false);	/* nop */
		epc0 = b.regs.cp0_epc;
		st = mips_mmio_emulate_one(&b.regs, b.fault_va,
					   (void __iomem *)b.dev, &res);
		EXPECT(st == MMIO_EMU_NOT_LOADSTORE, "nop refused");
		EXPECT(b.regs.cp0_epc == epc0, "nop left epc alone");

		setup(&b, itype(OP_LW, 8, 2, 0), WIN_BASE, 0, false);
		epc0 = b.regs.cp0_epc;
		st = mips_mmio_emulate_one(&b.regs, b.fault_va + 4,
					   (void __iomem *)b.dev, &res);
		EXPECT(st == MMIO_EMU_ADDR_MISMATCH, "mismatch refused");
		EXPECT(b.regs.cp0_epc == epc0, "mismatch left epc alone");
	}

	/* Delay slot: the branch evaluator owns the resume address, so epc
	 * must come from it and not from a += 4. */
	printf("delay slot\n");
	compute_calls = 0;
	setup(&b, itype(OP_LW, 8, 2, 0), WIN_BASE, 0, true);
	v = 0x0f0f0f0fu;
	memcpy(b.dev, &v, sizeof(v));
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	EXPECT(st == MMIO_EMU_OK, "status");
	EXPECT(res.delay_slot, "reported as a delay slot");
	EXPECT(compute_calls == 1, "branch evaluator called once");
	EXPECT(b.regs.cp0_epc == compute_target, "epc came from the evaluator");
	EXPECT(b.regs.regs[2] == 0x0f0f0f0fu, "value still loaded");

	/* A COP1 condition branch is refused before anything happens: it
	 * would need FPU state that does not belong to the faulting
	 * context. */
	printf("delay slot behind bc1t\n");
	compute_calls = 0;
	setup(&b, itype(OP_LW, 8, 2, 0), WIN_BASE, 0, true);
	b.text[0] = 0x45010003u;			/* bc1t */
	b.regs.regs[2] = 0xa5a5a5a5u;
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	EXPECT(st == MMIO_EMU_DELAY_SLOT, "bc1t refused");
	EXPECT(compute_calls == 0, "branch evaluator not called");
	EXPECT(b.regs.regs[2] == 0xa5a5a5a5u, "rt untouched");

	/* And an evaluator that itself refuses must not leave a half-done
	 * access behind either. */
	printf("delay slot the evaluator refuses\n");
	compute_calls = 0;
	setup(&b, itype(OP_SW, 8, 3, 0), WIN_BASE, 0, true);
	b.text[0] = 0xfc000000u;			/* op 0x3f marker */
	b.regs.regs[3] = 0x99999999u;
	memset(b.dev, 0, sizeof(b.dev));
	st = mips_mmio_emulate_one(&b.regs, b.fault_va,
				   (void __iomem *)b.dev, &res);
	memcpy(&v, b.dev, sizeof(v));
	EXPECT(st == MMIO_EMU_DELAY_SLOT, "refused");
	EXPECT(v == 0, "no store reached the device");

	printf("\n%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
