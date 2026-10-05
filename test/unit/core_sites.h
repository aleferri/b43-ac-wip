/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Core work the stock driver runs inside the PHY's sequences, which b43 runs
 * from the core or from mac80211 at another time. The harness emits the
 * core's operations at the stock driver's point, so that the op-for-op
 * comparison keeps its order; main.c says what each site emits. Only work
 * whose owner and content are known.
 *
 * Force-included (-include) ahead of the driver, whose phy_ac.h defines
 * B43_AC_CORE_SITE() as a no-op.
 */
#ifndef B43_TEST_CORE_SITES_H_
#define B43_TEST_CORE_SITES_H_

struct b43_wldev;

enum b43_phy_ac_core_site {
	/* b43_wireless_core_reset(): the MACCONTROL write after a core reset */
	B43_AC_SITE_CORE_RESET,
	/* b43_upload_microcode(): the PSM jump to 0 before the upload */
	B43_AC_SITE_UCODE_LOAD,
	/* b43_upload_microcode() starting the PSM, then b43_gpio_init() */
	B43_AC_SITE_UCODE_START,
	/* the stock driver's last MAC toggle and the core reset of its down */
	B43_AC_SITE_CORE_DOWN,
	/* b43_adjust_opmode(): beacon promiscuity and filter bits, switch tail */
	B43_AC_SITE_OPMODE_FILTERS,
	/*
	 * Leaving the BSS on a down, INFRA off / DISCPMQ on then AP off, with
	 * the stock driver's MAC toggles around them: b43 sets the mode from
	 * remove_interface and keeps the MAC suspended through the down
	 */
	B43_AC_SITE_DOWN_OPMODE,
	B43_AC_SITE_DOWN_OPMODE_END,
	/* b43_security_init(): the key rows of the address match table */
	B43_AC_SITE_KEYS_CLEAR,
	/* b43_upload_card_macaddress() before the BSSID is known */
	B43_AC_SITE_MACFILTER_FIRST,
	/* b43_macfilter_set() of BSSID and station, with their flags */
	B43_AC_SITE_MACFILTER,
	/* b43_ac_cac_match_gate(false) from b43_op_config() */
	B43_AC_SITE_CAC_CLOSE,
	/* b43_update_templates() on start_ap */
	B43_AC_SITE_BEACON_START,
	/* b43_update_templates() between two watchdog counter passes */
	B43_AC_SITE_BEACON_WD,
};

void b43_phy_ac_core_site(struct b43_wldev *dev, enum b43_phy_ac_core_site site);

#define B43_AC_CORE_SITE(dev, site) \
	b43_phy_ac_core_site(dev, B43_AC_SITE_##site)

#endif /* B43_TEST_CORE_SITES_H_ */
