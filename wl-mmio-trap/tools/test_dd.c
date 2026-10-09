// SPDX-License-Identifier: GPL-2.0
/*
 * Drives dma_dd.c on the host against a synthetic memory holding a TX ring
 * and its buffers.
 *
 * What it checks: which window offsets are channel registers, the walk from
 * the previous index to the new one, the wrap at the descriptor with EOT,
 * the index as an address and as an offset into the ring, the ring address
 * read back for a ring set up before the trap, both byte orders of the
 * descriptor words, address bit 63 in the high word, the budget, dump_len 0 tracking without dumping, a
 * buffer outside memory and a descriptor that is not one.
 *
 * What it cannot check: the byte order the stock driver actually uses on
 * the router, and the uncached reads.
 */
#include <stdio.h>
#include <string.h>

#include "dma_dd.h"

#define MEM_BASE	0x00c00000u
#define MEM_SIZE	0x00100000u
#define RING		0x00c18000u
#define BUFS		0x00c40000u

static u8 mem[MEM_SIZE];
static u32 ring_reg[DD_NCHAN];

struct got {
	unsigned int chan;
	u32 slot;
	struct dd_desc d;
	u8 buf[DD_MAX_DUMP];
	u32 n;
};

static struct got got[64];
static unsigned int ngot;
static int fails;

#define EXPECT(cond, what) do { \
	if (!(cond)) { printf("  FAIL: %s\n", what); fails++; } } while (0)

static bool rd(void *ctx, u32 bus, void *dst, u32 len)
{
	(void)ctx;
	if (bus < MEM_BASE || bus + len > MEM_BASE + MEM_SIZE || bus + len < bus)
		return false;
	memcpy(dst, mem + (bus - MEM_BASE), len);
	return true;
}

static u32 ring_addr(void *ctx, unsigned int chan)
{
	(void)ctx;
	return ring_reg[chan];
}

static void emit(void *ctx, unsigned int chan, u32 slot, const u8 *raw,
		 const struct dd_desc *d, const u8 *buf, u32 n)
{
	struct got *g = &got[ngot++];

	(void)ctx;
	(void)raw;
	g->chan = chan;
	g->slot = slot;
	g->d = *d;
	memcpy(g->buf, buf, n);
	g->n = n;
}

static const struct dd_ops ops = { rd, ring_addr, emit };

static void put32(u8 *p, u32 v, bool be)
{
	int i;

	for (i = 0; i < 4; i++)
		p[i] = be ? v >> (24 - 8 * i) : v >> (8 * i);
}

/* Descriptor @i of the ring, pointing at buffer @i of @len bytes, each
 * buffer filled with its index. */
static void mk_desc(u32 ring, unsigned int i, u32 len, bool eot, bool be)
{
	u8 *p = mem + (ring - MEM_BASE) + i * DD_SIZE;
	u32 buf = BUFS + i * 0x800;

	put32(p, DD_CTL0_SOF | DD_CTL0_EOF | (eot ? DD_CTL0_EOT : 0), be);
	put32(p + 4, len, be);
	put32(p + 8, buf, be);
	put32(p + 12, 0, be);
	memset(mem + (buf - MEM_BASE), (int)i, len);
}

static void set_addrhi(u32 ring, unsigned int i, u32 hi)
{
	put32(mem + (ring - MEM_BASE) + i * DD_SIZE + 12, hi, false);
}

static void reset(void)
{
	memset(mem, 0, sizeof(mem));
	memset(ring_reg, 0, sizeof(ring_reg));
	ngot = 0;
}

static void test_post(void)
{
	struct dd_state s;
	u32 budget = 100;
	unsigned int i;

	printf("post, address index\n");
	reset();
	dd_init(&s);
	for (i = 0; i < 8; i++)
		mk_desc(RING, i, 200, false, false);

	dd_on_write(&s, &ops, NULL, 0x208, RING, 168, &budget);
	dd_on_write(&s, &ops, NULL, 0x204, RING + 1 * DD_SIZE, 168, &budget);
	EXPECT(ngot == 1, "first post: one descriptor");
	EXPECT(got[0].slot == RING && got[0].chan == 0, "first post: slot 0");
	EXPECT(got[0].n == 168 && got[0].buf[167] == 0, "dump_len caps the buffer");
	EXPECT(!(got[0].d.flags & (DD_F_BE | DD_F_GUESS)), "little-endian, not guessed");
	EXPECT(got[0].d.ctl1 == 200 && got[0].d.addrlo == BUFS, "decoded words");

	dd_on_write(&s, &ops, NULL, 0x204, RING + 4 * DD_SIZE, 168, &budget);
	EXPECT(ngot == 4, "second post: three descriptors");
	EXPECT(got[3].slot == RING + 3 * DD_SIZE && got[3].buf[0] == 3,
	       "second post: slots 1-3 with their buffers");
	EXPECT(budget == 96, "one budget unit per descriptor");

	ngot = 0;
	dd_on_write(&s, &ops, NULL, 0x200, 0x1, 168, &budget);
	dd_on_write(&s, &ops, NULL, 0x210, 0x1, 168, &budget);
	dd_on_write(&s, &ops, NULL, 0x224, RING, 168, &budget);
	EXPECT(ngot == 0, "control, status and RX writes are not posts");
}

static void test_wrap(void)
{
	struct dd_state s;
	u32 budget = 100;

	printf("wrap at EOT\n");
	reset();
	dd_init(&s);
	mk_desc(RING, 0, 64, false, false);
	mk_desc(RING, 1, 64, false, false);
	mk_desc(RING, 2, 64, false, false);
	mk_desc(RING, 3, 64, true, false);

	dd_on_write(&s, &ops, NULL, 0x288, RING, 64, &budget);
	dd_on_write(&s, &ops, NULL, 0x284, RING + 2 * DD_SIZE, 64, &budget);
	ngot = 0;
	dd_on_write(&s, &ops, NULL, 0x284, RING + 1 * DD_SIZE, 64, &budget);
	EXPECT(ngot == 3, "slots 2, 3 and 0");
	EXPECT(got[0].chan == 2, "channel 2");
	EXPECT(got[0].slot == RING + 2 * DD_SIZE &&
	       got[1].slot == RING + 3 * DD_SIZE && got[2].slot == RING,
	       "walk order across the wrap");
}

static void test_offset_index(void)
{
	struct dd_state s;
	u32 budget = 100;

	printf("offset index\n");
	reset();
	dd_init(&s);
	mk_desc(RING, 0, 32, false, false);
	mk_desc(RING, 1, 32, false, false);

	dd_on_write(&s, &ops, NULL, 0x348, RING, 32, &budget);
	dd_on_write(&s, &ops, NULL, 0x344, 0, 32, &budget);
	dd_on_write(&s, &ops, NULL, 0x344, 2 * DD_SIZE, 32, &budget);
	EXPECT(ngot == 2 && got[1].slot == RING + DD_SIZE && got[1].chan == 5,
	       "offsets resolved against the ring, channel 5");

	ngot = 0;
	dd_on_write(&s, &ops, NULL, 0x384, RING + DD_SIZE, 32, &budget);
	EXPECT(ngot == 0, "past the sixth channel");
}

static void test_ring_before_trap(void)
{
	struct dd_state s;
	u32 budget = 100;

	printf("ring set up before the trap\n");
	reset();
	dd_init(&s);
	mk_desc(RING, 0, 32, false, false);
	mk_desc(RING, 1, 32, false, false);
	ring_reg[1] = RING;

	dd_on_write(&s, &ops, NULL, 0x244, RING + 2 * DD_SIZE, 32, &budget);
	EXPECT(ngot == 1 && got[0].slot == RING + DD_SIZE,
	       "the last slot only");
	EXPECT(got[0].d.flags & DD_F_GUESS, "marked as guessed");

	ngot = 0;
	dd_init(&s);
	dd_on_write(&s, &ops, NULL, 0x244, RING, 32, &budget);
	EXPECT(ngot == 0, "nothing to guess at the ring's first slot");

	ngot = 0;
	dd_init(&s);
	ring_reg[1] = 0;
	dd_on_write(&s, &ops, NULL, 0x244, 2 * DD_SIZE, 32, &budget);
	EXPECT(ngot == 0, "an offset with no ring is dropped");
}

static void test_big_endian(void)
{
	struct dd_state s;
	u32 budget = 100;

	printf("big-endian descriptor words\n");
	reset();
	dd_init(&s);
	mk_desc(RING, 0, 100, false, true);

	dd_on_write(&s, &ops, NULL, 0x208, RING, 168, &budget);
	dd_on_write(&s, &ops, NULL, 0x204, RING + DD_SIZE, 168, &budget);
	EXPECT(ngot == 1 && (got[0].d.flags & DD_F_BE), "detected");
	EXPECT(got[0].d.ctl1 == 100 && got[0].d.addrlo == BUFS &&
	       got[0].n == 100, "decoded, buffer shorter than dump_len");
}

static void test_addrhi(void)
{
	struct dd_state s;
	u32 budget = 100;

	printf("high address word\n");
	reset();
	dd_init(&s);
	mk_desc(RING, 0, 32, false, false);
	mk_desc(RING, 1, 32, false, false);
	set_addrhi(RING, 0, DD_ADDRHI_PCI64);
	set_addrhi(RING, 1, 0x00000001);

	dd_on_write(&s, &ops, NULL, 0x208, RING, 32, &budget);
	dd_on_write(&s, &ops, NULL, 0x204, RING + 2 * DD_SIZE, 32, &budget);
	EXPECT(ngot == 2, "both descriptors");
	EXPECT(got[0].n == 32 && !(got[0].d.flags & DD_F_NOBUF),
	       "bit 63 alone: the buffer is read");
	EXPECT(got[1].n == 0 && (got[1].d.flags & DD_F_NOBUF),
	       "other high bits: not memory to read");
}

static void test_limits(void)
{
	struct dd_state s;
	u32 budget = 2;
	unsigned int i;

	printf("budget, dump_len 0, bad buffer, bad descriptor\n");
	reset();
	dd_init(&s);
	for (i = 0; i < 6; i++)
		mk_desc(RING, i, 32, false, false);

	dd_on_write(&s, &ops, NULL, 0x208, RING, 32, &budget);
	dd_on_write(&s, &ops, NULL, 0x204, RING + 4 * DD_SIZE, 32, &budget);
	EXPECT(ngot == 2 && budget == 0, "stops when the budget runs out");

	ngot = 0;
	budget = 10;
	dd_on_write(&s, &ops, NULL, 0x204, RING + 5 * DD_SIZE, 0, &budget);
	EXPECT(ngot == 0 && budget == 10, "dump_len 0 dumps nothing");
	put32(mem + (RING - MEM_BASE) + 5 * DD_SIZE + 8, 0x40000000, false);
	mk_desc(RING, 6, 0, false, false);
	dd_on_write(&s, &ops, NULL, 0x204, RING + 7 * DD_SIZE, 32, &budget);
	EXPECT(ngot == 1 && got[0].slot == RING + 5 * DD_SIZE,
	       "tracking went on while off; the walk stops at a non-descriptor");
	EXPECT((got[0].d.flags & DD_F_NOBUF) && got[0].n == 0,
	       "buffer outside memory");
}

int main(void)
{
	test_post();
	test_wrap();
	test_offset_index();
	test_ring_before_trap();
	test_big_endian();
	test_addrhi();
	test_limits();

	if (fails) {
		printf("\n%d descriptor check(s) FAILED\n", fails);
		return 1;
	}
	printf("\nall descriptor checks passed\n");
	return 0;
}
