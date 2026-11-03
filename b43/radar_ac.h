/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_RADAR_AC_H_
#define B43_RADAR_AC_H_

#include <linux/types.h>

/*
 * The AC-PHY's radar pulse FIFOs (PHY 0x0253 and 0x0254, fill levels in
 * 0x0251 and 0x0252) hold four 16-bit words a pulse and 0x0600 words each.
 * Decoded against known pulses from a HackRF on the RT-AC66U B1, read back
 * under the stock driver with its radar on and under b43
 * (router-data/rt-ac66u-b1/radar-fifo.zip):
 *
 *   w0 15:0   time, bits 27:12, in 0.2 us units
 *   w1 11:0   time, bits 11:0, in 0.2 us units
 *   w1 15:12  moves with the pulse's frequency and chirp (not used)
 *   w2 7:0    width, in 0.2 us units
 *   w2 15:8   0xfd-0xff on every CW or chirped pulse at every level,
 *             0x00-0x87 on 31 of 32 noise pulses (see radar_ac.c)
 *   w3 3:0    time, in 1/16 of 0.2 us
 *   w3 7:4    2 on CW and chirped pulses, mostly 0 on noise (not used)
 *
 * The time is a 32-bit count of 12.5 ns that wraps every 53.7 s.
 */
#define B43_RADAR_FIFO_WORDS	0x0600
#define B43_RADAR_FIFO_PULSES	(B43_RADAR_FIFO_WORDS / 4)

/*
 * One pulse: its start in 12.5 ns units, its width in 0.2 us units, and
 * whether w2 15:8 reads as a radar pulse's.
 */
struct b43_radar_pulse {
	u32 ts;
	u16 width;
	bool coherent;
};

/* The pulses kept for the matching, about the longest burst's worth. */
#define B43_RADAR_HISTORY	128

struct b43_radar_type;

struct b43_radar {
	/* One poll's entries of each FIFO, decoded. */
	struct b43_radar_pulse fifo[2][B43_RADAR_FIFO_PULSES];
	unsigned int nfifo[2];
	/* The pulses seen, a ring with the newest at hist[head - 1]. */
	struct b43_radar_pulse hist[B43_RADAR_HISTORY];
	unsigned int head;
	unsigned int n;
};

void b43_radar_reset(struct b43_radar *r);

/* Decode one FIFO entry into r->fifo[fifo]; false when the poll's is full. */
bool b43_radar_entry(struct b43_radar *r, unsigned int fifo, const u16 w[4]);

/*
 * Match the poll's entries, collected with b43_radar_entry(), against the
 * radar types and empty them. Returns the type matched, NULL if none.
 */
const struct b43_radar_type *b43_radar_match(struct b43_radar *r);

const char *b43_radar_type_name(const struct b43_radar_type *t);

#endif /* B43_RADAR_AC_H_ */
