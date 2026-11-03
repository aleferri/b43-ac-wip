// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver
 * AC-PHY radar pulse matching
 *
 * The pulses of the two radar FIFOs (see radar_ac.h) are matched against the
 * short-pulse radar test signals of ETSI EN 301 893 (reference DFS test
 * signals 1-4, Table D.4) and FCC KDB 905462 D02 (radar types 0-4, Table 5):
 * a burst of pulses of one width at one pulse repetition interval. A pulse
 * that fits a type's width is the end of a candidate train with every
 * earlier pulse in that type's PRI range; the train is the pulses at whole
 * multiples of that interval, so missed pulses leave gaps but do not break
 * it. A type is reported when its train reaches the type's count.
 *
 * Only pulses that w2 15:8 says are coherent are matched (see
 * B43_RADAR_COHERENT). Both regions' types are matched whatever the
 * regulatory domain. Not
 * matched: ETSI 5 and 6 (staggered PRF), FCC 5 (long chirped pulses in
 * bursts over 12 s) and FCC 6 (frequency hopping).
 */

#include <linux/kernel.h>

#include "radar_ac.h"

/* Times in the FIFO's 12.5 ns units. */
#define B43_RADAR_TICKS_PER_US	80
#define B43_RADAR_US(us)	((u32)(us) * B43_RADAR_TICKS_PER_US)
/* A width unit, 0.2 us, in time units. */
#define B43_RADAR_WIDTH_TICKS	16

/*
 * The FIFO times agree with the pulses sent to within 0.05 us (one HackRF
 * sample) over 100 pulses, and a pulse is seen by both FIFOs at the same
 * time to the unit. The tolerance on a train is 1 us, and 0.1 us more per
 * interval for the error of an interval taken from one pair of pulses.
 */
#define B43_RADAR_TOL		B43_RADAR_US(1)
#define B43_RADAR_TOL_STEP	(B43_RADAR_TICKS_PER_US / 10)

/* The two FIFOs' entries of one pulse are this close, or the same. */
#define B43_RADAR_SAME		B43_RADAR_US(2)

/*
 * A pulse longer than about 27 us comes as two entries: 50 us as 27.0 us and,
 * 28.8 us after its start, 21.2 us. The second starts within this gap of the
 * end of a first that long.
 */
#define B43_RADAR_SPLIT_WIDTH	125
#define B43_RADAR_SPLIT_GAP	B43_RADAR_US(3)

/*
 * The AP's own beacons, when its transmissions are not blanked, and other
 * OFDM frames come as entries 0.0-0.2 us wide; the shortest radar pulse,
 * 0.5 us, reads 1.2 us or more.
 */
#define B43_RADAR_MIN_WIDTH	3

/*
 * w2 15:8 of an entry, what it measures not known, tells a radar pulse from
 * a burst of noise. Over the runs with known pulses
 * (router-data/rt-ac66u-b1/radar-fifo.zip) it reads 0xff on 2497 entries
 * of CW and chirped pulses, at every level down to where the FIFOs stop
 * seeing them, and 0xfd-0xfe on a few chirped ones; on noise10, pulses of
 * noise 10 us long every 1000 us, 0x00-0x87 on 31 of its 32 entries; on the
 * AP's own beacons, unblanked, 0x83-0x86 and 0xb4-0xb9. The entries a full
 * FIFO repeats (std-fcc1: one entry 256 times) read 0x79 and 0x83, and the
 * one wide noise entry of three runs reads 0x00. Without it a train of
 * noise bursts with a radar's timing is a radar: noise10 matches ETSI 2.
 *
 * Again with this driver's detector (radar-b43.zip, at 5270 MHz on ch 52/80,
 * HackRF gain 15-47): 1, 1.1, 5 and 20 us pulses, CW and chirped by 20 MHz,
 * 0xc0 or more on 1197 of 1205 entries from the level the FIFOs first see
 * them up (the 8 others are entries beyond the pulses sent), and noise10
 * on 2 of 138. A pulse at the centre of the 80 MHz channel (5290 MHz) falls
 * on the receiver's DC: 1.1 us pulses read 0x4c-0x9d there, longer ones come
 * as two 0.2 us entries at their edges; at 1 MHz off the centre they read as
 * anywhere else.
 */
#define B43_RADAR_COHERENT	0xc0

/* A pulse older than the longest burst is dropped from the history. */
#define B43_RADAR_HISTORY_SPAN	B43_RADAR_US(100 * 1000)

struct b43_radar_type {
	const char *name;
	/* FIFO width bounds, 0.2 us units. */
	u16 wmin, wmax;
	/* Pulse repetition interval bounds, us. */
	u16 pri_min, pri_max;
	/* The longest burst of the type, ms. */
	u16 burst_ms;
	/* Pulses on one train to report the type. */
	u8 need;
};

/*
 * The width bounds take the standard's in tenths of a microsecond: the FIFO
 * reads 0-0.2 us over the width sent from 2 us up and 1-1.5 us pulses as
 * 1.2-2.0 us, so the bounds go 0.4 us below and 1.2 us above.
 */
#define B43_RADAR_WLO(t)	((t) / 2 > B43_RADAR_MIN_WIDTH + 2 ? \
				 (t) / 2 - 2 : B43_RADAR_MIN_WIDTH)
#define B43_RADAR_WHI(t)	((t) / 2 + 6)

#define B43_RADAR_TYPE(_name, _wlo, _whi, _pmin, _pmax, _burst, _need) { \
	.name = _name,							 \
	.wmin = B43_RADAR_WLO(_wlo), .wmax = B43_RADAR_WHI(_whi),	 \
	.pri_min = _pmin, .pri_max = _pmax,				 \
	.burst_ms = _burst, .need = _need,				 \
}

/*
 * The count to report is about half the fewest pulses a burst of the type
 * has: pulses go missing while the AP transmits, and the tests count a
 * detection per burst.
 *
 *            width us   PRI us      pulses   longest burst
 *   FCC 1     1         518-3066    18-102   18 x 3066 us
 *     (FCC 0, 1 us at 1428 us x 18, falls in FCC 1)
 *   FCC 2     1-5       150-230     23-29    29 x 230 us
 *   FCC 3     6-10      200-500     16-18    18 x 500 us
 *   FCC 4     11-20     200-500     12-16    16 x 500 us
 *   ETSI 1    0.5-5     1000-5000   10       10 x 5000 us (200-1000 pps)
 *   ETSI 2    0.5-15    625-5000    15       15 x 5000 us (200-1600 pps)
 *   ETSI 3    0.5-15    250-435     25       25 x 435 us (2300-4000 pps)
 *   ETSI 4    20-30     250-500     20       20 x 500 us (2000-4000 pps,
 *                                            chirped)
 */
static const struct b43_radar_type b43_radar_types[] = {
	B43_RADAR_TYPE("FCC 1",  10,  10,  518, 3066, 56,  9),
	B43_RADAR_TYPE("FCC 2",  10,  50,  150,  230,  7, 12),
	B43_RADAR_TYPE("FCC 3",  60, 100,  200,  500,  9,  8),
	B43_RADAR_TYPE("FCC 4", 110, 200,  200,  500,  8,  6),
	B43_RADAR_TYPE("ETSI 1",  5,  50, 1000, 5000, 50,  5),
	B43_RADAR_TYPE("ETSI 2",  5, 150,  625, 5000, 75,  8),
	B43_RADAR_TYPE("ETSI 3",  5, 150,  250,  435, 11, 13),
	B43_RADAR_TYPE("ETSI 4", 200, 300, 250,  500, 10, 10),
};

const char *b43_radar_type_name(const struct b43_radar_type *t)
{
	return t->name;
}

void b43_radar_reset(struct b43_radar *r)
{
	r->nfifo[0] = 0;
	r->nfifo[1] = 0;
	r->head = 0;
	r->n = 0;
}

bool b43_radar_entry(struct b43_radar *r, unsigned int fifo, const u16 w[4])
{
	struct b43_radar_pulse *p;

	if (r->nfifo[fifo] >= B43_RADAR_FIFO_PULSES)
		return false;
	p = &r->fifo[fifo][r->nfifo[fifo]++];
	/*
	 * Modulo 2^32 the shifted times keep their differences, which is all
	 * the matching takes of them.
	 */
	p->ts = ((u32)w[0] << 16 | (u32)(w[1] & 0x0fff) << 4 |
		 (w[3] & 0x000f)) << r->shift;
	p->width = (w[2] & 0x00ff) << r->shift;
	p->coherent = (w[2] >> 8) >= B43_RADAR_COHERENT;
	return true;
}

/* The i-th newest pulse of the history, 0 the newest. */
static const struct b43_radar_pulse *b43_radar_hist(const struct b43_radar *r,
						    unsigned int i)
{
	return &r->hist[(r->head + B43_RADAR_HISTORY - 1 - i) %
			B43_RADAR_HISTORY];
}

/*
 * Whether pulse q belongs on a train of type t that ends in p: its width
 * fits the type and is p's, within what the FIFO varies on one radar. A
 * 1 us pulse reads 1.2-2.0 us, and the wider of the two FIFOs' entries is
 * kept: 1.2 us is 6 units.
 */
static bool b43_radar_fits(const struct b43_radar_type *t,
			   const struct b43_radar_pulse *p,
			   const struct b43_radar_pulse *q)
{
	u16 d = q->width > p->width ? q->width - p->width
				    : p->width - q->width;

	return q->width >= t->wmin && q->width <= t->wmax &&
	       d <= 6 + p->width / 8;
}

/* The pulses on the train of interval pri that ends in the newest. */
static unsigned int b43_radar_train(const struct b43_radar *r,
				    const struct b43_radar_type *t, u32 pri)
{
	const struct b43_radar_pulse *p = b43_radar_hist(r, 0);
	u32 span = B43_RADAR_US((u32)t->burst_ms * 1000) + B43_RADAR_TOL;
	unsigned int i, count = 1;
	u32 last = 0;

	for (i = 1; i < r->n; i++) {
		const struct b43_radar_pulse *q = b43_radar_hist(r, i);
		u32 d = p->ts - q->ts;
		u32 k, off;

		if (d > span)
			break;
		k = (d + pri / 2) / pri;
		if (!k || k == last)
			continue;
		off = d > k * pri ? d - k * pri : k * pri - d;
		if (off > B43_RADAR_TOL + k * B43_RADAR_TOL_STEP)
			continue;
		if (!b43_radar_fits(t, p, q))
			continue;
		last = k;
		if (++count >= t->need)
			break;
	}
	return count;
}

/* Whether the newest pulse ends a train of type t. */
static bool b43_radar_type_seen(const struct b43_radar *r,
				const struct b43_radar_type *t)
{
	const struct b43_radar_pulse *p = b43_radar_hist(r, 0);
	u32 lo = B43_RADAR_US(t->pri_min) - B43_RADAR_TOL;
	u32 hi = B43_RADAR_US(t->pri_max) + B43_RADAR_TOL;
	unsigned int j;

	if (p->width < t->wmin || p->width > t->wmax)
		return false;
	for (j = 1; j < r->n; j++) {
		const struct b43_radar_pulse *q = b43_radar_hist(r, j);
		u32 pri = p->ts - q->ts;

		if (pri > hi)
			break;
		if (pri < lo || !b43_radar_fits(t, p, q))
			continue;
		if (b43_radar_train(r, t, pri) >= t->need)
			return true;
	}
	return false;
}

/* Add a pulse to the history and match the types on it. */
static const struct b43_radar_type *
b43_radar_add(struct b43_radar *r, const struct b43_radar_pulse *p)
{
	unsigned int i;

	if (p->width < B43_RADAR_MIN_WIDTH || !p->coherent)
		return NULL;
	/* Time runs forward; an entry out of order is a FIFO glitch. */
	if (r->n && (s32)(p->ts - b43_radar_hist(r, 0)->ts) <= 0)
		return NULL;
	/* The oldest first, so that a wrapped time never looks recent. */
	while (r->n &&
	       p->ts - b43_radar_hist(r, r->n - 1)->ts > B43_RADAR_HISTORY_SPAN)
		r->n--;

	r->hist[r->head] = *p;
	r->head = (r->head + 1) % B43_RADAR_HISTORY;
	if (r->n < B43_RADAR_HISTORY)
		r->n++;

	for (i = 0; i < ARRAY_SIZE(b43_radar_types); i++)
		if (b43_radar_type_seen(r, &b43_radar_types[i]))
			return &b43_radar_types[i];
	return NULL;
}

const struct b43_radar_type *b43_radar_match(struct b43_radar *r)
{
	const struct b43_radar_type *seen = NULL;
	struct b43_radar_pulse cur = { 0 };
	unsigned int i = 0, j = 0;
	bool have = false;

	/*
	 * Polls come every 150 ms and the longest burst lasts 75 ms: after a
	 * poll without a pulse nothing earlier can be on a train with what
	 * follows. Emptying the history then also keeps the 53.7 s wrap of
	 * the time from bringing old pulses back.
	 */
	if (!r->nfifo[0] && !r->nfifo[1]) {
		r->n = 0;
		return NULL;
	}

	/*
	 * Each FIFO's entries are in time order: merge the two, take the two
	 * FIFOs' entries of one pulse as one (the wider), and the two entries
	 * of a split pulse as one.
	 */
	while (!seen && (i < r->nfifo[0] || j < r->nfifo[1])) {
		const struct b43_radar_pulse *p;
		s32 d;

		if (j >= r->nfifo[1] ||
		    (i < r->nfifo[0] &&
		     (s32)(r->fifo[0][i].ts - r->fifo[1][j].ts) <= 0))
			p = &r->fifo[0][i++];
		else
			p = &r->fifo[1][j++];

		if (have) {
			d = (s32)(p->ts - cur.ts);
			if (d >= -(s32)B43_RADAR_SAME &&
			    d <= (s32)B43_RADAR_SAME) {
				if (p->width > cur.width)
					cur.width = p->width;
				cur.coherent |= p->coherent;
				continue;
			}
			if (cur.width >= B43_RADAR_SPLIT_WIDTH && d > 0 &&
			    d <= (s32)(cur.width * B43_RADAR_WIDTH_TICKS +
				       B43_RADAR_SPLIT_GAP)) {
				u32 w = d / B43_RADAR_WIDTH_TICKS + p->width;

				cur.width = w > 0xffff ? 0xffff : w;
				continue;
			}
			seen = b43_radar_add(r, &cur);
		}
		cur = *p;
		have = true;
	}
	if (!seen && have)
		seen = b43_radar_add(r, &cur);

	r->nfifo[0] = 0;
	r->nfifo[1] = 0;
	if (seen)
		r->n = 0;
	return seen;
}
