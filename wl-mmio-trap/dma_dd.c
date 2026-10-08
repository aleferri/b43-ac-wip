// SPDX-License-Identifier: GPL-2.0
#include <linux/string.h>

#include "dma_dd.h"

#define DD_CHAN_BASE		0x200
#define DD_CHAN_STRIDE		0x40
#define DD_TXINDEX		0x04
#define DD_TXRINGLO		0x08
/* The index is an offset into the ring below this, an address above. */
#define DD_RING_SPAN		0x2000

static u32 le32_at(const u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static u32 be32_at(const u8 *p)
{
	return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static bool dd_ctl1_valid(u32 ctl1)
{
	return (ctl1 & DD_CTL1_BYTECNT) && !(ctl1 & 0xfff80000);
}

bool dd_decode(const u8 *raw, struct dd_desc *d)
{
	u32 (*rd)(const u8 *);

	if (dd_ctl1_valid(le32_at(raw + 4))) {
		rd = le32_at;
		d->flags = 0;
	} else if (dd_ctl1_valid(be32_at(raw + 4))) {
		rd = be32_at;
		d->flags = DD_F_BE;
	} else {
		return false;
	}
	d->ctl0 = rd(raw);
	d->ctl1 = rd(raw + 4);
	d->addrlo = rd(raw + 8);
	d->addrhi = rd(raw + 12);
	return true;
}

void dd_init(struct dd_state *s)
{
	memset(s, 0, sizeof(*s));
}

/* Which channel register a window offset is, if any. */
static int dd_reg(u32 off, unsigned int *chan)
{
	u32 rel;

	if (off < DD_CHAN_BASE ||
	    off >= DD_CHAN_BASE + DD_NCHAN * DD_CHAN_STRIDE)
		return -1;
	rel = off - DD_CHAN_BASE;
	*chan = rel / DD_CHAN_STRIDE;
	return rel % DD_CHAN_STRIDE;
}

/* One descriptor out of memory and its buffer; false stops the walk. */
static bool dd_dump_one(const struct dd_ops *ops, void *ctx, unsigned int chan,
			u32 slot, u8 flags, u32 dump_len, struct dd_desc *d)
{
	u8 raw[DD_SIZE];
	u8 buf[DD_MAX_DUMP];
	u32 n;

	if (!ops->read(ctx, slot, raw, sizeof(raw)) || !dd_decode(raw, d))
		return false;
	d->flags |= flags;

	n = d->ctl1 & DD_CTL1_BYTECNT;
	if (n > dump_len)
		n = dump_len;
	if (n > DD_MAX_DUMP)
		n = DD_MAX_DUMP;
	if (d->addrhi || !ops->read(ctx, d->addrlo, buf, n)) {
		d->flags |= DD_F_NOBUF;
		n = 0;
	}
	ops->emit(ctx, chan, slot, raw, d, buf, n);
	return true;
}

static void dd_walk(struct dd_chan *c, const struct dd_ops *ops, void *ctx,
		    unsigned int chan, u32 from, u32 to, u8 flags,
		    u32 dump_len, u32 *budget)
{
	struct dd_desc d;
	unsigned int i;
	u32 slot = from;

	for (i = 0; i < DD_MAX_WALK && slot != to && *budget; i++) {
		if (!dd_dump_one(ops, ctx, chan, slot, flags, dump_len, &d))
			return;
		(*budget)--;
		if (d.ctl0 & DD_CTL0_EOT) {
			if (!c->base_known)
				return;
			slot = c->base;
		} else {
			slot += DD_SIZE;
		}
	}
}

void dd_on_write(struct dd_state *s, const struct dd_ops *ops, void *ctx,
		 u32 off, u32 val, u32 dump_len, u32 *budget)
{
	struct dd_chan *c;
	unsigned int chan;
	u32 slot, from;
	u8 flags = 0;

	switch (dd_reg(off, &chan)) {
	case DD_TXRINGLO:
		/* A ring being set up: the first post starts at its head. */
		c = &s->ch[chan];
		c->base = val;
		c->base_known = true;
		c->last = val;
		c->last_known = true;
		return;
	case DD_TXINDEX:
		break;
	default:
		return;
	}

	c = &s->ch[chan];
	if (!c->base_known && ops->ring_addr) {
		c->base = ops->ring_addr(ctx, chan);
		c->base_known = c->base != 0;
	}
	if (val < DD_RING_SPAN) {
		if (!c->base_known)
			return;
		slot = c->base + val;
	} else {
		slot = val;
	}

	if (c->last_known) {
		from = c->last;
	} else {
		/* No earlier index: the slot before this one is the last
		 * posted, unless the ring has just wrapped. */
		if (c->base_known && slot == c->base)
			from = slot;
		else
			from = slot - DD_SIZE;
		flags = DD_F_GUESS;
	}
	c->last = slot;
	c->last_known = true;

	if (dump_len && budget && *budget)
		dd_walk(c, ops, ctx, chan, from, slot, flags, dump_len, budget);
}
