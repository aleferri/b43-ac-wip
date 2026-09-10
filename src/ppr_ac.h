/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_PPR_AC_H_
#define B43_PPR_AC_H_

#include "phy_common.h"

struct ssb_sprom;

/*
 * Per-rate power table for the AC-PHY, built from the SROM revision 11 fields.
 *
 * Same shape and units as b43's ppr.h -- one byte per rate in quarter dBm, the
 * same primitives -- with the rows the rev 11 SROM describes. It has no CCK
 * field for 5 GHz and carries its MCS offsets per bandwidth (mcsbw{20,40,80}),
 * so a channel loads the OFDM row and the 20 MHz MCS row at every width, the
 * 40 MHz row from 40 MHz up and the 80 MHz row at 80: that is how a bonded
 * channel carries its 20-in-40, 20-in-80 and 40-in-80 rate groups, and why
 * the maximum over the table is taken over every width the channel contains.
 */

#define B43_PPR_AC_RATES	8

struct b43_ppr_ac_rates {
	u8 ofdm[B43_PPR_AC_RATES];	/* 6, 9, 12, 18, 24, 36, 48, 54 Mb/s */
	u8 mcs_20[B43_PPR_AC_RATES];	/* MCS 0-7 at 20 MHz, or 20-in-40/80 */
	u8 mcs_40[B43_PPR_AC_RATES];	/* MCS 0-7 at 40 MHz, or 40-in-80 */
	u8 mcs_80[B43_PPR_AC_RATES];	/* MCS 0-7 at 80 MHz */
};

#define B43_PPR_AC_RATES_NUM	(sizeof(struct b43_ppr_ac_rates))

struct b43_ppr_ac {
	union {
		u8 __all_rates[B43_PPR_AC_RATES_NUM];
		struct b43_ppr_ac_rates rates;
	};
	/* Entries the current width loads: 16 at 20 MHz, 24 at 40, 32 at 80. */
	unsigned int num;
};

void b43_ppr_ac_clear(struct b43_ppr_ac *ppr, enum nl80211_chan_width width);
void b43_ppr_ac_add(struct b43_ppr_ac *ppr, int diff);
void b43_ppr_ac_apply_max(struct b43_ppr_ac *ppr, u8 max);
void b43_ppr_ac_apply_min(struct b43_ppr_ac *ppr, u8 min);
u8 b43_ppr_ac_get_max(const struct b43_ppr_ac *ppr);
void b43_ppr_ac_force_disabled(struct b43_ppr_ac *ppr, u8 threshold);
bool b43_ppr_ac_sprom_has_subband_po(const struct ssb_sprom *sprom);

unsigned int b43_ppr_ac_subband(u16 chan, enum nl80211_chan_width width);
u8 b43_ppr_ac_load_max_from_sprom(const struct ssb_sprom *sprom, u8 coremask,
				  unsigned int num_cores,
				  struct b43_ppr_ac *ppr, u16 chan,
				  enum nl80211_chan_width width);

#endif /* B43_PPR_AC_H_ */
