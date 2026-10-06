/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fixed-size record ring: any number of producers, one consumer.
 *
 * Producers must be serialised by the caller (wl_ring_put() takes no lock);
 * the consumer takes none either. It reads a run of records in place with
 * wl_ring_peek(), copies them wherever it likes -- to userspace, say -- and
 * only then gives the slots back with wl_ring_consume(), so a read drains
 * as much as the caller asked for in one go.
 *
 * Pure logic with no kernel dependency beyond the barriers: tools/ checks
 * this file natively.
 */
#ifndef WL_RING_H
#define WL_RING_H

#include <linux/types.h>

struct wl_ring {
	u8 *buf;
	u32 mask;	/* slots - 1; slots is a power of two */
	u32 esize;	/* record size in bytes */
	u32 head;	/* next slot to write, free-running */
	u32 tail;	/* next slot to read, free-running */
};

void wl_ring_init(struct wl_ring *r, void *buf, u32 slots, u32 esize);

/* Records waiting to be read. */
u32 wl_ring_len(const struct wl_ring *r);

/* False, with nothing written, when every slot is taken. */
bool wl_ring_put(struct wl_ring *r, const void *rec);

/* Points *p at the longest contiguous run of unread records, at most max,
 * and returns how many. The run ends at the end of the buffer even if more
 * records wrap round behind it: ask again after consuming. */
u32 wl_ring_peek(struct wl_ring *r, u32 max, const void **p);

/* Gives back n records previously returned by wl_ring_peek(). */
void wl_ring_consume(struct wl_ring *r, u32 n);

#endif /* WL_RING_H */
