// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver
 * AC-PHY radar pulse matching
 *
 * The pulses of the two radar FIFOs (see radar_ac.h) are matched against the
 * short-pulse radar test signals of ETSI EN 301 893 (reference DFS test
 * signals 1-6, Table D.4) and FCC KDB 905462 D02 (radar types 0-4 and 6,
 * Tables 5 and 7): a burst of pulses of one width at one pulse repetition
 * interval. A pulse
 * that fits a type's width is the end of a candidate train with every
 * earlier pulse in that type's PRI range; the train is the pulses at whole
 * multiples of that interval, so missed pulses leave gaps but do not break
 * it. A type is reported when its train reaches the type's count.
 *
 * A burst at a staggered PRF (ETSI 5 and 6) takes two or three intervals in
 * turn, so each third or second pulse is on a train at their sum: those
 * types are matched at that interval.
 *
 * FCC 6 hops over 475 MHz every 3 ms with 9 pulses a hop: each hop that
 * falls in the channel is a train of its own.
 *
 * FCC 5 (Table 6, long pulses) has no train: 8 to 20 bursts of 1 to 3
 * pulses 50-100 us wide over 12 s. Its bursts are counted over the last
 * 12 s of polls (b43_radar_long()).
 *
 * Only pulses that w2 15:8 says are coherent are matched (see
 * B43_RADAR_COHERENT), and only the types of the regulatory domain's DFS
 * region (struct b43_radar's region).
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
 * A CW pulse longer than about 27 us comes in parts 28.8 us apart: 50 us as
 * 27.0 and 21.2 us, 80 us as 27.0, 26.8 and 22.4 us, 100 us as 27.0, 26.8,
 * 26.8 and 13.8 us (router-data/rt-ac66u-b1/radar-long.zip; a chirped one
 * comes whole, see radar_ac.h). An entry is taken as the next part of a
 * pulse only when the part before it is a full 26.8-27.0 us and it starts
 * 28.8 us after that part, within B43_RADAR_SPLIT_TOL (the CW runs: within
 * 0.15 us): the next of two pulses that close is not a part.
 */
#define B43_RADAR_SPLIT_WIDTH	133
#define B43_RADAR_SPLIT_STEP	(B43_RADAR_US(28) + B43_RADAR_TICKS_PER_US * 8 / 10)
#define B43_RADAR_SPLIT_TOL	B43_RADAR_US(1)

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
 *
 * At 20 MHz fifo 1 reads 0xc0 or more on most noise entries too, and fifo 0
 * below it on short pulses. There an entry is also taken as coherent only
 * with w3 bit 5 set (router-data/rt-ac66u-b1/radar-20mhz.zip, 52/20): 0 of
 * the 142 entries of five noise runs, 2 to 30 us long, have both; 20-100
 * entries of each radar test signal do, one a pulse.
 */
#define B43_RADAR_COHERENT	0xc0

/* A pulse older than the longest burst is dropped from the history. */
#define B43_RADAR_HISTORY_SPAN	B43_RADAR_US(100 * 1000)

struct b43_radar_type {
	const char *name;
	enum b43_radar_region region;
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

#define B43_RADAR_TYPE(_region, _name, _wlo, _whi, _pmin, _pmax, _burst, \
		       _need) {						 \
	.name = _name, .region = B43_RADAR_##_region,			 \
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
 *   FCC 6     1         333         9 a hop  9 x 333 us (one hop, of a
 *                                            hop every 3 ms over 475 MHz)
 *   ETSI 1    0.5-5     1000-5000   10       10 x 5000 us (200-1000 pps)
 *   ETSI 2    0.5-15    625-5000    15       15 x 5000 us (200-1600 pps)
 *   ETSI 3    0.5-15    250-435     25       25 x 435 us (2300-4000 pps)
 *   ETSI 4    20-30     250-500     20       20 x 500 us (2000-4000 pps,
 *                                            chirped)
 *   ETSI 5    0.5-2     2500-3333   10       30 x 3333 us (300-400 pps,
 *                       x 2 or 3    a PRF    2 or 3 PRFs 20 pps apart)
 *   ETSI 6    0.5-2     833-2500    15       45 x 2500 us (400-1200 pps,
 *                       x 2 or 3    a PRF    2 or 3 PRFs 80 pps apart)
 *
 * ETSI 5 and 6 are matched at the sum of two or three of their intervals,
 * 5132-9399 us and 1726-6369 us with the PRFs that far apart, on the
 * pulses of one PRF; their longest bursts last 94 and 96 ms.
 */
static const struct b43_radar_type b43_radar_types[] = {
	B43_RADAR_TYPE(FCC,  "FCC 1",   10,  10,  518, 3066,  56,  9),
	B43_RADAR_TYPE(FCC,  "FCC 2",   10,  50,  150,  230,   7, 12),
	B43_RADAR_TYPE(FCC,  "FCC 3",   60, 100,  200,  500,   9,  8),
	B43_RADAR_TYPE(FCC,  "FCC 4",  110, 200,  200,  500,   8,  6),
	B43_RADAR_TYPE(FCC,  "FCC 6",   10,  10,  325,  341,   4,  6),
	B43_RADAR_TYPE(ETSI, "ETSI 1",   5,  50, 1000, 5000,  50,  5),
	B43_RADAR_TYPE(ETSI, "ETSI 2",   5, 150,  625, 5000,  75,  8),
	B43_RADAR_TYPE(ETSI, "ETSI 3",   5, 150,  250,  435,  11, 13),
	B43_RADAR_TYPE(ETSI, "ETSI 4", 200, 300,  250,  500,  10, 10),
	B43_RADAR_TYPE(ETSI, "ETSI 5",   5,  20, 5000, 9500,  95,  5),
	B43_RADAR_TYPE(ETSI, "ETSI 6",   5,  20, 1600, 6500,  97,  7),
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
	r->poll = 0;
	r->long_head = 0;
	r->long_n = 0;
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
	p->width = ((w[2] & 0x00ff) | (w[3] & 0x0010) << 4) << r->shift;
	p->coherent = (w[2] >> 8) >= B43_RADAR_COHERENT &&
		      (r->shift != 2 || (w[3] & 0x0020));
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

/*
 * FCC 5 (KDB 905462 D02 Table 6): over 12 s, 8 to 20 bursts, one in each
 * of as many equal parts of it; a burst is 1 to 3 pulses of one width,
 * 50-100 us, chirped by 5-20 MHz, 1000-2000 us apart. A chirped one comes
 * as one entry as long as it is, a CW one in parts (both measured,
 * router-data/rt-ac66u-b1/radar-fcc5.zip). A long pulse within
 * B43_RADAR_LONG_GAP of the previous one, in this poll or the one before,
 * is on its burst; any other starts a burst. The type is reported when
 * B43_RADAR_LONG_NEED bursts started in the last 12 s: with the fewest
 * bursts, 8, and one pulse a burst, 5 is what a detector that misses
 * three in ten still reaches four times in five.
 *
 * A poll's FCC 5 burst is at most 3 pulses of 4 parts in both FIFOs, 24
 * entries. A poll of more than B43_RADAR_LONG_BUSY entries does not count
 * its long pulses: random pulses up to 40 us wide at 2000 a second make
 * 2-3 long ones a second of parts of close short ones, and a carrier that
 * fills both FIFOs (twice in 180 s idle at 52/80, radar-fcc5.zip) reads
 * as runs of parts and long entries.
 */
static const struct b43_radar_type b43_radar_fcc5 = {
	.name = "FCC 5", .region = B43_RADAR_FCC,
	.wmin = 240, .wmax = 520,
};

#define B43_RADAR_LONG_GAP	B43_RADAR_US(2000 + 100 + 10)
#define B43_RADAR_LONG_POLLS	(12000 / B43_RADAR_POLL_MS)
#define B43_RADAR_LONG_NEED	5
#define B43_RADAR_LONG_BUSY	64

static bool b43_radar_long(struct b43_radar *r, const struct b43_radar_pulse *p)
{
	unsigned int last, i, count = 0;

	if (r->long_busy ||
	    p->width < b43_radar_fcc5.wmin || p->width > b43_radar_fcc5.wmax)
		return false;
	if (r->long_n) {
		last = r->long_poll[(r->long_head + B43_RADAR_LONG_BURSTS - 1) %
				    B43_RADAR_LONG_BURSTS];
		if (r->poll - last <= 1 &&
		    p->ts - r->long_ts <= B43_RADAR_LONG_GAP) {
			r->long_ts = p->ts;
			return false;
		}
	}
	r->long_ts = p->ts;
	r->long_poll[r->long_head] = r->poll;
	r->long_head = (r->long_head + 1) % B43_RADAR_LONG_BURSTS;
	if (r->long_n < B43_RADAR_LONG_BURSTS)
		r->long_n++;

	for (i = 0; i < r->long_n; i++)
		if (r->poll - r->long_poll[i] < B43_RADAR_LONG_POLLS)
			count++;
	if (count < B43_RADAR_LONG_NEED)
		return false;
	/* Start the ring over: the count above only reads long_poll[0..n-1] */
	r->long_head = 0;
	r->long_n = 0;
	return true;
}

/* Add a pulse to the history and match the types on it. */
static const struct b43_radar_type *
b43_radar_add(struct b43_radar *r, const struct b43_radar_pulse *p)
{
	unsigned int i;

	if (p->width < B43_RADAR_MIN_WIDTH || !p->coherent)
		return NULL;
	if (r->region != B43_RADAR_ETSI && b43_radar_long(r, p))
		return &b43_radar_fcc5;
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

	for (i = 0; i < ARRAY_SIZE(b43_radar_types); i++) {
		const struct b43_radar_type *t = &b43_radar_types[i];

		if (r->region != B43_RADAR_ALL && t->region != r->region)
			continue;
		if (b43_radar_type_seen(r, t))
			return t;
	}
	return NULL;
}

const struct b43_radar_type *b43_radar_match(struct b43_radar *r)
{
	const struct b43_radar_type *seen = NULL;
	struct b43_radar_pulse cur = { 0 };
	unsigned int i = 0, j = 0;
	/* The newest part of the pulse in cur: its start and width. */
	u32 part_ts = 0;
	u16 part_w = 0;
	bool have = false;

	r->poll++;
	r->long_busy = r->nfifo[0] + r->nfifo[1] > B43_RADAR_LONG_BUSY;

	/*
	 * Polls come every 150 ms and the longest train lasts 96 ms: after a
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
	 * FIFOs' entries of one pulse, or of one part of it, as one (the
	 * wider), and the parts of a split pulse as one.
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
			bool part;
			u32 w;

			d = (s32)(p->ts - part_ts);
			part = part_w >= B43_RADAR_SPLIT_WIDTH &&
			       d >= (s32)(B43_RADAR_SPLIT_STEP - B43_RADAR_SPLIT_TOL) &&
			       d <= (s32)(B43_RADAR_SPLIT_STEP + B43_RADAR_SPLIT_TOL);
			if (part ||
			    (d >= -(s32)B43_RADAR_SAME && d <= (s32)B43_RADAR_SAME)) {
				if (part) {
					part_ts = p->ts;
					part_w = p->width;
				} else if (p->width > part_w) {
					part_w = p->width;
				}
				w = (part_ts - cur.ts) / B43_RADAR_WIDTH_TICKS +
				    part_w;
				w = min_t(u32, w, 0xffff);
				if (w > cur.width)
					cur.width = w;
				cur.coherent |= p->coherent;
				continue;
			}
			seen = b43_radar_add(r, &cur);
		}
		cur = *p;
		part_ts = p->ts;
		part_w = p->width;
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
