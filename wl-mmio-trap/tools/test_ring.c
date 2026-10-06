// SPDX-License-Identifier: GPL-2.0
/*
 * Drives wl_ring.c on the host: ordering, the full and empty edges, a wrap
 * of the buffer, peek limited by the end of the buffer and by max, and one
 * producer against one consumer on two threads.
 *
 * What it cannot check: the barriers on a weakly ordered MIPS core. The
 * threaded run only shows the logic holds under a real interleaving on
 * the host's memory model.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "wl_ring.h"

struct rec {
	u32 seq;
	u32 check;	/* ~seq: a torn copy shows up here */
};

static int fails;

static void fail(const char *what)
{
	printf("  FAIL: %s\n", what);
	fails++;
}

#define EXPECT(cond, what) do { if (!(cond)) fail(what); } while (0)

static struct rec mk(u32 seq)
{
	struct rec r = { seq, ~seq };

	return r;
}

static void test_basic(void)
{
	struct rec buf[4];
	struct wl_ring r;
	const void *p;
	u32 i, n;

	printf("fill, refuse, drain\n");
	wl_ring_init(&r, buf, 4, sizeof(buf[0]));
	EXPECT(wl_ring_len(&r) == 0, "starts empty");
	EXPECT(wl_ring_peek(&r, 10, &p) == 0, "peek on empty gives nothing");

	for (i = 0; i < 4; i++) {
		struct rec x = mk(i);

		EXPECT(wl_ring_put(&r, &x), "put while there is room");
	}
	EXPECT(wl_ring_len(&r) == 4, "len counts every slot when full");
	{
		struct rec x = mk(99);

		EXPECT(!wl_ring_put(&r, &x), "put refused when full");
	}
	EXPECT(wl_ring_len(&r) == 4, "a refused put changes nothing");

	n = wl_ring_peek(&r, 10, &p);
	EXPECT(n == 4, "peek returns the whole run");
	for (i = 0; i < n; i++)
		EXPECT(((const struct rec *)p)[i].seq == i, "records in order");
	wl_ring_consume(&r, n);
	EXPECT(wl_ring_len(&r) == 0, "empty after consuming everything");
}

static void test_wrap_and_max(void)
{
	struct rec buf[4];
	struct wl_ring r;
	const struct rec *p;
	u32 i, n, next = 0, want = 0;

	printf("wrap, run ends at the buffer end, max\n");
	wl_ring_init(&r, buf, 4, sizeof(buf[0]));

	/* Move head and tail to slot 3, so the next three records wrap. */
	for (i = 0; i < 3; i++) {
		struct rec x = mk(next++);

		wl_ring_put(&r, &x);
	}
	n = wl_ring_peek(&r, 10, (const void **)&p);
	wl_ring_consume(&r, n);
	want = next;

	for (i = 0; i < 3; i++) {
		struct rec x = mk(next++);

		EXPECT(wl_ring_put(&r, &x), "put across the wrap");
	}
	EXPECT(wl_ring_len(&r) == 3, "len across the wrap");

	n = wl_ring_peek(&r, 10, (const void **)&p);
	EXPECT(n == 1, "first run stops at the end of the buffer");
	EXPECT(p[0].seq == want, "and holds the oldest record");
	wl_ring_consume(&r, n);
	want++;

	n = wl_ring_peek(&r, 1, (const void **)&p);
	EXPECT(n == 1, "max of 1 is honoured");
	EXPECT(p[0].seq == want, "next record after the wrap");
	wl_ring_consume(&r, n);
	want++;

	n = wl_ring_peek(&r, 10, (const void **)&p);
	EXPECT(n == 1 && p[0].seq == want, "the last one");
	wl_ring_consume(&r, n);
	EXPECT(wl_ring_len(&r) == 0, "drained");

	EXPECT(wl_ring_peek(&r, 0, (const void **)&p) == 0, "max of 0 gives nothing");
}

#define N_RECS 2000000u

static struct wl_ring tr;
static volatile int producer_done;

static void *producer(void *arg)
{
	u32 i;

	(void)arg;
	for (i = 0; i < N_RECS; ) {
		struct rec x = mk(i);

		if (wl_ring_put(&tr, &x))
			i++;
	}
	producer_done = 1;
	return NULL;
}

static void test_threads(void)
{
	struct rec *buf = malloc(64 * sizeof(*buf));
	pthread_t t;
	u32 got = 0;
	int bad = 0;

	printf("one producer, one consumer\n");
	wl_ring_init(&tr, buf, 64, sizeof(*buf));
	pthread_create(&t, NULL, producer, NULL);

	while (got < N_RECS) {
		const struct rec *p;
		u32 n = wl_ring_peek(&tr, 16, (const void **)&p), i;

		for (i = 0; i < n; i++, got++)
			if (p[i].seq != got || p[i].check != ~got)
				bad++;
		wl_ring_consume(&tr, n);
	}
	pthread_join(t, NULL);
	EXPECT(bad == 0, "every record arrives once, in order, whole");
	EXPECT(wl_ring_len(&tr) == 0, "nothing left over");
	free(buf);
}

int main(void)
{
	test_basic();
	test_wrap_and_max();
	test_threads();

	printf("\n%s\n", fails ? "FAILED" : "all ring checks passed");
	return fails ? 1 : 0;
}
