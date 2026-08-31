/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B43_PHY_AC_RXIQCAL_H_
#define B43_PHY_AC_RXIQCAL_H_

struct b43_wldev;

/*
 * Generic RX-IQ solver. Returns -EOPNOTSUPP while the register map is not
 * filled in, and has no callers yet. Rationale in rxiqcal_phy_ac.c.
 */
int b43_phy_ac_rxiqcal(struct b43_wldev *dev, u8 cal_type);

/*
 * Measure -- two rounds of 0x4000 samples, summed -- and program the RX-IQ
 * compensation coefficients for the cores in core_mask. The arithmetic and
 * the register map are confirmed bit-exactly against the agcombo capture
 * that records read values (docs/rxiq-cal-analysis.md). Expects the caller
 * to have the tone running and the measurement path configured.
 */
int b43_phy_ac_rx_iq_comp_update(struct b43_wldev *dev, u8 core_mask);

/*
 * Debug: run the 4-tone-mode RX-IQ measurement sequence and log the raw
 * accumulator values (i_pwr, q_pwr, iq_prod) per core.
 * Call after txpwr_by_index in set_channel.
 */
void b43_phy_ac_rxiq_est_debug(struct b43_wldev *dev);

/*
 * Radio-side setup of the rxiqcal measurement loopback, called per core
 * after rxgainctrl_regs in the set_channel flow. After
 * wlc_phy_rxcal_radio_setup_nphy, adapted to the 2069 radio.
 */
void b43_phy_ac_rxcal_radio_setup(struct b43_wldev *dev, u8 rx_core);

/*
 * PHY-side setup of the calibration tone generator, run once rather than per
 * core: a block of some 26 ops that touches both cores in a specific order
 * (first pass forward, second pass reversed) plus the 0x0393/0x0394/0x040f
 * configuration.
 */
void b43_phy_ac_rxcal_tone_setup(struct b43_wldev *dev);

/*
 * Arm the tone for one core: peek 0x0393, write 0x0394 = channel | core,
 * write 0x0393 = 0x8000. Called before gainctrl() for that core.
 */
void b43_phy_ac_rxcal_tone_arm(struct b43_wldev *dev, u8 rx_core);

/* Per-core RX-IQ cal: a fixed four-step sweep with settling. */
void b43_phy_ac_rxcal_gainctrl(struct b43_wldev *dev, u8 rx_core);

/*
 * Per-core cleanup after gainctrl: 14 PHY writes undoing rxgainctrl_regs and
 * 7 radio writes undoing rxcal_radio_setup. The caller runs two separate
 * loops, all cores' PHY then all cores' radio, to match the vendor order.
 */
void b43_phy_ac_rxcal_cleanup(struct b43_wldev *dev, u8 rx_core);
void b43_phy_ac_rxcal_radio_cleanup(struct b43_wldev *dev, u8 rx_core);

#endif /* B43_PHY_AC_RXIQCAL_H_ */
