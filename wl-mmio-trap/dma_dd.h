/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The TX descriptors wl posts, read out of memory when it writes a TX
 * channel's index register.
 *
 * The descriptor and the frame in front of which wl puts the d11 TX header
 * never cross the register window: the DMA engine fetches them from memory
 * by itself. The index write that hands them over does cross it, so this is
 * where the trap can see what the engine is about to read. The register
 * layout is the DMA64 one of the d11 core: channel n at 0x200 + 0x40 * n,
 * TX index at +0x04, TX ring address at +0x08. On the engines of these chips
 * the index is the low 32 bits of the bus address of the descriptor after
 * the last one posted; an older engine takes the offset into the ring, and
 * both are handled.
 *
 * Pure logic, with the memory reads and the record output behind
 * callbacks: tools/ checks this file natively.
 */
#ifndef DMA_DD_H
#define DMA_DD_H

#include <linux/types.h>

#define DD_NCHAN		6
#define DD_SIZE			16	/* struct dma64dd */
#define DD_MAX_DUMP		256	/* buffer bytes per descriptor */
#define DD_MAX_WALK		512	/* descriptors per index write */

/* Descriptor control words, as brcmsmac and b43 name them */
#define DD_CTL0_SOF		0x80000000
#define DD_CTL0_EOF		0x40000000
#define DD_CTL0_EOT		0x10000000
#define DD_CTL1_BYTECNT		0x00007fff
/* Address bit 63, which the engine behind PCIe carries on every address it
 * is given (the ring's and the buffers', 0x80000000 in the high words on the
 * vd625); not part of the memory address. */
#define DD_ADDRHI_PCI64		0x80000000

/* Flags of a dumped descriptor */
#define DD_F_BE			0x01	/* descriptor words big-endian in memory */
#define DD_F_NOBUF		0x02	/* buffer not in readable memory */
#define DD_F_GUESS		0x04	/* no earlier index: only the last slot */

struct dd_chan {
	u32 base;		/* bus address of the ring */
	u32 last;		/* bus address the last index write pointed at */
	bool base_known;
	bool last_known;
};

struct dd_state {
	struct dd_chan ch[DD_NCHAN];
};

struct dd_desc {
	u32 ctl0, ctl1, addrlo, addrhi;
	u8 flags;
};

struct dd_ops {
	/* Bytes at a bus address; false when it is not memory to read. */
	bool (*read)(void *ctx, u32 bus, void *dst, u32 len);
	/* The ring address register of a channel, for a ring whose address
	 * was written before the trap was armed. May be NULL. */
	u32 (*ring_addr)(void *ctx, unsigned int chan);
	/* One descriptor: its slot, its 16 raw bytes, and n bytes of the
	 * buffer it points at (n = 0 with DD_F_NOBUF). */
	void (*emit)(void *ctx, unsigned int chan, u32 slot, const u8 *raw,
		     const struct dd_desc *d, const u8 *buf, u32 n);
};

void dd_init(struct dd_state *s);

/*
 * Feed one trapped write to the window. Index writes walk the descriptors
 * posted since the previous one and emit each with up to dump_len bytes of
 * its buffer, while *budget lasts (one per descriptor). dump_len 0 still
 * tracks the rings, so dumping can be switched on mid-capture.
 */
void dd_on_write(struct dd_state *s, const struct dd_ops *ops, void *ctx,
		 u32 off, u32 val, u32 dump_len, u32 *budget);

/* Decode 16 raw descriptor bytes; false if neither byte order is valid. */
bool dd_decode(const u8 *raw, struct dd_desc *d);

#endif
