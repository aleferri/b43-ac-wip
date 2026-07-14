/* SPDX-License-Identifier: GPL-2.0
 * Stub for phy_common.h: struct b43_phy_operations and any helper used
 * by phy_ac.c that would normally live here.
 */
#ifndef _STUB_PHY_COMMON_H
#define _STUB_PHY_COMMON_H

#include "b43.h"

/*
 * recalc_txpower return type (see drivers/net/wireless/broadcom/b43/
 * phy_common.h upstream). The scratch phy_ac op returns _DONE unless
 * it detects a state that requires an adjust_txpower follow-up.
 */
enum b43_txpwr_result {
	B43_TXPWR_RES_NEED_ADJUST,
	B43_TXPWR_RES_DONE,
};

/*
 * PHY operation vtable populated by every PHY type. Field ordering
 * mirrors upstream so partial designated initialisers in phy_ac.c
 * (e.g. `.switch_analog = ...`) resolve to the correct slot.
 */
struct b43_phy_operations {
	int  (*allocate)(struct b43_wldev *dev);
	void (*free)(struct b43_wldev *dev);
	void (*prepare_structs)(struct b43_wldev *dev);
	int  (*prepare_hardware)(struct b43_wldev *dev);
	int  (*init)(struct b43_wldev *dev);
	void (*exit)(struct b43_wldev *dev);

	u16  (*phy_read)(struct b43_wldev *dev, u16 reg);
	void (*phy_write)(struct b43_wldev *dev, u16 reg, u16 val);
	void (*phy_maskset)(struct b43_wldev *dev, u16 reg, u16 mask, u16 set);
	u16  (*radio_read)(struct b43_wldev *dev, u16 reg);
	void (*radio_write)(struct b43_wldev *dev, u16 reg, u16 val);

	bool (*supports_hwpctl)(struct b43_wldev *dev);
	void (*software_rfkill)(struct b43_wldev *dev, bool blocked);
	void (*switch_analog)(struct b43_wldev *dev, bool on);
	int  (*switch_channel)(struct b43_wldev *dev, unsigned int new_channel);
	unsigned int (*get_default_chan)(struct b43_wldev *dev);
	void (*set_rx_antenna)(struct b43_wldev *dev, int antenna);
	int  (*interf_mitigation)(struct b43_wldev *dev, int new_mode);

	enum b43_txpwr_result (*recalc_txpower)(struct b43_wldev *dev,
						bool ignore_tssi);
	void (*adjust_txpower)(struct b43_wldev *dev);

	void (*pwork_15sec)(struct b43_wldev *dev);
	void (*pwork_60sec)(struct b43_wldev *dev);
};

#endif
