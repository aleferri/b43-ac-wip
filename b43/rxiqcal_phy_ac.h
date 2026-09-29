/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B43_PHY_AC_RXIQCAL_H_
#define B43_PHY_AC_RXIQCAL_H_

struct b43_wldev;

/*
 * Measure -- two rounds of 0x4000 samples, summed -- and program the RX-IQ
 * compensation coefficients for the cores in core_mask. The arithmetic and
 * the register map are confirmed bit-exactly against the agcombo capture
 * that records read values (docs/rxiq-cal-analysis.md). Expects the caller
 * to have the tone running and the measurement path configured.
 */
int b43_phy_ac_rxiqcal_comp_update(struct b43_wldev *dev, u8 core_mask);

/*
 * Debug: run the 4-tone-mode RX-IQ measurement sequence and log the raw
 * accumulator values (i_pwr, q_pwr, iq_prod) per core.
 * Call after txpwr_by_index in set_channel.
 */
void b43_phy_ac_rxiqcal_est_debug(struct b43_wldev *dev);

#endif /* B43_PHY_AC_RXIQCAL_H_ */
