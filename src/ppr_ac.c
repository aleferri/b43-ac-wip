// SPDX-License-Identifier: GPL-2.0
/*
 * Per-rate power table for the AC-PHY, SROM revision 11.
 *
 * The primitives follow b43's ppr.c one for one. What differs is the load:
 * rev 11 has per-core maxp5ga[] over four 5 GHz sub-bands and the MCS power
 * offsets per bandwidth, and the table takes the minimum maxp over the active
 * cores as b43_ppr_load_max_from_sprom() does over its two.
 */

#include "b43.h"
#include "ppr_ac.h"

#define ppr_ac_for_each_entry(ppr, i, rate)				\
	for (i = 0, rate = (ppr)->__all_rates;				\
	     i < (ppr)->num;						\
	     i++, rate++)

static unsigned int b43_ppr_ac_num(enum nl80211_chan_width width)
{
	switch (width) {
	case NL80211_CHAN_WIDTH_80:
		return 4 * B43_PPR_AC_RATES;
	case NL80211_CHAN_WIDTH_40:
		return 3 * B43_PPR_AC_RATES;
	default:
		return 2 * B43_PPR_AC_RATES;
	}
}

void b43_ppr_ac_clear(struct b43_ppr_ac *ppr, enum nl80211_chan_width width)
{
	memset(ppr, 0, sizeof(*ppr));
	ppr->num = b43_ppr_ac_num(width);
}

void b43_ppr_ac_add(struct b43_ppr_ac *ppr, int diff)
{
	unsigned int i;
	u8 *rate;

	ppr_ac_for_each_entry(ppr, i, rate)
		*rate = (u8)clamp_t(int, *rate + diff, 0, 127);
}

void b43_ppr_ac_apply_max(struct b43_ppr_ac *ppr, u8 max)
{
	unsigned int i;
	u8 *rate;

	ppr_ac_for_each_entry(ppr, i, rate)
		*rate = min(*rate, max);
}

void b43_ppr_ac_apply_min(struct b43_ppr_ac *ppr, u8 min)
{
	unsigned int i;
	u8 *rate;

	ppr_ac_for_each_entry(ppr, i, rate)
		*rate = max(*rate, min);
}

/*
 * Rates the closed loop cannot regulate are not floored, they are switched
 * off: a target below the power the TSSI detector resolves would make the
 * loop chase a reading it does not have. Zero is the disabled marker, and
 * get_max() skips it by construction.
 */
void b43_ppr_ac_force_disabled(struct b43_ppr_ac *ppr, u8 threshold)
{
	unsigned int i;
	u8 *rate;

	ppr_ac_for_each_entry(ppr, i, rate)
		if (*rate < threshold)
			*rate = 0;
}

u8 b43_ppr_ac_get_max(const struct b43_ppr_ac *ppr)
{
	unsigned int i;
	u8 res = 0;

	for (i = 0; i < ppr->num; i++)
		res = max(ppr->__all_rates[i], res);

	return res;
}

/*
 * Sub-band index into maxp5ga[].
 *
 * Keyed on the primary channel, not the centre: at 80 MHz the captures follow
 * the primary, and using the centre puts ch36 one group too high.
 *
 * The first boundary depends on the width, 5210 at 20 and 80 MHz and 5250 at
 * 40. The sign of the residuals settles it: a regulatory limit can only lower
 * a target, so a residual where the driver comes out *below* the vendor cannot
 * be explained by the regulatory stage. At 40 MHz the 5210 boundary leaves
 * ch44 two units low, which nothing downstream could raise; 5250 leaves ch36
 * and ch52 two units high, which a limit explains, and every other 40 MHz
 * configuration exact.
 *
 * Deliberately not b43_phy_ac_pa5g_group(), which implements the
 * subband5gver=4 split at 5250 for every width and feeds the pa5ga
 * coefficients. The two partitions coincide at 40 MHz and differ at 20.
 *
 * The 20 MHz boundary rests on one board: it is pinned by ch40 giving 66 and
 * ch44 giving 64 with maxp5ga = {72, 70, ...}, and only the d6220 has those
 * two entries distinct.
 */
unsigned int b43_ppr_ac_subband(u16 chan, enum nl80211_chan_width width)
{
	u16 freq = 5000 + 5 * chan;
	u16 first = (width == NL80211_CHAN_WIDTH_40) ? 5250 : 5210;

	if (freq < first)
		return 0;
	if (freq < 5500)
		return 1;
	return 2;
}

static u8 b43_ppr_ac_off(u32 po, unsigned int mcs)
{
	return (u8)(((po >> (4 * mcs)) & 0xf) * 2);
}

static u8 b43_ppr_ac_sub(u8 maxp, u8 off)
{
	return off > maxp ? 0 : (u8)(maxp - off);
}

/*
 * Load the SROM limits for a channel. Returns the maxp the table was built
 * from, the minimum over the active cores, so a caller that wants a core's
 * own limit can add back (maxp5ga[core] - maxp) as the vendor does.
 *
 * The mcsbw*po words are selected by the l/m/h band of the primary channel
 * (below 52, below 100, the rest), which is not the maxp5ga sub-band above:
 * the two partitions are the SROM's, not this driver's, and they differ at
 * ch48-51 and ch100.
 *
 * The OFDM row has no field of its own in rev 11. It follows the 20 MHz MCS
 * row through the mapping the SHM per-rate offsets were inverted against:
 * 6, 9, 12 and 18 Mb/s on MCS0, then 24, 36, 48 and 54 on MCS1-4.
 */
u8 b43_ppr_ac_load_max_from_sprom(const struct ssb_sprom *sprom, u8 coremask,
				  unsigned int num_cores,
				  struct b43_ppr_ac *ppr, u16 chan,
				  enum nl80211_chan_width width)
{
	static const u8 ofdm_on_mcs[B43_PPR_AC_RATES] = { 0, 0, 0, 0, 1, 2, 3, 4 };
	const u32 po[3][3] = {
		{ sprom->mcsbw205glpo, sprom->mcsbw405glpo, sprom->mcsbw805glpo },
		{ sprom->mcsbw205gmpo, sprom->mcsbw405gmpo, sprom->mcsbw805gmpo },
		{ sprom->mcsbw205ghpo, sprom->mcsbw405ghpo, sprom->mcsbw805ghpo },
	};
	struct b43_ppr_ac_rates *rates = &ppr->rates;
	unsigned int sb = b43_ppr_ac_subband(chan, width);
	unsigned int band = chan < 52 ? 0 : chan < 100 ? 1 : 2;
	unsigned int c, i;
	u8 maxp = 0xff;

	b43_ppr_ac_clear(ppr, width);

	for (c = 0; c < num_cores && c < ARRAY_SIZE(sprom->core_pwr_info); c++) {
		if (!(coremask & (1u << c)))
			continue;
		maxp = min(maxp, sprom->core_pwr_info[c].maxp5ga[sb]);
	}
	if (maxp == 0xff)
		maxp = 0;

	for (i = 0; i < B43_PPR_AC_RATES; i++) {
		rates->mcs_20[i] = b43_ppr_ac_sub(maxp,
						  b43_ppr_ac_off(po[band][0], i));
		if (ppr->num > 2 * B43_PPR_AC_RATES)
			rates->mcs_40[i] = b43_ppr_ac_sub(maxp,
					b43_ppr_ac_off(po[band][1], i));
		if (ppr->num > 3 * B43_PPR_AC_RATES)
			rates->mcs_80[i] = b43_ppr_ac_sub(maxp,
					b43_ppr_ac_off(po[band][2], i));
	}
	for (i = 0; i < B43_PPR_AC_RATES; i++)
		rates->ofdm[i] = rates->mcs_20[ofdm_on_mcs[i]];

	return maxp;
}

/*
 * The rev 11 SROM also carries explicit offsets for the sub-band rows of a
 * bonded channel -- 20-in-40, 20-in-80/160, 40-in-80, duplicate legacy, and
 * the MCS 8-9 pair -- in low-rate and high-rate halves. They are all zero on
 * every board captured, so which nibble lands on which rate is not
 * established and the loader does not add them. A board where they are not
 * zero is one this table does not describe yet.
 */
bool b43_ppr_ac_sprom_has_subband_po(const struct ssb_sprom *sprom)
{
	unsigned int i;

	if (sprom->sb20in40hrpo || sprom->sb20in40lrpo ||
	    sprom->dot11agduphrpo || sprom->dot11agduplrpo)
		return true;
	for (i = 0; i < 3; i++)
		if (sprom->mcslr5gpo[i] || sprom->sb20in80and160hr5gpo[i] ||
		    sprom->sb20in80and160lr5gpo[i] || sprom->sb40and80hr5gpo[i] ||
		    sprom->sb40and80lr5gpo[i])
			return true;
	return false;
}
