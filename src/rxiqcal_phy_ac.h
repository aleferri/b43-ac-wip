/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B43_PHY_AC_RXIQCAL_H_
#define B43_PHY_AC_RXIQCAL_H_

struct b43_wldev;

/*
 * RX I/Q calibration (SKELETON). Struttura + solve generico da brcmsmac N-PHY;
 * register-map AC-PHY da riempire dalla trace. Ritorna -EOPNOTSUPP finche'
 * B43_PHY_AC_RXIQCAL_REGMAP_FILLED == 0. Fase B: non wirare in set_channel
 * prima che la RX base funzioni.
 */
int b43_phy_ac_rxiqcal(struct b43_wldev *dev, u8 cal_type);

/*
 * Misura (2 round da 0x4000 campioni, sommati) e programma i coefficienti di
 * compensazione RX-IQ per i core in core_mask. Matematica e regmap confermati
 * bit-exact contro la cattura agcombo con retval (docs/rxiq-cal-analysis.md).
 * Presuppone tone attivo e path di misura gia' configurati dal chiamante.
 */
int b43_phy_ac_rx_iq_comp_update(struct b43_wldev *dev, u8 core_mask);

/*
 * Debug: run the 4-tone-mode RX-IQ measurement sequence and log the raw
 * accumulator values (i_pwr, q_pwr, iq_prod) per core.
 * Call after txpwr_by_index in set_channel.
 */
void b43_phy_ac_rxiq_est_debug(struct b43_wldev *dev);

/*
 * Radio-side setup del loopback di misura per rxiqcal. Chiamata per-core
 * dopo rxgainctrl_regs nel flow di set_channel. Ref:
 * wlc_phy_rxcal_radio_setup_nphy (con adattamenti radio 2069).
 */
void b43_phy_ac_rxcal_radio_setup(struct b43_wldev *dev, u8 rx_core);

/*
 * Tone-setup pre-rxcal: PHY-side setup del generatore di tone di calibrazione.
 * Non è per-core: emette un blocco di ~26 op che tocca entrambi i core in
 * un ordine specifico (pass1 core-forward, pass2 core-reverse) più config di
 * 0x0393/0x0394/0x040f. Vendor #39709-#39734.
 */
void b43_phy_ac_rxcal_tone_setup(struct b43_wldev *dev);

/*
 * Tone arm per un core specifico: peek 0x0393 + WR 0x0394 (channel|core)
 * + WR 0x0393 = 0x8000 (arm bit). Chiamata prima di gainctrl(core).
 * Vendor #39732-#39734 (core 0), #39815-#39817 (core 1).
 */
void b43_phy_ac_rxcal_tone_arm(struct b43_wldev *dev, u8 rx_core);

/*
 * RX-IQ cal per-core: sweep 4-step con settling. Rimpiazza il vecchio
 * hill-climb N-PHY (che non matcha il pattern AC-PHY). Vendor 80 op/core.
 */
void b43_phy_ac_rxcal_gainctrl(struct b43_wldev *dev, u8 rx_core);

/*
 * Cleanup per-core dopo gainctrl. PHY: 14 WR (undo rxgainctrl_regs); Radio:
 * 7 WR (undo rxcal_radio_setup). Chiamati in due loop separati (all cores
 * PHY, then all cores radio) per matchare l'ordine vendor.
 */
void b43_phy_ac_rxcal_cleanup(struct b43_wldev *dev, u8 rx_core);
void b43_phy_ac_rxcal_radio_cleanup(struct b43_wldev *dev, u8 rx_core);

#endif /* B43_PHY_AC_RXIQCAL_H_ */
