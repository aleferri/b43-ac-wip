// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The driver's phy_ac.c, with what only the harness needs and the static
 * helpers it reads:
 *
 *   - the RX-IQ estimator of rxiqcal_phy_ac.c, which takes the per-width RX
 *     gain fields from b43_phy_ac_rxgain_bw();
 *   - the PHY's share of three core writes the stock driver makes and b43
 *     does not: shm 0x00cc and 0x00ce of the BSS configuration and the
 *     probe-response PLCP of every template load (emit_core_bss_config()
 *     and the conf_tx passes in main.c). b43 leaves probe responses to
 *     mac80211, so its microcode never sends one from the template.
 */
#include "phy_ac.c"
#include "rxiqcal_phy_ac.c"
#include "test_harness.h"

/*
 * shm 0x00cc as the BSS configuration writes it: the chain mask of the setup
 * site in bits 8:6 and the constant 0x0004; see b43_phy_ac_bss_cc_update().
 */
u16 b43_phy_ac_bss_cc(struct b43_wldev *dev)
{
	u16 pair[2];

	b43_phy_ac_chain_pair(dev, B43_PHY_AC_CHAIN_SETUP, dev->phy.ac->cal_width,
			      pair);
	return (u16)(pair[0] << 6 | 0x0004);
}

/* shm 0x00ce of the BSS configuration, at the setup site. */
u16 b43_phy_ac_beacon_pwr_offset(struct b43_wldev *dev)
{
	return b43_phy_ac_beacon_pwr_offset_at(dev, B43_PHY_AC_CHAIN_SETUP);
}

/*
 * Probe response length plus FCS: a fixed part that grows with the width
 * (the HT/VHT elements), plus the SSID. The same length also sets 0x001e and
 * the BTL cells of the core's copy.
 */
static u16 b43_phy_ac_prb_rsp_len(enum nl80211_chan_width width,
				  unsigned int ssid_len)
{
	u16 fixed;

	switch (width) {
	case NL80211_CHAN_WIDTH_80:
		fixed = 279;
		break;
	case NL80211_CHAN_WIDTH_40:
		fixed = 278;
		break;
	default:
		fixed = 277;
		break;
	}
	return (u16)(fixed + ssid_len);
}

/*
 * Probe-response PLCP and its duration, for each of the eight OFDM rates,
 * with the arithmetic of brcms_c_compute_ofdm_plcp() and
 * brcms_c_calc_frame_time():
 *
 *   SIGNAL:   tmp = len << 5, plcp[0] = nibble_rate | (tmp & 0xff),
 *             plcp[1] = tmp >> 8, plcp[2] = tmp >> 16
 *   duration: 20 + ceil((len * 8 + 22) / NDBPS) * 4 + SIFS
 *
 * With len = 284 and SIFS = 16 this gives the 420, 292, 228, 164, 132, 100,
 * 84 and 80 us of cold01.
 *
 * @len is the probe response plus FCS. On hardware it should come from
 * B43_SHM_SH_PRTLEN (0x004a), where the core writes the template length
 * (0x0118 = 280 in the capture). The +1 per width doubling in
 * b43_phy_ac_prb_rsp_len() is unexplained: at 40 MHz the template measures
 * 284 bytes while SIGNAL says 285.
 */
static void b43_phy_ac_prb_rsp_plcp(struct b43_wldev *dev, u16 len)
{
	static const u16 ndbps[8] = { 24, 36, 48, 72, 96, 144, 192, 216 };
	unsigned int i;
	u32 tmp = (u32)(len & 0xfff) << 5;

	for (i = 0; i < ARRAY_SIZE(ndbps); i++) {
		u16 block = b43_phy_ac_rate_shm_offset(dev, i);
		u16 nsym = DIV_ROUND_UP(len * 8 + 22, ndbps[i]);
		u16 plcp01 = (u16)((b43_phy_ac_ofdm_dirmap[i] | (tmp & 0xff)) |
				   ((tmp >> 8 & 0xff) << 8));

		b43_shm_write16(dev, B43_SHM_SHARED,
				block + B43_AC_RT_PLCP, plcp01);
		b43_shm_write16(dev, B43_SHM_SHARED,
				block + B43_AC_RT_PLCP + 2,
				(u16)(tmp >> 16 & 0xff));
		b43_shm_write16(dev, B43_SHM_SHARED,
				block + B43_AC_RT_PLCP + 4,
				(u16)(20 + nsym * 4 + 16));
	}
}

/* One probe-response PLCP pass, for an SSID of @ssid_len bytes. */
void b43_phy_ac_prb_rsp_plcp_pass(struct b43_wldev *dev, unsigned int ssid_len)
{
	b43_phy_ac_prb_rsp_plcp(dev,
			b43_phy_ac_prb_rsp_len(dev->phy.ac->cal_width, ssid_len));
}
