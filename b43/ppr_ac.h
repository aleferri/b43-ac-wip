/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_PPR_AC_H_
#define B43_PPR_AC_H_

#include "phy_common.h"

struct ssb_sprom;

/*
 * Per-rate power table for the AC-PHY, built from the SROM revision 11 fields.
 *
 * Same units and primitives as b43's ppr.h -- one byte per entry in quarter
 * dBm -- but the entries are not rates. Each mcsbw{20,40,80}5g{l,m,h}po word
 * carries eight nibbles, and each nibble is the power offset of one modulation
 * and coding class, shared by every legacy OFDM rate and every MCS that uses
 * it:
 *
 *   group  class          OFDM rates       MCS
 *     0    BPSK, QPSK     6, 9, 12, 18     0, 1, 2
 *     1    16-QAM 1/2     24               3
 *     2    16-QAM 3/4     36               4
 *     3    64-QAM 2/3     48               5
 *     4    64-QAM 3/4     54               6
 *     5    64-QAM 5/6     --               7
 *     6    256-QAM 3/4    --               8
 *     7    256-QAM 5/6    --               9
 *
 * This is the decode `wl curpower` prints as "Board Limits" on the agcombo and
 * DSL-3580L dumps (docs/txpwr-target-derivation.md). The table therefore holds
 * one row of eight groups per bandwidth, and b43_ppr_ac_ofdm() translates a
 * legacy rate into its group. There is no CCK field for 5 GHz in rev 11.
 *
 * A channel loads the 20 MHz row at every width, the 40 MHz row from 40 MHz up
 * and the 80 MHz row at 80: those are the 20-in-40, 20-in-80 and 40-in-80 rate
 * groups of a bonded channel, and the reason the maximum over the table is
 * taken over every width the channel contains.
 */

#define B43_PPR_AC_GROUPS	8
#define B43_PPR_AC_ROWS		3	/* 20, 40, 80 MHz */
#define B43_PPR_AC_OFDM_RATES	8	/* 6, 9, 12, 18, 24, 36, 48, 54 Mb/s */

struct b43_ppr_ac {
	union {
		u8 __all[B43_PPR_AC_ROWS * B43_PPR_AC_GROUPS];
		u8 rows[B43_PPR_AC_ROWS][B43_PPR_AC_GROUPS];
	};
	/* Entries the current width loads: 8 at 20 MHz, 16 at 40, 24 at 80. */
	unsigned int num;
};

void b43_ppr_ac_clear(struct b43_ppr_ac *ppr, enum nl80211_chan_width width);
void b43_ppr_ac_add(struct b43_ppr_ac *ppr, int diff);
void b43_ppr_ac_apply_max(struct b43_ppr_ac *ppr, u8 max);
void b43_ppr_ac_apply_min(struct b43_ppr_ac *ppr, u8 min);
u8 b43_ppr_ac_get_max(const struct b43_ppr_ac *ppr);

/*
 * The power of one rate on the row of @width. @rate indexes the legacy OFDM
 * rates in bitrate order, 0 = 6 Mb/s to 7 = 54 Mb/s. A row the table did not
 * load reads as zero.
 */
u8 b43_ppr_ac_ofdm(const struct b43_ppr_ac *ppr, enum nl80211_chan_width width,
		   unsigned int rate);

/* The same for a VHT MCS, 0-9, on the row of @width. */
u8 b43_ppr_ac_vht(const struct b43_ppr_ac *ppr, enum nl80211_chan_width width,
		  unsigned int mcs);

bool b43_ppr_ac_sprom_has_subband_po(const struct ssb_sprom *sprom);

unsigned int b43_ppr_ac_subband(u16 chan, enum nl80211_chan_width width);
unsigned int b43_ppr_ac_po_band(u16 chan);
/* The highest entry of the row the width loads. */
u8 b43_ppr_ac_row_max(const struct b43_ppr_ac *ppr, enum nl80211_chan_width width);
u8 b43_ppr_ac_load_max_from_sprom(const struct ssb_sprom *sprom, u8 coremask,
				  unsigned int num_cores,
				  struct b43_ppr_ac *ppr, u16 chan,
				  enum nl80211_chan_width width, u8 ceiling);
void b43_ppr_ac_load_spacing(const struct ssb_sprom *sprom,
			     struct b43_ppr_ac *ppr, u16 chan,
			     enum nl80211_chan_width width);

#endif /* B43_PPR_AC_H_ */
