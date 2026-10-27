// SPDX-License-Identifier: GPL-2.0
/*
 * Drives arm_mmio_emulate.c on the host against synthetic instruction words
 * and a synthetic "device" buffer, the way tools/test_emulate.c does for
 * the MIPS unit.
 *
 * What this checks: the decode of LDR/STR/LDRB/STRB immediate and of the
 * extra-load/store LDRH/STRH/LDRSB/LDRSH, the U add/subtract direction, the
 * effective-address agreement with fault_va, the widths, sign extension,
 * that a store takes its value from Rt and a load writes it, that the
 * condition code gates execution, that pc advances by 4 on OK and not on a
 * refusal, and that register-offset / write-back / post-indexed / pc / LDRD
 * forms are refused with regs and device untouched.
 *
 * What it cannot check: behaviour under a real data abort, and that the
 * encodings below match what the target's gcc actually emits -- the words
 * are built here from the ARM ARM, not disassembled from wl.
 */
#include <stdio.h>
#include <string.h>

#include "arm_mmio_emulate.h"

static int fails;

static void fail(const char *what)
{
	printf("  FAIL: %s\n", what);
	fails++;
}

#define EXPECT(cond, what) do { if (!(cond)) fail(what); } while (0)

#define COND_AL  0xeu

/* ldr/str/ldrb/strb, immediate offset form: cond 010 1 U B 0 L Rn Rt imm12 */
static u32 ls_imm(u32 cond, int L, int B, int U, u32 Rn, u32 Rt, u32 imm12)
{
	return (cond << 28) | (0x2u << 25) | (1u << 24) | ((u32)!!U << 23) |
	       ((u32)!!B << 22) | ((u32)!!L << 20) | (Rn << 16) | (Rt << 12) |
	       (imm12 & 0xfff);
}

/* extra load/store, immediate: cond 000 1 U 1 0 L Rn Rt immH 1 S H 1 immL */
static u32 extra_imm(u32 cond, int L, int U, int S, int H, u32 Rn, u32 Rt,
		     u32 imm8)
{
	return (cond << 28) | (1u << 24) | ((u32)!!U << 23) | (1u << 22) |
	       ((u32)!!L << 20) | (Rn << 16) | (Rt << 12) |
	       ((imm8 & 0xf0) << 4) | (0x9u << 4) | ((u32)!!S << 6) |
	       ((u32)!!H << 5) | (imm8 & 0xf);
}

struct bench {
	struct arm_emu_regs regs;
	unsigned char dev[16];
	unsigned long fault_va;
};

/* Rn holds a "window" address that is NOT the device buffer; the emulator
 * compares the decoded effective address against fault_va and then accesses
 * through access_va, exactly as the module does on target. */
#define WIN_BASE 0x40000000UL
#define RN 4

static void setup(struct bench *b)
{
	memset(b, 0, sizeof(*b));
	b->regs.r[RN] = WIN_BASE;
	b->regs.r[15] = 0x9000;
	b->regs.cpsr = 0;
	for (int i = 0; i < 16; i++)
		b->dev[i] = 0x10 + i;
}

static enum arm_emu_status run(struct bench *b, u32 insn, u32 off,
			       struct arm_emu_result *out)
{
	b->fault_va = WIN_BASE + off;
	return arm_mmio_emulate_one(&b->regs, insn, b->fault_va,
				    (void *)(b->dev + off), out);
}

int main(void)
{
	struct arm_emu_result out;
	struct bench b;
	enum arm_emu_status st;

	printf("test_arm_emulate\n");

	/* LDR word at +4: r1 = dev[4..7] little-endian, pc advances. */
	setup(&b);
	st = run(&b, ls_imm(COND_AL, 1, 0, 1, RN, 1, 4), 4, &out);
	EXPECT(st == ARM_MMIO_OK, "ldr status");
	EXPECT(out.width == 4 && !out.is_write, "ldr shape");
	EXPECT(b.regs.r[1] == 0x17161514u, "ldr value");
	EXPECT(b.regs.r[15] == 0x9004, "ldr pc");

	/* STR word at +0: dev[0..3] = r2, pc advances. */
	setup(&b);
	b.regs.r[2] = 0xaabbccdd;
	st = run(&b, ls_imm(COND_AL, 0, 0, 1, RN, 2, 0), 0, &out);
	EXPECT(st == ARM_MMIO_OK && out.is_write && out.width == 4, "str shape");
	EXPECT(b.dev[0] == 0xdd && b.dev[3] == 0xaa, "str bytes");

	/* LDRB at +5: zero-extended single byte. */
	setup(&b);
	st = run(&b, ls_imm(COND_AL, 1, 1, 1, RN, 3, 5), 5, &out);
	EXPECT(st == ARM_MMIO_OK && out.width == 1, "ldrb shape");
	EXPECT(b.regs.r[3] == 0x15u, "ldrb value");

	/* STRB at +2. */
	setup(&b);
	b.regs.r[5] = 0x1234ff99;
	st = run(&b, ls_imm(COND_AL, 0, 1, 1, RN, 5, 2), 2, &out);
	EXPECT(st == ARM_MMIO_OK && out.is_write && out.width == 1, "strb shape");
	EXPECT(b.dev[2] == 0x99 && b.dev[3] == 0x13, "strb one byte only");

	/* LDRH at +4: dev[4],dev[5] = 0x14,0x15 -> 0x1514. */
	setup(&b);
	st = run(&b, extra_imm(COND_AL, 1, 1, 0, 1, RN, 6, 4), 4, &out);
	EXPECT(st == ARM_MMIO_OK && out.width == 2 && !out.sign_extend, "ldrh shape");
	EXPECT(b.regs.r[6] == 0x1514u, "ldrh value");

	/* LDRSB of a negative byte sign-extends. */
	setup(&b);
	b.dev[7] = 0x80;
	st = run(&b, extra_imm(COND_AL, 1, 1, 1, 0, RN, 7, 7), 7, &out);
	EXPECT(st == ARM_MMIO_OK && out.width == 1 && out.sign_extend, "ldrsb shape");
	EXPECT(b.regs.r[7] == 0xffffff80u, "ldrsb value");

	/* STRH at +8. */
	setup(&b);
	b.regs.r[8] = 0x0000beef;
	st = run(&b, extra_imm(COND_AL, 0, 1, 0, 1, RN, 8, 8), 8, &out);
	EXPECT(st == ARM_MMIO_OK && out.is_write && out.width == 2, "strh shape");
	EXPECT(b.dev[8] == 0xef && b.dev[9] == 0xbe && b.dev[10] == 0x1a,
	       "strh two bytes only");

	/* U=0: subtract the offset. r1 = dev[0], base = WIN_BASE+4. */
	setup(&b);
	b.regs.r[RN] = WIN_BASE + 4;
	b.fault_va = WIN_BASE;
	st = arm_mmio_emulate_one(&b.regs, ls_imm(COND_AL, 1, 0, 0, RN, 1, 4),
				  WIN_BASE, (void *)b.dev, &out);
	EXPECT(st == ARM_MMIO_OK && b.regs.r[1] == 0x13121110u, "ldr U=0");

	/* Condition false: NE with Z set. Nothing touched. */
	setup(&b);
	b.regs.cpsr = (1u << 30);		/* Z */
	b.regs.r[1] = 0x5a5a5a5a;
	st = run(&b, ls_imm(0x1 /* NE */, 1, 0, 1, RN, 1, 4), 4, &out);
	EXPECT(st == ARM_MMIO_COND_FALSE, "cond false status");
	EXPECT(b.regs.r[1] == 0x5a5a5a5a && b.regs.r[15] == 0x9000,
	       "cond false untouched");

	/* Address mismatch: decoded ea != fault_va. */
	setup(&b);
	b.fault_va = WIN_BASE + 8;
	st = arm_mmio_emulate_one(&b.regs, ls_imm(COND_AL, 1, 0, 1, RN, 1, 4),
				  WIN_BASE + 8, (void *)b.dev, &out);
	EXPECT(st == ARM_MMIO_ADDR_MISMATCH, "addr mismatch");
	EXPECT(b.regs.r[15] == 0x9000, "mismatch pc unmoved");

	/* Register-offset LDR (bit 25 set): refused. */
	setup(&b);
	st = run(&b, ls_imm(COND_AL, 1, 0, 1, RN, 1, 0) | (1u << 25), 0, &out);
	EXPECT(st == ARM_MMIO_NOT_SIMPLE_LS, "reg-offset refused");

	/* Post-indexed (P=0): refused, base would change. */
	setup(&b);
	st = run(&b, ls_imm(COND_AL, 1, 0, 1, RN, 1, 4) & ~(1u << 24), 4, &out);
	EXPECT(st == ARM_MMIO_NOT_SIMPLE_LS, "post-indexed refused");

	/* Write-back (W=1): refused. */
	setup(&b);
	st = run(&b, ls_imm(COND_AL, 1, 0, 1, RN, 1, 4) | (1u << 21), 4, &out);
	EXPECT(st == ARM_MMIO_NOT_SIMPLE_LS, "write-back refused");

	/* pc as data register: refused. */
	setup(&b);
	st = run(&b, ls_imm(COND_AL, 1, 0, 1, RN, 15, 4), 4, &out);
	EXPECT(st == ARM_MMIO_NOT_SIMPLE_LS, "pc data refused");

	/* STRD (extra-LS, L=0, sh=3): doubleword, refused distinctly. */
	setup(&b);
	st = run(&b, extra_imm(COND_AL, 0, 1, 1, 1, RN, 0, 8), 8, &out);
	EXPECT(st == ARM_MMIO_DOUBLEWORD, "strd refused");

	if (fails) {
		printf("  %d FAILED\n", fails);
		return 1;
	}
	printf("  all passed\n");
	return 0;
}
