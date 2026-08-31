/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B43_RADIO_2069_H_
#define B43_RADIO_2069_H_

#include <linux/types.h>

struct b43_wldev;

/* Per-core register routing for the 2069 radio. */

/* One PHY-side row in the per-channel table. */
/*
 * One row of the per-channel table for the 2069 radio.
 *
 * All 45 cluster-1 values of an entry, u16[2..46], are radio writes:
 *   radio_raw[39]  the first 39, offsets 4 to 80, registers 0x08e8 to 0x006f
 *   chan_raw6[6]   the r2069_chan_writes batch, offsets 82 to 92, registers
 *                  0x11a, 0x11b, 0x719, 0x630, 0x65c and 0x662
 *
 * These values are validated against the vendor sweep on all 16 BW20 channels
 * by reverse-tools/check_channeltab.py.
 */
struct b43_phy_ac_channeltab_e_radio2069 {
	u8  channel;
	u16 freq;
	u16 radio_raw[39];
	u16 chan_raw6[6];
	u16 phy_bw[6];
};

const struct b43_phy_ac_channeltab_e_radio2069 *
b43_phy_ac_get_channeltab_e_r2069(struct b43_wldev *dev, u16 freq);

void b43_radio_2069_channel_setup(struct b43_wldev *dev,
	const struct b43_phy_ac_channeltab_e_radio2069 *e);

void b43_radio_2069_rccal(struct b43_wldev *dev);

void b43_radio_2069_afecal(struct b43_wldev *dev);

void b43_radio_2069_init(struct b43_wldev *dev);

void b43_radio_2069_pwron(struct b43_wldev *dev);

void b43_radio_2069_afe_lpf_stage(struct b43_wldev *dev, u16 afe_728);

#endif /* B43_RADIO_2069_H_ */
