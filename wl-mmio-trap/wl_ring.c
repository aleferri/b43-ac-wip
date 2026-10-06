// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <linux/string.h>
#include <asm/barrier.h>

#include "wl_ring.h"

void wl_ring_init(struct wl_ring *r, void *buf, u32 slots, u32 esize)
{
	r->buf = buf;
	r->mask = slots - 1;
	r->esize = esize;
	r->head = 0;
	r->tail = 0;
}

u32 wl_ring_len(const struct wl_ring *r)
{
	return (u32)(ACCESS_ONCE(r->head) - ACCESS_ONCE(r->tail));
}

bool wl_ring_put(struct wl_ring *r, const void *rec)
{
	if (wl_ring_len(r) > r->mask)
		return false;

	memcpy(r->buf + (size_t)(r->head & r->mask) * r->esize, rec, r->esize);
	smp_wmb();		/* the record before the head that publishes it */
	r->head++;
	return true;
}

u32 wl_ring_peek(struct wl_ring *r, u32 max, const void **p)
{
	u32 tail = r->tail;
	u32 n = (u32)(ACCESS_ONCE(r->head) - tail);
	u32 to_end = r->mask + 1 - (tail & r->mask);

	smp_rmb();		/* the head before the records it covers */
	if (n > to_end)
		n = to_end;
	if (n > max)
		n = max;
	*p = r->buf + (size_t)(tail & r->mask) * r->esize;
	return n;
}

void wl_ring_consume(struct wl_ring *r, u32 n)
{
	smp_mb();		/* reads of the slots before they are handed back */
	r->tail += n;
}
