// SPDX-License-Identifier: GPL-2.0-or-later
/*

  Broadcom B43 wireless driver

  Copyright (c) 2005 Martin Langer <martin-langer@gmx.de>
  Copyright (c) 2005 Stefano Brivio <stefano.brivio@polimi.it>
  Copyright (c) 2005-2009 Michael Buesch <m@bues.ch>
  Copyright (c) 2005 Danny van Dyk <kugelfang@gentoo.org>
  Copyright (c) 2005 Andreas Jaggi <andreas.jaggi@waterwave.ch>
  Copyright (c) 2010-2011 Rafał Miłecki <zajec5@gmail.com>

  SDIO support
  Copyright (c) 2009 Albert Herranz <albert_herranz@yahoo.es>

  Some parts of the code in this file are derived from the ipw2200
  driver  Copyright(c) 2003 - 2004 Intel Corporation.


*/

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/workqueue.h>
#include <linux/skbuff.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

#include "b43.h"
#include "main.h"
#include "debugfs.h"
#include "phy_common.h"
#include "phy_g.h"
#include "phy_n.h"
#include "dma.h"
#include "pio.h"
#include "sysfs.h"
#include "xmit.h"
#include "lo.h"
#include "sdio.h"
#include <linux/mmc/sdio_func.h>

MODULE_DESCRIPTION("Broadcom B43 wireless driver");
MODULE_AUTHOR("Martin Langer");
MODULE_AUTHOR("Stefano Brivio");
MODULE_AUTHOR("Michael Buesch");
MODULE_AUTHOR("Gábor Stefanik");
MODULE_AUTHOR("Rafał Miłecki");
MODULE_LICENSE("GPL");

MODULE_FIRMWARE("b43/ucode11.fw");
MODULE_FIRMWARE("b43/ucode13.fw");
MODULE_FIRMWARE("b43/ucode14.fw");
MODULE_FIRMWARE("b43/ucode15.fw");
MODULE_FIRMWARE("b43/ucode16_lp.fw");
MODULE_FIRMWARE("b43/ucode16_mimo.fw");
MODULE_FIRMWARE("b43/ucode24_lcn.fw");
MODULE_FIRMWARE("b43/ucode25_lcn.fw");
MODULE_FIRMWARE("b43/ucode25_mimo.fw");
MODULE_FIRMWARE("b43/ucode26_mimo.fw");
MODULE_FIRMWARE("b43/ucode29_mimo.fw");
MODULE_FIRMWARE("b43/ucode33_lcn40.fw");
MODULE_FIRMWARE("b43/ucode30_mimo.fw");
MODULE_FIRMWARE("b43/ucode5.fw");
MODULE_FIRMWARE("b43/ucode40.fw");
MODULE_FIRMWARE("b43/ucode42.fw");
MODULE_FIRMWARE("b43/ucode9.fw");

static int modparam_bad_frames_preempt;
module_param_named(bad_frames_preempt, modparam_bad_frames_preempt, int, 0444);
MODULE_PARM_DESC(bad_frames_preempt,
		 "enable(1) / disable(0) Bad Frames Preemption");

static char modparam_fwpostfix[16];
module_param_string(fwpostfix, modparam_fwpostfix, 16, 0444);
MODULE_PARM_DESC(fwpostfix, "Postfix for the .fw files to load.");

static int modparam_hwpctl;
module_param_named(hwpctl, modparam_hwpctl, int, 0444);
MODULE_PARM_DESC(hwpctl, "Enable hardware-side power control (default off)");

static int modparam_nohwcrypt;
module_param_named(nohwcrypt, modparam_nohwcrypt, int, 0444);
MODULE_PARM_DESC(nohwcrypt, "Disable hardware encryption.");

static int modparam_hwtkip;
module_param_named(hwtkip, modparam_hwtkip, int, 0444);
MODULE_PARM_DESC(hwtkip, "Enable hardware tkip.");

static int modparam_qos = 1;
module_param_named(qos, modparam_qos, int, 0444);
MODULE_PARM_DESC(qos, "Enable QOS support (default on)");

static int modparam_btcoex = 1;
module_param_named(btcoex, modparam_btcoex, int, 0444);
MODULE_PARM_DESC(btcoex, "Enable Bluetooth coexistence (default on)");

int b43_modparam_verbose = B43_VERBOSITY_DEFAULT;
module_param_named(verbose, b43_modparam_verbose, int, 0644);
MODULE_PARM_DESC(verbose, "Log message verbosity: 0=error, 1=warn, 2=info(default), 3=debug");

static int b43_modparam_pio;
module_param_named(pio, b43_modparam_pio, int, 0644);
MODULE_PARM_DESC(pio, "Use PIO accesses by default: 0=DMA, 1=PIO");

static int modparam_allhwsupport = !IS_ENABLED(CONFIG_BRCMSMAC);
module_param_named(allhwsupport, modparam_allhwsupport, int, 0444);
MODULE_PARM_DESC(allhwsupport, "Enable support for all hardware (even it if overlaps with the brcmsmac driver)");

#ifdef CONFIG_B43_BCMA
static const struct bcma_device_id b43_bcma_tbl[] = {
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x11, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x15, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x17, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x18, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x1C, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x1D, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x1E, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x28, BCMA_ANY_CLASS),
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, 0x2A, BCMA_ANY_CLASS),
	{},
};
MODULE_DEVICE_TABLE(bcma, b43_bcma_tbl);
#endif

#ifdef CONFIG_B43_SSB
static const struct ssb_device_id b43_ssb_tbl[] = {
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 5),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 6),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 7),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 9),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 10),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 11),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 12),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 13),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 15),
	SSB_DEVICE(SSB_VENDOR_BROADCOM, SSB_DEV_80211, 16),
	{},
};
MODULE_DEVICE_TABLE(ssb, b43_ssb_tbl);
#endif

/* Channel and ratetables are shared for all devices.
 * They can't be const, because ieee80211 puts some precalculated
 * data in there. This data is the same for all devices, so we don't
 * get concurrency issues */
#define RATETAB_ENT(_rateid, _flags) \
	{								\
		.bitrate	= B43_RATE_TO_BASE100KBPS(_rateid),	\
		.hw_value	= (_rateid),				\
		.flags		= (_flags),				\
	}

/*
 * NOTE: When changing this, sync with xmit.c's
 *	 b43_plcp_get_bitrate_idx_* functions!
 */
static struct ieee80211_rate __b43_ratetable[] = {
	RATETAB_ENT(B43_CCK_RATE_1MB, 0),
	RATETAB_ENT(B43_CCK_RATE_2MB, IEEE80211_RATE_SHORT_PREAMBLE),
	RATETAB_ENT(B43_CCK_RATE_5MB, IEEE80211_RATE_SHORT_PREAMBLE),
	RATETAB_ENT(B43_CCK_RATE_11MB, IEEE80211_RATE_SHORT_PREAMBLE),
	RATETAB_ENT(B43_OFDM_RATE_6MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_9MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_12MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_18MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_24MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_36MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_48MB, 0),
	RATETAB_ENT(B43_OFDM_RATE_54MB, 0),
};

#define b43_a_ratetable		(__b43_ratetable + 4)
#define b43_a_ratetable_size	8
#define b43_b_ratetable		(__b43_ratetable + 0)
#define b43_b_ratetable_size	4
#define b43_g_ratetable		(__b43_ratetable + 0)
#define b43_g_ratetable_size	12

#define CHAN2G(_channel, _freq, _flags) {			\
	.band			= NL80211_BAND_2GHZ,		\
	.center_freq		= (_freq),			\
	.hw_value		= (_channel),			\
	.flags			= (_flags),			\
	.max_antenna_gain	= 0,				\
	.max_power		= 30,				\
}
static struct ieee80211_channel b43_2ghz_chantable[] = {
	CHAN2G(1, 2412, 0),
	CHAN2G(2, 2417, 0),
	CHAN2G(3, 2422, 0),
	CHAN2G(4, 2427, 0),
	CHAN2G(5, 2432, 0),
	CHAN2G(6, 2437, 0),
	CHAN2G(7, 2442, 0),
	CHAN2G(8, 2447, 0),
	CHAN2G(9, 2452, 0),
	CHAN2G(10, 2457, 0),
	CHAN2G(11, 2462, 0),
	CHAN2G(12, 2467, 0),
	CHAN2G(13, 2472, 0),
	CHAN2G(14, 2484, 0),
};

/* No support for the last 3 channels (12, 13, 14) */
#define b43_2ghz_chantable_limited_size		11
#undef CHAN2G

#define CHAN4G(_channel, _flags) {				\
	.band			= NL80211_BAND_5GHZ,		\
	.center_freq		= 4000 + (5 * (_channel)),	\
	.hw_value		= (_channel),			\
	.flags			= (_flags),			\
	.max_antenna_gain	= 0,				\
	.max_power		= 30,				\
}
#define CHAN5G(_channel, _flags) {				\
	.band			= NL80211_BAND_5GHZ,		\
	.center_freq		= 5000 + (5 * (_channel)),	\
	.hw_value		= (_channel),			\
	.flags			= (_flags),			\
	.max_antenna_gain	= 0,				\
	.max_power		= 30,				\
}
static struct ieee80211_channel b43_5ghz_nphy_chantable[] = {
	CHAN4G(184, 0),		CHAN4G(186, 0),
	CHAN4G(188, 0),		CHAN4G(190, 0),
	CHAN4G(192, 0),		CHAN4G(194, 0),
	CHAN4G(196, 0),		CHAN4G(198, 0),
	CHAN4G(200, 0),		CHAN4G(202, 0),
	CHAN4G(204, 0),		CHAN4G(206, 0),
	CHAN4G(208, 0),		CHAN4G(210, 0),
	CHAN4G(212, 0),		CHAN4G(214, 0),
	CHAN4G(216, 0),		CHAN4G(218, 0),
	CHAN4G(220, 0),		CHAN4G(222, 0),
	CHAN4G(224, 0),		CHAN4G(226, 0),
	CHAN4G(228, 0),
	CHAN5G(32, 0),		CHAN5G(34, 0),
	CHAN5G(36, 0),		CHAN5G(38, 0),
	CHAN5G(40, 0),		CHAN5G(42, 0),
	CHAN5G(44, 0),		CHAN5G(46, 0),
	CHAN5G(48, 0),		CHAN5G(50, 0),
	CHAN5G(52, 0),		CHAN5G(54, 0),
	CHAN5G(56, 0),		CHAN5G(58, 0),
	CHAN5G(60, 0),		CHAN5G(62, 0),
	CHAN5G(64, 0),		CHAN5G(66, 0),
	CHAN5G(68, 0),		CHAN5G(70, 0),
	CHAN5G(72, 0),		CHAN5G(74, 0),
	CHAN5G(76, 0),		CHAN5G(78, 0),
	CHAN5G(80, 0),		CHAN5G(82, 0),
	CHAN5G(84, 0),		CHAN5G(86, 0),
	CHAN5G(88, 0),		CHAN5G(90, 0),
	CHAN5G(92, 0),		CHAN5G(94, 0),
	CHAN5G(96, 0),		CHAN5G(98, 0),
	CHAN5G(100, 0),		CHAN5G(102, 0),
	CHAN5G(104, 0),		CHAN5G(106, 0),
	CHAN5G(108, 0),		CHAN5G(110, 0),
	CHAN5G(112, 0),		CHAN5G(114, 0),
	CHAN5G(116, 0),		CHAN5G(118, 0),
	CHAN5G(120, 0),		CHAN5G(122, 0),
	CHAN5G(124, 0),		CHAN5G(126, 0),
	CHAN5G(128, 0),		CHAN5G(130, 0),
	CHAN5G(132, 0),		CHAN5G(134, 0),
	CHAN5G(136, 0),		CHAN5G(138, 0),
	CHAN5G(140, 0),		CHAN5G(142, 0),
	CHAN5G(144, 0),		CHAN5G(145, 0),
	CHAN5G(146, 0),		CHAN5G(147, 0),
	CHAN5G(148, 0),		CHAN5G(149, 0),
	CHAN5G(150, 0),		CHAN5G(151, 0),
	CHAN5G(152, 0),		CHAN5G(153, 0),
	CHAN5G(154, 0),		CHAN5G(155, 0),
	CHAN5G(156, 0),		CHAN5G(157, 0),
	CHAN5G(158, 0),		CHAN5G(159, 0),
	CHAN5G(160, 0),		CHAN5G(161, 0),
	CHAN5G(162, 0),		CHAN5G(163, 0),
	CHAN5G(164, 0),		CHAN5G(165, 0),
	CHAN5G(166, 0),		CHAN5G(168, 0),
	CHAN5G(170, 0),		CHAN5G(172, 0),
	CHAN5G(174, 0),		CHAN5G(176, 0),
	CHAN5G(178, 0),		CHAN5G(180, 0),
	CHAN5G(182, 0),
};

static struct ieee80211_channel b43_5ghz_nphy_chantable_limited[] = {
	CHAN5G(36, 0),		CHAN5G(40, 0),
	CHAN5G(44, 0),		CHAN5G(48, 0),
	CHAN5G(149, 0),		CHAN5G(153, 0),
	CHAN5G(157, 0),		CHAN5G(161, 0),
	CHAN5G(165, 0),
};

static struct ieee80211_channel b43_5ghz_aphy_chantable[] = {
	CHAN5G(34, 0),		CHAN5G(36, 0),
	CHAN5G(38, 0),		CHAN5G(40, 0),
	CHAN5G(42, 0),		CHAN5G(44, 0),
	CHAN5G(46, 0),		CHAN5G(48, 0),
	CHAN5G(52, 0),		CHAN5G(56, 0),
	CHAN5G(60, 0),		CHAN5G(64, 0),
	CHAN5G(100, 0),		CHAN5G(104, 0),
	CHAN5G(108, 0),		CHAN5G(112, 0),
	CHAN5G(116, 0),		CHAN5G(120, 0),
	CHAN5G(124, 0),		CHAN5G(128, 0),
	CHAN5G(132, 0),		CHAN5G(136, 0),
	CHAN5G(140, 0),		CHAN5G(149, 0),
	CHAN5G(153, 0),		CHAN5G(157, 0),
	CHAN5G(161, 0),		CHAN5G(165, 0),
	CHAN5G(184, 0),		CHAN5G(188, 0),
	CHAN5G(192, 0),		CHAN5G(196, 0),
	CHAN5G(200, 0),		CHAN5G(204, 0),
	CHAN5G(208, 0),		CHAN5G(212, 0),
	CHAN5G(216, 0),
};

/* 802.11ac UNII 20 MHz channels supported by the BCM4352/4360 AC-PHY. */
static struct ieee80211_channel b43_5ghz_acphy_chantable[] = {
	CHAN5G(36, 0),		CHAN5G(40, 0),
	CHAN5G(44, 0),		CHAN5G(48, 0),
	CHAN5G(52, 0),		CHAN5G(56, 0),
	CHAN5G(60, 0),		CHAN5G(64, 0),
	CHAN5G(100, 0),		CHAN5G(104, 0),
	CHAN5G(108, 0),		CHAN5G(112, 0),
	CHAN5G(116, 0),		CHAN5G(120, 0),
	CHAN5G(124, 0),		CHAN5G(128, 0),
	CHAN5G(132, 0),		CHAN5G(136, 0),
	CHAN5G(140, 0),		CHAN5G(144, 0),
	CHAN5G(149, 0),		CHAN5G(153, 0),
	CHAN5G(157, 0),		CHAN5G(161, 0),
	CHAN5G(165, 0),
};
#undef CHAN4G
#undef CHAN5G

static struct ieee80211_supported_band b43_band_5GHz_nphy = {
	.band		= NL80211_BAND_5GHZ,
	.channels	= b43_5ghz_nphy_chantable,
	.n_channels	= ARRAY_SIZE(b43_5ghz_nphy_chantable),
	.bitrates	= b43_a_ratetable,
	.n_bitrates	= b43_a_ratetable_size,
};

static struct ieee80211_supported_band b43_band_5GHz_nphy_limited = {
	.band		= NL80211_BAND_5GHZ,
	.channels	= b43_5ghz_nphy_chantable_limited,
	.n_channels	= ARRAY_SIZE(b43_5ghz_nphy_chantable_limited),
	.bitrates	= b43_a_ratetable,
	.n_bitrates	= b43_a_ratetable_size,
};

static struct ieee80211_supported_band b43_band_5GHz_aphy = {
	.band		= NL80211_BAND_5GHZ,
	.channels	= b43_5ghz_aphy_chantable,
	.n_channels	= ARRAY_SIZE(b43_5ghz_aphy_chantable),
	.bitrates	= b43_a_ratetable,
	.n_bitrates	= b43_a_ratetable_size,
};

static struct ieee80211_supported_band b43_band_5GHz_acphy = {
	.band		= NL80211_BAND_5GHZ,
	.channels	= b43_5ghz_acphy_chantable,
	.n_channels	= ARRAY_SIZE(b43_5ghz_acphy_chantable),
	.bitrates	= b43_a_ratetable,
	.n_bitrates	= b43_a_ratetable_size,
};

static struct ieee80211_supported_band b43_band_2GHz = {
	.band		= NL80211_BAND_2GHZ,
	.channels	= b43_2ghz_chantable,
	.n_channels	= ARRAY_SIZE(b43_2ghz_chantable),
	.bitrates	= b43_g_ratetable,
	.n_bitrates	= b43_g_ratetable_size,
};

static struct ieee80211_supported_band b43_band_2ghz_limited = {
	.band		= NL80211_BAND_2GHZ,
	.channels	= b43_2ghz_chantable,
	.n_channels	= b43_2ghz_chantable_limited_size,
	.bitrates	= b43_g_ratetable,
	.n_bitrates	= b43_g_ratetable_size,
};

static void b43_wireless_core_exit(struct b43_wldev *dev);
static int b43_wireless_core_init(struct b43_wldev *dev);
static struct b43_wldev * b43_wireless_core_stop(struct b43_wldev *dev);
static int b43_wireless_core_start(struct b43_wldev *dev);
static void b43_op_bss_info_changed(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *conf,
				    u64 changed);

static int b43_ratelimit(struct b43_wl *wl)
{
	if (!wl || !wl->current_dev)
		return 1;
	if (b43_status(wl->current_dev) < B43_STAT_STARTED)
		return 1;
	/* We are up and running.
	 * Ratelimit the messages to avoid DoS over the net. */
	return net_ratelimit();
}

void b43info(struct b43_wl *wl, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (b43_modparam_verbose < B43_VERBOSITY_INFO)
		return;
	if (!b43_ratelimit(wl))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_INFO "b43-%s: %pV",
	       (wl && wl->hw) ? wiphy_name(wl->hw->wiphy) : "wlan", &vaf);

	va_end(args);
}

void b43err(struct b43_wl *wl, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (b43_modparam_verbose < B43_VERBOSITY_ERROR)
		return;
	if (!b43_ratelimit(wl))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_ERR "b43-%s ERROR: %pV",
	       (wl && wl->hw) ? wiphy_name(wl->hw->wiphy) : "wlan", &vaf);

	va_end(args);
}

/*
 * An error that ends in a controller restart, and so happens at most once
 * per restart: not rate-limited, or the messages of the bring-up the restart
 * interrupted use up net_ratelimit()'s budget and the cause is never seen.
 */
void b43err_restart(struct b43_wl *wl, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (b43_modparam_verbose < B43_VERBOSITY_ERROR)
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_ERR "b43-%s ERROR: %pV",
	       (wl && wl->hw) ? wiphy_name(wl->hw->wiphy) : "wlan", &vaf);

	va_end(args);
}

void b43warn(struct b43_wl *wl, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (b43_modparam_verbose < B43_VERBOSITY_WARN)
		return;
	if (!b43_ratelimit(wl))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_WARNING "b43-%s warning: %pV",
	       (wl && wl->hw) ? wiphy_name(wl->hw->wiphy) : "wlan", &vaf);

	va_end(args);
}

void b43dbg(struct b43_wl *wl, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (b43_modparam_verbose < B43_VERBOSITY_DEBUG)
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_DEBUG "b43-%s debug: %pV",
	       (wl && wl->hw) ? wiphy_name(wl->hw->wiphy) : "wlan", &vaf);

	va_end(args);
}

static void b43_ram_write(struct b43_wldev *dev, u16 offset, u32 val)
{
	u32 macctl;

	B43_WARN_ON(offset % 4 != 0);

	macctl = b43_read32(dev, B43_MMIO_MACCTL);
	if (macctl & B43_MACCTL_BE)
		val = swab32(val);

	b43_write32(dev, B43_MMIO_RAM_CONTROL, offset);
	b43_write32(dev, B43_MMIO_RAM_DATA, val);
}

/*
 * Template RAM advances its address on every data write, and the AC cores'
 * stock driver writes each template that way: RAM_CONTROL once, then the
 * words. A sequence starts with next = U16_MAX and rewrites the control
 * only where the offsets stop being consecutive.
 */
struct b43_ram_seq {
	u16 next;
	bool swap;
};

static void b43_ram_seq_write(struct b43_wldev *dev, struct b43_ram_seq *seq,
			      u16 offset, u32 val)
{
	B43_WARN_ON(offset % 4 != 0);

	if (offset != seq->next)
		b43_write32(dev, B43_MMIO_RAM_CONTROL, offset);
	b43_write32(dev, B43_MMIO_RAM_DATA, seq->swap ? swab32(val) : val);
	seq->next = offset + sizeof(u32);
}

/* A template word: in sequence on the AC cores, one by one on the others. */
static void b43_tpl_write(struct b43_wldev *dev, struct b43_ram_seq *seq,
			  u16 offset, u32 val)
{
	if (seq)
		b43_ram_seq_write(dev, seq, offset, val);
	else
		b43_ram_write(dev, offset, val);
}

static inline void b43_shm_control_word(struct b43_wldev *dev,
					u16 routing, u16 offset)
{
	u32 control;

	/* "offset" is the WORD offset. */
	control = routing;
	control <<= 16;
	control |= offset;
	b43_write32(dev, B43_MMIO_SHM_CONTROL, control);
}

u32 b43_shm_read32(struct b43_wldev *dev, u16 routing, u16 offset)
{
	u32 ret;

	if (routing == B43_SHM_SHARED) {
		B43_WARN_ON(offset & 0x0001);
		if (offset & 0x0003) {
			/* Unaligned access */
			b43_shm_control_word(dev, routing, offset >> 2);
			ret = b43_read16(dev, B43_MMIO_SHM_DATA_UNALIGNED);
			b43_shm_control_word(dev, routing, (offset >> 2) + 1);
			ret |= ((u32)b43_read16(dev, B43_MMIO_SHM_DATA)) << 16;

			goto out;
		}
		offset >>= 2;
	}
	b43_shm_control_word(dev, routing, offset);
	ret = b43_read32(dev, B43_MMIO_SHM_DATA);
out:
	return ret;
}

u16 b43_shm_read16(struct b43_wldev *dev, u16 routing, u16 offset)
{
	u16 ret;

	if (routing == B43_SHM_SHARED) {
		B43_WARN_ON(offset & 0x0001);
		if (offset & 0x0003) {
			/* Unaligned access */
			b43_shm_control_word(dev, routing, offset >> 2);
			ret = b43_read16(dev, B43_MMIO_SHM_DATA_UNALIGNED);

			goto out;
		}
		offset >>= 2;
	}
	b43_shm_control_word(dev, routing, offset);
	ret = b43_read16(dev, B43_MMIO_SHM_DATA);
out:
	return ret;
}

void b43_shm_write32(struct b43_wldev *dev, u16 routing, u16 offset, u32 value)
{
	if (routing == B43_SHM_SHARED) {
		B43_WARN_ON(offset & 0x0001);
		if (offset & 0x0003) {
			/* Unaligned access */
			b43_shm_control_word(dev, routing, offset >> 2);
			b43_write16(dev, B43_MMIO_SHM_DATA_UNALIGNED,
				    value & 0xFFFF);
			b43_shm_control_word(dev, routing, (offset >> 2) + 1);
			b43_write16(dev, B43_MMIO_SHM_DATA,
				    (value >> 16) & 0xFFFF);
			return;
		}
		offset >>= 2;
	}
	b43_shm_control_word(dev, routing, offset);
	b43_write32(dev, B43_MMIO_SHM_DATA, value);
}

void b43_shm_write16(struct b43_wldev *dev, u16 routing, u16 offset, u16 value)
{
	if (routing == B43_SHM_SHARED) {
		B43_WARN_ON(offset & 0x0001);
		if (offset & 0x0003) {
			/* Unaligned access */
			b43_shm_control_word(dev, routing, offset >> 2);
			b43_write16(dev, B43_MMIO_SHM_DATA_UNALIGNED, value);
			return;
		}
		offset >>= 2;
	}
	b43_shm_control_word(dev, routing, offset);
	b43_write16(dev, B43_MMIO_SHM_DATA, value);
}

/* Read HostFlags */
u64 b43_hf_read(struct b43_wldev *dev)
{
	u64 ret;

	ret = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF3);
	ret <<= 16;
	ret |= b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF2);
	ret <<= 16;
	ret |= b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF1);

	return ret;
}

/* Write HostFlags */
void b43_hf_write(struct b43_wldev *dev, u64 value)
{
	u16 lo, mi, hi;

	lo = (value & 0x00000000FFFFULL);
	mi = (value & 0x0000FFFF0000ULL) >> 16;
	hi = (value & 0xFFFF00000000ULL) >> 32;
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF1, lo);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF2, mi);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF3, hi);
}

/* Read the firmware capabilities bitmask (Opensource firmware only) */
static u16 b43_fwcapa_read(struct b43_wldev *dev)
{
	B43_WARN_ON(!dev->fw.opensource);
	return b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_FWCAPA);
}

void b43_tsf_read(struct b43_wldev *dev, u64 *tsf)
{
	u32 low, high;

	B43_WARN_ON(dev->dev->core_rev < 3);

	/* The hardware guarantees us an atomic read, if we
	 * read the low register first. */
	low = b43_read32(dev, B43_MMIO_REV3PLUS_TSF_LOW);
	high = b43_read32(dev, B43_MMIO_REV3PLUS_TSF_HIGH);

	*tsf = high;
	*tsf <<= 32;
	*tsf |= low;
}

static void b43_time_lock(struct b43_wldev *dev)
{
	b43_maskset32(dev, B43_MMIO_MACCTL, ~0, B43_MACCTL_TBTTHOLD);
	/* Commit the write */
	b43_read32(dev, B43_MMIO_MACCTL);
}

static void b43_time_unlock(struct b43_wldev *dev)
{
	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_TBTTHOLD, 0);
	/* Commit the write */
	b43_read32(dev, B43_MMIO_MACCTL);
}

static void b43_tsf_write_locked(struct b43_wldev *dev, u64 tsf)
{
	u32 low, high;

	B43_WARN_ON(dev->dev->core_rev < 3);

	low = tsf;
	high = (tsf >> 32);
	/* The hardware guarantees us an atomic write, if we
	 * write the low register first. */
	b43_write32(dev, B43_MMIO_REV3PLUS_TSF_LOW, low);
	b43_write32(dev, B43_MMIO_REV3PLUS_TSF_HIGH, high);
}

void b43_tsf_write(struct b43_wldev *dev, u64 tsf)
{
	b43_time_lock(dev);
	b43_tsf_write_locked(dev, tsf);
	b43_time_unlock(dev);
}

/* Whether the address match table has the wide layout: 64 rows with a flags
 * half-word, rather than the plain address rows the older cores take.
 *
 * 42 is the revision the captures report, on all three boards they come from,
 * and the one on which the OEM driver's MACFILTER writer is never entered. 41
 * has not been seen and lower revisions are untested, so they keep the old
 * path: getting this boundary wrong downwards would leave the receive filter
 * unprogrammed, while getting it wrong upwards only leaves things as they are
 * today.
 *
 * The number is comparable with what the captures show directly: the value the
 * driver puts in B43_SHM_SH_WLCOREREV is this same field. */
static bool b43_amt_wide(struct b43_wldev *dev)
{
	return dev->dev->core_rev >= 42;
}

/*
 * One entry of the address match table.
 *
 * Same routing and the same address packing keymac_write() has always used --
 * B43_SHM_RCMTA, one word pair per index, low byte first -- with the flags
 * half-word the AC cores keep above the last two address bytes. Passing
 * B43_AMT_KEEP_FLAGS leaves the flags alone, which is what the callers that
 * only set an address want.
 *
 * The entry is two words at index * 2. The second is written 32 bits wide so
 * the flags go with it: writing 16 bits, as keymac_write() did, leaves
 * whatever the flags were. Both words are read before they are written, as
 * the stock driver does on every row at the bus; the flags B43_AMT_KEEP_FLAGS
 * keeps are the ones that read returns.
 */
#define B43_AMT_KEEP_FLAGS	0xffff

/* Rows available for keys and matches. The wide layout has 64, and reserves the
 * top two for the station address and the BSSID. */
static u16 b43_amt_entries(struct b43_wldev *dev)
{
	return b43_amt_wide(dev) ? B43_AMT_WIDE_ENTRIES : B43_NR_PAIRWISE_KEYS;
}

static void b43_amt_write(struct b43_wldev *dev, u16 index, const u8 *addr,
			  u16 flags)
{
	u32 lo = 0, hi = 0, old_hi;

	b43_shm_read32(dev, B43_SHM_RCMTA, (index * 2) + 0);
	old_hi = b43_shm_read32(dev, B43_SHM_RCMTA, (index * 2) + 1);

	if (addr) {
		lo = addr[0];
		lo |= ((u32)addr[1] << 8);
		lo |= ((u32)addr[2] << 16);
		lo |= ((u32)addr[3] << 24);
		hi = addr[4];
		hi |= ((u32)addr[5] << 8);
	}

	if (flags == B43_AMT_KEEP_FLAGS)
		hi |= old_hi & 0xffff0000;
	else
		hi |= (u32)flags << 16;

	b43_shm_write32(dev, B43_SHM_RCMTA, (index * 2) + 0, lo);
	b43_shm_write32(dev, B43_SHM_RCMTA, (index * 2) + 1, hi);
}

static
void b43_macfilter_set(struct b43_wldev *dev, u16 offset, const u8 *mac)
{
	static const u8 zero_addr[ETH_ALEN] = { 0 };
	u16 data;

	if (!mac)
		mac = zero_addr;

	/*
	 * On the wide layout this port is not what the hardware reads: the
	 * address match table has taken over, and the two rows at the top of
	 * it hold what the port used to. A capture with the OEM driver's writer
	 * for this port traced shows it never being entered, while the table is
	 * written throughout the bring-up.
	 */
	if (b43_amt_wide(dev)) {
		u16 index, flags;

		switch (offset) {
		case B43_MACFILTER_SELF:
			index = B43_AMT_SELF;
			flags = B43_AMT_F_SELF;
			break;
		case B43_MACFILTER_BSSID:
			index = B43_AMT_BSSID;
			flags = B43_AMT_F_BSSID;
			break;
		default:
			B43_WARN_ON(1);
			return;
		}
		b43_amt_write(dev, index, mac, flags);
		return;
	}

	offset |= 0x0020;
	b43_write16(dev, B43_MMIO_MACFILTER_CONTROL, offset);

	data = mac[0];
	data |= mac[1] << 8;
	b43_write16(dev, B43_MMIO_MACFILTER_DATA, data);
	data = mac[2];
	data |= mac[3] << 8;
	b43_write16(dev, B43_MMIO_MACFILTER_DATA, data);
	data = mac[4];
	data |= mac[5] << 8;
	b43_write16(dev, B43_MMIO_MACFILTER_DATA, data);
}

/* The station address in AC template RAM, padded to eight bytes. */
#define B43_TPL_AC_MACADDR	0x0048

static void b43_write_macaddr_template_ac(struct b43_wldev *dev, const u8 *mac)
{
	struct b43_ram_seq seq = {
		.next = U16_MAX,
		.swap = !!(b43_read32(dev, B43_MMIO_MACCTL) & B43_MACCTL_BE),
	};

	b43_ram_seq_write(dev, &seq, B43_TPL_AC_MACADDR,
			  (u32)mac[0] | (u32)mac[1] << 8 |
			  (u32)mac[2] << 16 | (u32)mac[3] << 24);
	b43_ram_seq_write(dev, &seq, B43_TPL_AC_MACADDR + sizeof(u32),
			  (u32)mac[4] | (u32)mac[5] << 8);
}

static void b43_write_mac_bssid_templates(struct b43_wldev *dev)
{
	const u8 *mac;
	const u8 *bssid;
	u8 mac_bssid[ETH_ALEN * 2];
	int i;
	u32 tmp;

	bssid = dev->wl->bssid;
	mac = dev->wl->mac_addr;

	b43_macfilter_set(dev, B43_MACFILTER_BSSID, bssid);

	/* On the AC the station address goes to B43_TPL_AC_MACADDR. */
	if (dev->phy.type == B43_PHYTYPE_AC)
		return;

	memcpy(mac_bssid, mac, ETH_ALEN);
	memcpy(mac_bssid + ETH_ALEN, bssid, ETH_ALEN);

	/* Write our MAC address and BSSID to template ram */
	for (i = 0; i < ARRAY_SIZE(mac_bssid); i += sizeof(u32)) {
		tmp = (u32) (mac_bssid[i + 0]);
		tmp |= (u32) (mac_bssid[i + 1]) << 8;
		tmp |= (u32) (mac_bssid[i + 2]) << 16;
		tmp |= (u32) (mac_bssid[i + 3]) << 24;
		b43_ram_write(dev, 0x20 + i, tmp);
	}
}

/* Shared-memory copy of the station address, AC PHY only. Three words at
 * B43_SHM_SH_AC_MACADDR, low byte of each address pair first, which is the
 * same packing b43_macfilter_set() uses for the MACFILTER port. */
static void b43_shm_macaddr_set(struct b43_wldev *dev, const u8 *mac)
{
	static const u8 zero_addr[ETH_ALEN] = { 0 };
	unsigned int i;

	if (dev->phy.type != B43_PHYTYPE_AC)
		return;
	if (!mac)
		mac = zero_addr;

	for (i = 0; i < ETH_ALEN; i += 2) {
		u16 data = mac[i] | (mac[i + 1] << 8);

		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_AC_MACADDR + i, data);
	}
}

static void b43_upload_card_macaddress(struct b43_wldev *dev)
{
	b43_write_mac_bssid_templates(dev);
	b43_macfilter_set(dev, B43_MACFILTER_SELF, dev->wl->mac_addr);
	if (dev->phy.type == B43_PHYTYPE_AC)
		b43_write_macaddr_template_ac(dev, dev->wl->mac_addr);
	b43_shm_macaddr_set(dev, dev->wl->mac_addr);
}

static void b43_set_slot_time(struct b43_wldev *dev, u16 slot_time)
{
	/* slot_time is in usec. */

	/*
	 * The shared-memory slot time was left alone here because writing it
	 * hurt the transmit rate on BCM4311, and that was never understood. It
	 * is still left alone on those cores. From revision 42 both are
	 * written on either band, the IFS slot register first: the bus
	 * captures of the agcombo (7.14) and of the MacBookAir6,1 (6.30) write
	 * 0x0207 and then 9 on 5 GHz.
	 */
	if (dev->dev->core_rev >= 42) {
		b43_write16(dev, B43_MMIO_IFSSLOT, 510 + slot_time);
		b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_SLOTT,
				slot_time);
		return;
	}

	if (b43_current_band(dev->wl) == NL80211_BAND_5GHZ)
		return;
	b43_write16(dev, B43_MMIO_IFSSLOT, 510 + slot_time);
}

static void b43_short_slot_timing_enable(struct b43_wldev *dev)
{
	b43_set_slot_time(dev, 9);
}

static void b43_short_slot_timing_disable(struct b43_wldev *dev)
{
	b43_set_slot_time(dev, 20);
}

/* DummyTransmission function, as documented on
 * https://bcm-v4.sipsolutions.net/802.11/DummyTransmission
 */
void b43_dummy_transmission(struct b43_wldev *dev, bool ofdm, bool pa_on)
{
	struct b43_phy *phy = &dev->phy;
	unsigned int i, max_loop;
	u16 value;
	u32 buffer[5] = {
		0x00000000,
		0x00D40000,
		0x00000000,
		0x01000000,
		0x00000000,
	};

	if (ofdm) {
		max_loop = 0x1E;
		buffer[0] = 0x000201CC;
	} else {
		max_loop = 0xFA;
		buffer[0] = 0x000B846E;
	}

	for (i = 0; i < 5; i++)
		b43_ram_write(dev, i * 4, buffer[i]);

	b43_write16(dev, B43_MMIO_XMTSEL, 0x0000);

	if (dev->dev->core_rev < 11)
		b43_write16(dev, B43_MMIO_WEPCTL, 0x0000);
	else
		b43_write16(dev, B43_MMIO_WEPCTL, 0x0100);

	value = (ofdm ? 0x41 : 0x40);
	b43_write16(dev, B43_MMIO_TXE0_PHYCTL, value);
	if (phy->type == B43_PHYTYPE_N || phy->type == B43_PHYTYPE_LP ||
	    phy->type == B43_PHYTYPE_LCN || phy->type == B43_PHYTYPE_AC)
		b43_write16(dev, B43_MMIO_TXE0_PHYCTL1, 0x1A02);

	b43_write16(dev, B43_MMIO_TXE0_WM_0, 0x0000);
	b43_write16(dev, B43_MMIO_TXE0_WM_1, 0x0000);

	b43_write16(dev, B43_MMIO_XMTTPLATETXPTR, 0x0000);
	b43_write16(dev, B43_MMIO_XMTTXCNT, 0x0014);
	b43_write16(dev, B43_MMIO_XMTSEL, 0x0826);
	b43_write16(dev, B43_MMIO_TXE0_CTL, 0x0000);

	if (!pa_on && phy->type == B43_PHYTYPE_N) {
		; /*b43_nphy_pa_override(dev, false) */
	}

	switch (phy->type) {
	case B43_PHYTYPE_N:
	case B43_PHYTYPE_LCN:
		b43_write16(dev, B43_MMIO_TXE0_AUX, 0x00D0);
		break;
	case B43_PHYTYPE_LP:
		b43_write16(dev, B43_MMIO_TXE0_AUX, 0x0050);
		break;
	default:
		b43_write16(dev, B43_MMIO_TXE0_AUX, 0x0030);
	}
	b43_read16(dev, B43_MMIO_TXE0_AUX);

	if (phy->radio_ver == 0x2050 && phy->radio_rev <= 0x5)
		b43_radio_write16(dev, 0x0051, 0x0017);
	for (i = 0x00; i < max_loop; i++) {
		value = b43_read16(dev, B43_MMIO_TXE0_STATUS);
		if (value & 0x0080)
			break;
		udelay(10);
	}
	for (i = 0x00; i < 0x0A; i++) {
		value = b43_read16(dev, B43_MMIO_TXE0_STATUS);
		if (value & 0x0400)
			break;
		udelay(10);
	}
	for (i = 0x00; i < 0x19; i++) {
		value = b43_read16(dev, B43_MMIO_IFSSTAT);
		if (!(value & 0x0100))
			break;
		udelay(10);
	}
	if (phy->radio_ver == 0x2050 && phy->radio_rev <= 0x5)
		b43_radio_write16(dev, 0x0051, 0x0037);
}

static void key_write(struct b43_wldev *dev,
		      u8 index, u8 algorithm, const u8 *key)
{
	unsigned int i;
	u32 offset;
	u16 value;
	u16 kidx;

	/* Key index/algo block. An empty slot of the AC ucode is all zero. */
	kidx = b43_kidx_to_fw(dev, index);
	if (dev->fw.hdr_format == B43_FW_HDR_AC &&
	    algorithm == B43_SEC_ALGO_NONE)
		value = 0;
	else
		value = ((kidx << 4) | algorithm);
	b43_shm_write16(dev, B43_SHM_SHARED,
			b43_shm_sh_keyidxblock(dev) + (kidx * 2), value);

	/* Write the key to the Key Table Pointer offset */
	offset = dev->ktp + (index * B43_SEC_KEYSIZE);
	for (i = 0; i < B43_SEC_KEYSIZE; i += 2) {
		value = key[i];
		value |= (u16) (key[i + 1]) << 8;
		b43_shm_write16(dev, B43_SHM_SHARED, offset + i, value);
	}
}

static void keymac_write(struct b43_wldev *dev, u8 index, const u8 *addr)
{
	u8 pairwise_keys_start = B43_NR_GROUP_KEYS * 2;

	if (b43_new_kidx_api(dev))
		pairwise_keys_start = B43_NR_GROUP_KEYS;

	B43_WARN_ON(index < pairwise_keys_start);
	/* We have four default TX keys and possibly four default RX keys.
	 * Physical mac 0 is mapped to physical key 4 or 8, depending
	 * on the firmware version.
	 * So we must adjust the index here.
	 */
	index -= pairwise_keys_start;
	B43_WARN_ON(index >= b43_amt_entries(dev));

	/* Receive match transmitter address (RCMTA) mechanism. Keep the flags:
	 * on the wide layout the self and BSSID entries carry them, and a key
	 * row has no business changing what is above its address. */
	b43_amt_write(dev, index, addr, B43_AMT_KEEP_FLAGS);
}

/* The ucode will use phase1 key with TEK key to decrypt rx packets.
 * When a packet is received, the iv32 is checked.
 * - if it doesn't the packet is returned without modification (and software
 *   decryption can be done). That's what happen when iv16 wrap.
 * - if it does, the rc4 key is computed, and decryption is tried.
 *   Either it will success and B43_RX_MAC_DEC is returned,
 *   either it fails and B43_RX_MAC_DEC|B43_RX_MAC_DECERR is returned
 *   and the packet is not usable (it got modified by the ucode).
 * So in order to never have B43_RX_MAC_DECERR, we should provide
 * a iv32 and phase1key that match. Because we drop packets in case of
 * B43_RX_MAC_DECERR, if we have a correct iv32 but a wrong phase1key, all
 * packets will be lost without higher layer knowing (ie no resync possible
 * until next wrap).
 *
 * NOTE : this should support 50 key like RCMTA because
 * (B43_SHM_SH_KEYIDXBLOCK - B43_SHM_SH_TKIPTSCTTAK)/14 = 50
 */
static void rx_tkip_phase1_write(struct b43_wldev *dev, u8 index, u32 iv32,
		u16 *phase1key)
{
	unsigned int i;
	u32 offset;
	u8 pairwise_keys_start = B43_NR_GROUP_KEYS * 2;

	if (!modparam_hwtkip)
		return;

	if (b43_new_kidx_api(dev))
		pairwise_keys_start = B43_NR_GROUP_KEYS;

	B43_WARN_ON(index < pairwise_keys_start);
	/* We have four default TX keys and possibly four default RX keys.
	 * Physical mac 0 is mapped to physical key 4 or 8, depending
	 * on the firmware version.
	 * So we must adjust the index here.
	 */
	index -= pairwise_keys_start;
	B43_WARN_ON(index >= B43_NR_PAIRWISE_KEYS);

	if (b43_debug(dev, B43_DBG_KEYS)) {
		b43dbg(dev->wl, "rx_tkip_phase1_write : idx 0x%x, iv32 0x%x\n",
				index, iv32);
	}
	/* Write the key to the  RX tkip shared mem */
	offset = B43_SHM_SH_TKIPTSCTTAK + index * (10 + 4);
	for (i = 0; i < 10; i += 2) {
		b43_shm_write16(dev, B43_SHM_SHARED, offset + i,
				phase1key ? phase1key[i / 2] : 0);
	}
	b43_shm_write16(dev, B43_SHM_SHARED, offset + i, iv32);
	b43_shm_write16(dev, B43_SHM_SHARED, offset + i + 2, iv32 >> 16);
}

static void b43_op_update_tkip_key(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_key_conf *keyconf,
				   struct ieee80211_sta *sta,
				   u32 iv32, u16 *phase1key)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;
	int index = keyconf->hw_key_idx;

	if (B43_WARN_ON(!modparam_hwtkip))
		return;

	/* This is only called from the RX path through mac80211, where
	 * our mutex is already locked. */
	B43_WARN_ON(!mutex_is_locked(&wl->mutex));
	dev = wl->current_dev;
	B43_WARN_ON(!dev || b43_status(dev) < B43_STAT_INITIALIZED);

	keymac_write(dev, index, NULL);	/* First zero out mac to avoid race */

	rx_tkip_phase1_write(dev, index, iv32, phase1key);
	/* only pairwise TKIP keys are supported right now */
	if (WARN_ON(!sta))
		return;
	keymac_write(dev, index, sta->addr);
}

static void do_key_write(struct b43_wldev *dev,
			 u8 index, u8 algorithm,
			 const u8 *key, size_t key_len, const u8 *mac_addr)
{
	u8 buf[B43_SEC_KEYSIZE] = { 0, };
	u8 pairwise_keys_start = B43_NR_GROUP_KEYS * 2;

	if (b43_new_kidx_api(dev))
		pairwise_keys_start = B43_NR_GROUP_KEYS;

	B43_WARN_ON(index >= ARRAY_SIZE(dev->key));
	B43_WARN_ON(key_len > B43_SEC_KEYSIZE);

	/*
	 * First zero out mac, so that the old address does not match with the
	 * new key. A clear writes the zero address at the end anyway, and the
	 * stock driver writes such a row once.
	 */
	if (index >= pairwise_keys_start && mac_addr)
		keymac_write(dev, index, NULL);
	if (algorithm == B43_SEC_ALGO_TKIP) {
		/*
		 * We should provide an initial iv32, phase1key pair.
		 * We could start with iv32=0 and compute the corresponding
		 * phase1key, but this means calling ieee80211_get_tkip_key
		 * with a fake skb (or export other tkip function).
		 * Because we are lazy we hope iv32 won't start with
		 * 0xffffffff and let's b43_op_update_tkip_key provide a
		 * correct pair.
		 */
		rx_tkip_phase1_write(dev, index, 0xffffffff, (u16*)buf);
	} else if (index >= pairwise_keys_start) /* clear it */
		rx_tkip_phase1_write(dev, index, 0, NULL);
	if (key)
		memcpy(buf, key, key_len);
	key_write(dev, index, algorithm, buf);
	if (index >= pairwise_keys_start)
		keymac_write(dev, index, mac_addr);

	dev->key[index].algorithm = algorithm;
}

static int b43_key_write(struct b43_wldev *dev,
			 int index, u8 algorithm,
			 const u8 *key, size_t key_len,
			 const u8 *mac_addr,
			 struct ieee80211_key_conf *keyconf)
{
	int i;
	int pairwise_keys_start;

	/* For ALG_TKIP the key is encoded as a 256-bit (32 byte) data block:
	 * 	- Temporal Encryption Key (128 bits)
	 * 	- Temporal Authenticator Tx MIC Key (64 bits)
	 * 	- Temporal Authenticator Rx MIC Key (64 bits)
	 *
	 * 	Hardware only store TEK
	 */
	if (algorithm == B43_SEC_ALGO_TKIP && key_len == 32)
		key_len = 16;
	if (key_len > B43_SEC_KEYSIZE)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(dev->key); i++) {
		/* Check that we don't already have this key. */
		B43_WARN_ON(dev->key[i].keyconf == keyconf);
	}
	if (index < 0) {
		/* Pairwise key. Get an empty slot for the key. */
		if (b43_new_kidx_api(dev))
			pairwise_keys_start = B43_NR_GROUP_KEYS;
		else
			pairwise_keys_start = B43_NR_GROUP_KEYS * 2;
		for (i = pairwise_keys_start;
		     i < pairwise_keys_start + B43_NR_PAIRWISE_KEYS;
		     i++) {
			B43_WARN_ON(i >= ARRAY_SIZE(dev->key));
			if (!dev->key[i].keyconf) {
				/* found empty */
				index = i;
				break;
			}
		}
		if (index < 0) {
			b43warn(dev->wl, "Out of hardware key memory\n");
			return -ENOSPC;
		}
	} else
		B43_WARN_ON(index > 3);

	do_key_write(dev, index, algorithm, key, key_len, mac_addr);
	if ((index <= 3) && !b43_new_kidx_api(dev)) {
		/* Default RX key */
		B43_WARN_ON(mac_addr);
		do_key_write(dev, index + 4, algorithm, key, key_len, NULL);
	}
	keyconf->hw_key_idx = index;
	dev->key[index].keyconf = keyconf;

	return 0;
}

static int b43_key_clear(struct b43_wldev *dev, int index)
{
	if (B43_WARN_ON((index < 0) || (index >= ARRAY_SIZE(dev->key))))
		return -EINVAL;
	do_key_write(dev, index, B43_SEC_ALGO_NONE,
		     NULL, B43_SEC_KEYSIZE, NULL);
	if ((index <= 3) && !b43_new_kidx_api(dev)) {
		do_key_write(dev, index + 4, B43_SEC_ALGO_NONE,
			     NULL, B43_SEC_KEYSIZE, NULL);
	}
	dev->key[index].keyconf = NULL;

	return 0;
}

static void b43_clear_keys(struct b43_wldev *dev)
{
	int i, count;

	if (b43_new_kidx_api(dev))
		count = B43_NR_GROUP_KEYS + B43_NR_PAIRWISE_KEYS;
	else
		count = B43_NR_GROUP_KEYS * 2 + B43_NR_PAIRWISE_KEYS;
	for (i = 0; i < count; i++)
		b43_key_clear(dev, i);
}

static void b43_dump_keymemory(struct b43_wldev *dev)
{
	unsigned int i, index, count, offset, pairwise_keys_start;
	u8 mac[ETH_ALEN];
	u16 algo;
	u32 rcmta0;
	u16 rcmta1;
	u64 hf;
	struct b43_key *key;

	if (!b43_debug(dev, B43_DBG_KEYS))
		return;

	hf = b43_hf_read(dev);
	b43dbg(dev->wl, "Hardware key memory dump:  USEDEFKEYS=%u\n",
	       !!(hf & B43_HF_USEDEFKEYS));
	if (b43_new_kidx_api(dev)) {
		pairwise_keys_start = B43_NR_GROUP_KEYS;
		count = B43_NR_GROUP_KEYS + B43_NR_PAIRWISE_KEYS;
	} else {
		pairwise_keys_start = B43_NR_GROUP_KEYS * 2;
		count = B43_NR_GROUP_KEYS * 2 + B43_NR_PAIRWISE_KEYS;
	}
	for (index = 0; index < count; index++) {
		key = &(dev->key[index]);
		printk(KERN_DEBUG "Key slot %02u: %s",
		       index, (key->keyconf == NULL) ? " " : "*");
		offset = dev->ktp + (index * B43_SEC_KEYSIZE);
		for (i = 0; i < B43_SEC_KEYSIZE; i += 2) {
			u16 tmp = b43_shm_read16(dev, B43_SHM_SHARED, offset + i);
			printk("%02X%02X", (tmp & 0xFF), ((tmp >> 8) & 0xFF));
		}

		algo = b43_shm_read16(dev, B43_SHM_SHARED,
				      B43_SHM_SH_KEYIDXBLOCK + (index * 2));
		printk("   Algo: %04X/%02X", algo, key->algorithm);

		if (index >= pairwise_keys_start) {
			if (key->algorithm == B43_SEC_ALGO_TKIP) {
				printk("   TKIP: ");
				offset = B43_SHM_SH_TKIPTSCTTAK + (index - 4) * (10 + 4);
				for (i = 0; i < 14; i += 2) {
					u16 tmp = b43_shm_read16(dev, B43_SHM_SHARED, offset + i);
					printk("%02X%02X", (tmp & 0xFF), ((tmp >> 8) & 0xFF));
				}
			}
			rcmta0 = b43_shm_read32(dev, B43_SHM_RCMTA,
						((index - pairwise_keys_start) * 2) + 0);
			rcmta1 = b43_shm_read16(dev, B43_SHM_RCMTA,
						((index - pairwise_keys_start) * 2) + 1);
			*((__le32 *)(&mac[0])) = cpu_to_le32(rcmta0);
			*((__le16 *)(&mac[4])) = cpu_to_le16(rcmta1);
			printk("   MAC: %pM", mac);
		} else
			printk("   DEFAULT KEY");
		printk("\n");
	}
}

void b43_power_saving_ctl_bits(struct b43_wldev *dev, unsigned int ps_flags)
{
	u32 macctl;
	u16 ucstat;
	bool hwps;
	bool awake;
	int i;

	B43_WARN_ON((ps_flags & B43_PS_ENABLED) &&
		    (ps_flags & B43_PS_DISABLED));
	B43_WARN_ON((ps_flags & B43_PS_AWAKE) && (ps_flags & B43_PS_ASLEEP));

	if (ps_flags & B43_PS_ENABLED) {
		hwps = true;
	} else if (ps_flags & B43_PS_DISABLED) {
		hwps = false;
	} else {
		//TODO: If powersave is not off and FIXME is not set and we are not in adhoc
		//      and thus is not an AP and we are associated, set bit 25
	}
	if (ps_flags & B43_PS_AWAKE) {
		awake = true;
	} else if (ps_flags & B43_PS_ASLEEP) {
		awake = false;
	} else {
		//TODO: If the device is awake or this is an AP, or we are scanning, or FIXME,
		//      or we are associated, or FIXME, or the latest PS-Poll packet sent was
		//      successful, set bit26
	}

/* FIXME: For now we force awake-on and hwps-off */
	hwps = false;
	awake = true;

	macctl = b43_read32(dev, B43_MMIO_MACCTL);
	if (hwps)
		macctl |= B43_MACCTL_HWPS;
	else
		macctl &= ~B43_MACCTL_HWPS;
	if (awake)
		macctl |= B43_MACCTL_AWAKE;
	else
		macctl &= ~B43_MACCTL_AWAKE;
	b43_write32(dev, B43_MMIO_MACCTL, macctl);
	/* Commit write */
	b43_read32(dev, B43_MMIO_MACCTL);
	if (awake && dev->dev->core_rev >= 5) {
		/* Wait for the microcode to wake up. */
		for (i = 0; i < 100; i++) {
			ucstat = b43_shm_read16(dev, B43_SHM_SHARED,
						B43_SHM_SH_UCODESTAT);
			if (ucstat != B43_SHM_SH_UCODESTAT_SLEEP)
				break;
			udelay(10);
		}
	}
}

/* https://bcm-v4.sipsolutions.net/802.11/PHY/BmacCorePllReset */
void b43_wireless_core_phy_pll_reset(struct b43_wldev *dev)
{
	struct bcma_drv_cc *bcma_cc __maybe_unused;
	struct ssb_chipcommon *ssb_cc __maybe_unused;

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		bcma_cc = &dev->dev->bdev->bus->drv_cc;

		bcma_cc_write32(bcma_cc, BCMA_CC_PMU_CHIPCTL_ADDR, 0);
		bcma_cc_mask32(bcma_cc, BCMA_CC_PMU_CHIPCTL_DATA, ~0x4);
		bcma_cc_set32(bcma_cc, BCMA_CC_PMU_CHIPCTL_DATA, 0x4);
		bcma_cc_mask32(bcma_cc, BCMA_CC_PMU_CHIPCTL_DATA, ~0x4);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		ssb_cc = &dev->dev->sdev->bus->chipco;

		chipco_write32(ssb_cc, SSB_CHIPCO_CHIPCTL_ADDR, 0);
		chipco_mask32(ssb_cc, SSB_CHIPCO_CHIPCTL_DATA, ~0x4);
		chipco_set32(ssb_cc, SSB_CHIPCO_CHIPCTL_DATA, 0x4);
		chipco_mask32(ssb_cc, SSB_CHIPCO_CHIPCTL_DATA, ~0x4);
		break;
#endif
	}
}

#ifdef CONFIG_B43_BCMA
static void b43_bcma_phy_reset(struct b43_wldev *dev)
{
	u32 flags;

	/* Put PHY into reset */
	flags = bcma_aread32(dev->dev->bdev, BCMA_IOCTL);
	flags |= B43_BCMA_IOCTL_PHY_RESET;
	flags |= dev->phy.bw_clkbits ?: B43_BCMA_IOCTL_PHY_BW_20MHZ;

	/*
	 * The AC's stock driver, 6.30 and 7.14 alike, forces the clock while
	 * the PHY is in reset: 0x14f. b43_phy_take_out_of_reset() continues.
	 */
	if (b43_is_ac_core(dev))
		flags |= BCMA_IOCTL_FGC;
	bcma_awrite32(dev->dev->bdev, BCMA_IOCTL, flags);
	udelay(2);

	b43_phy_take_out_of_reset(dev);
}

static void b43_bcma_wireless_core_reset(struct b43_wldev *dev, bool gmode)
{
	u32 req = B43_BCMA_CLKCTLST_80211_PLL_REQ |
		  B43_BCMA_CLKCTLST_PHY_PLL_REQ;
	u32 status = B43_BCMA_CLKCTLST_80211_PLL_ST |
		     B43_BCMA_CLKCTLST_PHY_PLL_ST;
	u32 flags;

	flags = B43_BCMA_IOCTL_PHY_CLKEN;
	if (gmode)
		flags |= B43_BCMA_IOCTL_GMODE;
	b43_device_enable(dev, flags);

	if (b43_is_ac_core(dev)) {
		u16 tmp;

		tmp = bcma_aread32(dev->dev->bdev, BCMA_IOCTL);
		tmp &= ~B43_BCMA_IOCTL_DAC;
		tmp |= 0x100;
		bcma_awrite32(dev->dev->bdev, BCMA_IOCTL, tmp);

		tmp = bcma_aread32(dev->dev->bdev, BCMA_IOCTL);
		tmp &= ~B43_BCMA_IOCTL_PHY_CLKEN;
		bcma_awrite32(dev->dev->bdev, BCMA_IOCTL, tmp);

		tmp = bcma_aread32(dev->dev->bdev, BCMA_IOCTL);
		tmp |= B43_BCMA_IOCTL_PHY_CLKEN;
		bcma_awrite32(dev->dev->bdev, BCMA_IOCTL, tmp);
	}

	bcma_core_set_clockmode(dev->dev->bdev, BCMA_CLKMODE_FAST);
	b43_bcma_phy_reset(dev);
	/*
	 * Neither stock driver requests the two PLLs on the AC, 6.30 and 7.14
	 * alike: both status bits are already up when it reads clk_ctl_st.
	 * Requesting them only when they are not keeps that sequence and does
	 * not leave the core without them on a board where they are down.
	 */
	if (!b43_is_ac_core(dev) ||
	    (bcma_read32(dev->dev->bdev, BCMA_CLKCTLST) & status) != status)
		bcma_core_pll_ctl(dev->dev->bdev, req, status, true);
}
#endif

#ifdef CONFIG_B43_SSB
static void b43_ssb_wireless_core_reset(struct b43_wldev *dev, bool gmode)
{
	u32 flags = 0;

	if (gmode)
		flags |= B43_TMSLOW_GMODE;
	flags |= B43_TMSLOW_PHYCLKEN;
	flags |= B43_TMSLOW_PHYRESET;
	if (dev->phy.type == B43_PHYTYPE_N)
		flags |= B43_TMSLOW_PHY_BANDWIDTH_20MHZ; /* Make 20 MHz def */
	b43_device_enable(dev, flags);
	msleep(2);		/* Wait for the PLL to turn on. */

	b43_phy_take_out_of_reset(dev);
}
#endif

void b43_wireless_core_reset(struct b43_wldev *dev, bool gmode)
{
	u32 macctl;

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		b43_bcma_wireless_core_reset(dev, gmode);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		b43_ssb_wireless_core_reset(dev, gmode);
		break;
#endif
	}

	/* Turn Analog ON, but only if we already know the PHY-type.
	 * This protects against very early setup where we don't know the
	 * PHY-type, yet. wireless_core_reset will be called once again later,
	 * when we know the PHY-type. */
	if (dev->phy.ops)
		dev->phy.ops->switch_analog(dev, 1);

	macctl = b43_read32(dev, B43_MMIO_MACCTL);
	macctl &= ~B43_MACCTL_GMODE;
	if (gmode)
		macctl |= B43_MACCTL_GMODE;
	macctl |= B43_MACCTL_IHR_ENABLED;
	/*
	 * The AC's stock driver keeps the ucode awake whenever the MAC is
	 * disabled, from the first reset on: 0x04000400 here. b43 forces it
	 * around a suspend; before the first b43_mac_enable() nothing did.
	 */
	if (b43_is_ac_core(dev))
		macctl |= B43_MACCTL_AWAKE;
	b43_write32(dev, B43_MMIO_MACCTL, macctl);
}

#define B43_TXST_AC_INTERMEDIATE	0x00000004
#define B43_TXST_AC_PM_INDICATED	0x00000008
#define B43_TXST_AC_SUPP		0x000000f0
#define B43_TXST_AC_SUPP_SHIFT		4
#define B43_TXST_AC_ACKED		0x00008000

/*
 * One TX status of the AC microcode: two packages of four words, read as two
 * passes over XMITSTAT_0..3. Bit 0 of the first word says an entry is there,
 * and the second package has it set too, so the entry is read whole before
 * the next one is looked at. Returns false when the queue is empty.
 *
 * In the first package word 0 holds the frame ID in its high half and the
 * status bits in its low half, words 2 and 3 the transmit attempts at rates
 * 0-1 and 2-3, in bits 7:0 and 23:16. b43's descriptor asks for its first
 * rate only, so the attempts are those of rate 0: on such a frame bits 23:16
 * read 1 once it is acknowledged, which is not an attempt. The second
 * package is not decoded.
 */
static bool b43_txstatus_read_ac(struct b43_wldev *dev,
				 struct b43_txstatus *stat)
{
	static const u16 reg[] = {
		B43_MMIO_XMITSTAT_0, B43_MMIO_XMITSTAT_1,
		B43_MMIO_XMITSTAT_2, B43_MMIO_XMITSTAT_3,
	};
	u32 w[ARRAY_SIZE(reg)];
	int i;

	w[0] = b43_read32(dev, reg[0]);
	if (!(w[0] & 0x00000001))
		return false;
	for (i = 1; i < ARRAY_SIZE(reg); i++)
		w[i] = b43_read32(dev, reg[i]);

	memset(stat, 0, sizeof(*stat));
	for (i = 0; i < ARRAY_SIZE(reg); i++) {
		stat->raw[i] = w[i];
		stat->raw[i + 4] = b43_read32(dev, reg[i]);
	}
	stat->cookie = w[0] >> 16;
	stat->acked = !!(w[0] & B43_TXST_AC_ACKED);
	stat->intermediate = !!(w[0] & B43_TXST_AC_INTERMEDIATE);
	stat->pm_indicated = !!(w[0] & B43_TXST_AC_PM_INDICATED);
	stat->supp_reason = (w[0] & B43_TXST_AC_SUPP) >> B43_TXST_AC_SUPP_SHIFT;
	stat->frame_count = w[2] & 0xff;
	return true;
}

static void handle_irq_transmit_status(struct b43_wldev *dev)
{
	u32 v0, v1;
	u16 tmp;
	struct b43_txstatus stat;

	if (dev->fw.hdr_format == B43_FW_HDR_AC) {
		while (b43_txstatus_read_ac(dev, &stat))
			b43_handle_txstatus(dev, &stat);
		return;
	}

	while (1) {
		v0 = b43_read32(dev, B43_MMIO_XMITSTAT_0);
		if (!(v0 & 0x00000001))
			break;
		v1 = b43_read32(dev, B43_MMIO_XMITSTAT_1);

		stat.cookie = (v0 >> 16);
		stat.seq = (v1 & 0x0000FFFF);
		stat.phy_stat = ((v1 & 0x00FF0000) >> 16);
		tmp = (v0 & 0x0000FFFF);
		stat.frame_count = ((tmp & 0xF000) >> 12);
		stat.rts_count = ((tmp & 0x0F00) >> 8);
		stat.supp_reason = ((tmp & 0x001C) >> 2);
		stat.pm_indicated = !!(tmp & 0x0080);
		stat.intermediate = !!(tmp & 0x0040);
		stat.for_ampdu = !!(tmp & 0x0020);
		stat.acked = !!(tmp & 0x0002);

		b43_handle_txstatus(dev, &stat);
	}
}

static void drain_txstatus_queue(struct b43_wldev *dev)
{
	struct b43_txstatus stat;
	u32 dummy;

	if (dev->dev->core_rev < 5)
		return;
	if (dev->fw.hdr_format == B43_FW_HDR_AC) {
		while (b43_txstatus_read_ac(dev, &stat))
			;
		return;
	}
	/* Read all entries from the microcode TXstatus FIFO
	 * and throw them away.
	 */
	while (1) {
		dummy = b43_read32(dev, B43_MMIO_XMITSTAT_0);
		if (!(dummy & 0x00000001))
			break;
		dummy = b43_read32(dev, B43_MMIO_XMITSTAT_1);
	}
}

static u32 b43_jssi_read(struct b43_wldev *dev)
{
	u32 val = 0;

	val = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_JSSI1);
	val <<= 16;
	val |= b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_JSSI0);

	return val;
}

static void b43_jssi_write(struct b43_wldev *dev, u32 jssi)
{
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_JSSI0,
			(jssi & 0x0000FFFF));
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_JSSI1,
			(jssi & 0xFFFF0000) >> 16);
}

static void b43_generate_noise_sample(struct b43_wldev *dev)
{
	b43_jssi_write(dev, 0x7F7F7F7F);
	b43_write32(dev, B43_MMIO_MACCMD,
		    b43_read32(dev, B43_MMIO_MACCMD) | B43_MACCMD_BGNOISE);
}

static void b43_calculate_link_quality(struct b43_wldev *dev)
{
	/* Top half of Link Quality calculation. */

	if (dev->phy.type != B43_PHYTYPE_G)
		return;
	if (dev->noisecalc.calculation_running)
		return;
	dev->noisecalc.calculation_running = true;
	dev->noisecalc.nr_samples = 0;

	b43_generate_noise_sample(dev);
}

static void handle_irq_noise(struct b43_wldev *dev)
{
	struct b43_phy_g *phy = dev->phy.g;
	u16 tmp;
	u8 noise[4];
	u8 i, j;
	s32 average;

	if (dev->phy.type == B43_PHYTYPE_AC) {
		if (dev->phy.ops->noise_sample_done)
			dev->phy.ops->noise_sample_done(dev);
		return;
	}

	/* Bottom half of Link Quality calculation. */

	if (dev->phy.type != B43_PHYTYPE_G)
		return;

	/* Possible race condition: It might be possible that the user
	 * changed to a different channel in the meantime since we
	 * started the calculation. We ignore that fact, since it's
	 * not really that much of a problem. The background noise is
	 * an estimation only anyway. Slightly wrong results will get damped
	 * by the averaging of the 8 sample rounds. Additionally the
	 * value is shortlived. So it will be replaced by the next noise
	 * calculation round soon. */

	B43_WARN_ON(!dev->noisecalc.calculation_running);
	*((__le32 *)noise) = cpu_to_le32(b43_jssi_read(dev));
	if (noise[0] == 0x7F || noise[1] == 0x7F ||
	    noise[2] == 0x7F || noise[3] == 0x7F)
		goto generate_new;

	/* Get the noise samples. */
	B43_WARN_ON(dev->noisecalc.nr_samples >= 8);
	i = dev->noisecalc.nr_samples;
	noise[0] = clamp_val(noise[0], 0, ARRAY_SIZE(phy->nrssi_lt) - 1);
	noise[1] = clamp_val(noise[1], 0, ARRAY_SIZE(phy->nrssi_lt) - 1);
	noise[2] = clamp_val(noise[2], 0, ARRAY_SIZE(phy->nrssi_lt) - 1);
	noise[3] = clamp_val(noise[3], 0, ARRAY_SIZE(phy->nrssi_lt) - 1);
	dev->noisecalc.samples[i][0] = phy->nrssi_lt[noise[0]];
	dev->noisecalc.samples[i][1] = phy->nrssi_lt[noise[1]];
	dev->noisecalc.samples[i][2] = phy->nrssi_lt[noise[2]];
	dev->noisecalc.samples[i][3] = phy->nrssi_lt[noise[3]];
	dev->noisecalc.nr_samples++;
	if (dev->noisecalc.nr_samples == 8) {
		/* Calculate the Link Quality by the noise samples. */
		average = 0;
		for (i = 0; i < 8; i++) {
			for (j = 0; j < 4; j++)
				average += dev->noisecalc.samples[i][j];
		}
		average /= (8 * 4);
		average *= 125;
		average += 64;
		average /= 128;
		tmp = b43_shm_read16(dev, B43_SHM_SHARED, 0x40C);
		tmp = (tmp / 128) & 0x1F;
		if (tmp >= 8)
			average += 2;
		else
			average -= 25;
		if (tmp == 8)
			average -= 72;
		else
			average -= 48;

		dev->stats.link_noise = average;
		dev->noisecalc.calculation_running = false;
		return;
	}
generate_new:
	b43_generate_noise_sample(dev);
}

static void handle_irq_tbtt_indication(struct b43_wldev *dev)
{
	if (b43_is_mode(dev->wl, NL80211_IFTYPE_AP)) {
		///TODO: PS TBTT
	} else {
		if (1 /*FIXME: the last PSpoll frame was sent successfully */ )
			b43_power_saving_ctl_bits(dev, 0);
	}
	if (b43_is_mode(dev->wl, NL80211_IFTYPE_ADHOC))
		dev->dfq_valid = true;
}

static void handle_irq_atim_end(struct b43_wldev *dev)
{
	if (dev->dfq_valid) {
		b43_write32(dev, B43_MMIO_MACCMD,
			    b43_read32(dev, B43_MMIO_MACCMD)
			    | B43_MACCMD_DFQ_VALID);
		dev->dfq_valid = false;
	}
}

/* Entries read from the power management queue before the handler gives up. */
#define B43_PMQ_DRAIN_MAX	256

static void handle_irq_pmq(struct b43_wldev *dev)
{
	unsigned int i;
	u32 tmp;

	//TODO: AP mode.

	for (i = 0; i < B43_PMQ_DRAIN_MAX; i++) {
		tmp = b43_read32(dev, B43_MMIO_PS_STATUS);
		if (!(tmp & 0x00000008))
			break;
	}
	if (i == B43_PMQ_DRAIN_MAX)
		b43warn(dev->wl, "Power management queue not drained: 0x%08x\n",
			tmp);
	/* 16bit write is odd, but correct. */
	b43_write16(dev, B43_MMIO_PS_STATUS, 0x0002);
}

static const struct b43_tpl_layout b43_tpl_layout_legacy = {
	.bcn_base	= { B43_SHM_SH_BT_BASE0, B43_SHM_SH_BT_BASE1 },
	.bcn_size	= 0x0200,
	.hdr_len	= sizeof(struct b43_plcp_hdr6),
	.plcp_off	= 0,
};

static const struct b43_tpl_layout b43_tpl_layout_ac_784 = {
	.bcn_base	= { B43_SHM_SH_BT_BASE0_AC784,
			    B43_SHM_SH_BT_BASE1_AC784 },
	.bcn_size	= 0x0200,
	.hdr_len	= 12,
	.plcp_off	= 3,
};

static const struct b43_tpl_layout b43_tpl_layout_ac_832 = {
	.bcn_base	= { B43_SHM_SH_BT_BASE0_AC832,
			    B43_SHM_SH_BT_BASE1_AC832 },
	.bcn_size	= 0x0280,
	.hdr_len	= 12,
	.plcp_off	= 3,
};

static const struct b43_tpl_layout *b43_tpl_layout_find(struct b43_wldev *dev)
{
	if (dev->fw.hdr_format != B43_FW_HDR_AC)
		return &b43_tpl_layout_legacy;

	switch (dev->fw.rev) {
	case 784:
		return &b43_tpl_layout_ac_784;
	case 832:
	case 928:
		return &b43_tpl_layout_ac_832;
	}
	return NULL;
}

/* Byte @i of a template: the header the layout asks for, then the frame. */
static u8 b43_tpl_byte(const struct b43_tpl_layout *tpl,
		       const struct b43_plcp_hdr4 *plcp,
		       const u8 *data, u16 size, unsigned int i)
{
	if (i >= tpl->hdr_len)
		return i - tpl->hdr_len < size ? data[i - tpl->hdr_len] : 0;
	if (i >= tpl->plcp_off && i - tpl->plcp_off < sizeof(plcp->raw))
		return plcp->raw[i - tpl->plcp_off];
	return 0;
}

/* @shadow, if given, receives the words as written, template RAM order. */
static void b43_write_template_common(struct b43_wldev *dev,
				      const u8 *data, u16 size,
				      u16 ram_offset,
				      u16 shm_size_offset, u8 rate,
				      struct b43_bcn_shadow *shadow)
{
	const struct b43_tpl_layout *tpl = dev->fw.tpl;
	unsigned int i, total = tpl->hdr_len + size;
	struct b43_plcp_hdr4 plcp;
	struct b43_ram_seq seq = { .next = U16_MAX }, *s = NULL;

	if (dev->phy.type == B43_PHYTYPE_AC) {
		seq.swap = b43_read32(dev, B43_MMIO_MACCTL) & B43_MACCTL_BE;
		s = &seq;
	}

	plcp.data = 0;
	b43_generate_plcp_hdr(&plcp, size + FCS_LEN, rate);

	for (i = 0; i < total; i += sizeof(u32)) {
		u32 tmp = 0;
		unsigned int b;

		for (b = 0; b < sizeof(u32); b++)
			tmp |= (u32)b43_tpl_byte(tpl, &plcp, data, size,
						 i + b) << (8 * b);
		b43_tpl_write(dev, s, ram_offset + i, tmp);
		if (shadow && i / 4 < ARRAY_SIZE(shadow->words))
			shadow->words[i / 4] = tmp;
	}
	b43_shm_write16(dev, B43_SHM_SHARED, shm_size_offset, total);

	if (shadow) {
		shadow->base = ram_offset;
		shadow->total = total;
		shadow->valid = true;
	}
}

/* Check if the use of the antenna that ieee80211 told us to
 * use is possible. This will fall back to DEFAULT.
 * "antenna_nr" is the antenna identifier we got from ieee80211. */
u8 b43_ieee80211_antenna_sanitize(struct b43_wldev *dev,
				  u8 antenna_nr)
{
	u8 antenna_mask;

	if (antenna_nr == 0) {
		/* Zero means "use default antenna". That's always OK. */
		return 0;
	}

	/* Get the mask of available antennas. */
	if (dev->phy.gmode)
		antenna_mask = dev->dev->bus_sprom->ant_available_bg;
	else
		antenna_mask = dev->dev->bus_sprom->ant_available_a;

	if (!(antenna_mask & (1 << (antenna_nr - 1)))) {
		/* This antenna is not available. Fall back to default. */
		return 0;
	}

	return antenna_nr;
}

/* Convert a b43 antenna number value to the PHY TX control value. */
static u16 b43_antenna_to_phyctl(int antenna)
{
	switch (antenna) {
	case B43_ANTENNA0:
		return B43_TXH_PHY_ANT0;
	case B43_ANTENNA1:
		return B43_TXH_PHY_ANT1;
	case B43_ANTENNA2:
		return B43_TXH_PHY_ANT2;
	case B43_ANTENNA3:
		return B43_TXH_PHY_ANT3;
	case B43_ANTENNA_AUTO0:
	case B43_ANTENNA_AUTO1:
		return B43_TXH_PHY_ANT01AUTO;
	}
	B43_WARN_ON(1);
	return 0;
}

/*
 * The PHY TX control words of the beacon, the ACK/CTS and the probe
 * response in shared memory have the pre-AC layout; the AC microcode keeps
 * the beacon's elsewhere, see b43_write_beacon_phytxctl_ac().
 */
static bool b43_phytxctl_in_shm(struct b43_wldev *dev)
{
	return dev->fw.hdr_format != B43_FW_HDR_AC;
}

static void b43_write_beacon_phytxctl(struct b43_wldev *dev, u16 rate)
{
	u16 ctl;

	ctl = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL);
	/* We can't send beacons with short preamble. Would get PHY errors. */
	ctl &= ~B43_TXH_PHY_SHORTPRMBL;
	ctl &= ~B43_TXH_PHY_ANT;
	ctl &= ~B43_TXH_PHY_ENC;
	ctl |= b43_antenna_to_phyctl(B43_ANTENNA_DEFAULT);
	if (b43_is_cck_rate(rate))
		ctl |= B43_TXH_PHY_ENC_CCK;
	else
		ctl |= B43_TXH_PHY_ENC_OFDM;
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL, ctl);
}

/*
 * The PHY keeps the chain mask in this word (b43_phy_ac_bss_cc_update()) and
 * the beacon's power offset in the next. The encoding is the core's: the
 * stock drivers set it before each beacon template, 0x00c4 to 0x00c5 on the
 * DSL-3580L, 0x0144 to 0x0145 on the agcombo; the initvals' 0x0044 says CCK,
 * which 5 GHz cannot carry.
 */
static void b43_write_beacon_phytxctl_ac(struct b43_wldev *dev, u16 rate)
{
	u16 ctl;

	ctl = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL_AC);
	ctl &= ~B43_TXH_PHY_ENC;
	ctl |= b43_is_cck_rate(rate) ? B43_TXH_PHY_ENC_CCK : B43_TXH_PHY_ENC_OFDM;
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL_AC, ctl);
}

static void b43_write_beacon_template(struct b43_wldev *dev,
				      unsigned int idx,
				      u16 ram_offset,
				      u16 shm_size_offset)
{
	const struct b43_tpl_layout *tpl = dev->fw.tpl;
	unsigned int i, len, variable_len;
	u16 tim_position = 0, dtim_period = 0;
	const struct ieee80211_mgmt *bcn;
	const u8 *ie;
	bool tim_found = false;
	unsigned int rate;
	struct ieee80211_tx_info *info;
	unsigned long flags;
	struct sk_buff *beacon_skb;

	spin_lock_irqsave(&dev->wl->beacon_lock, flags);
	info = IEEE80211_SKB_CB(dev->wl->current_beacon);
	rate = ieee80211_get_tx_rate(dev->wl->hw, info)->hw_value;
	/* Clone the beacon, so it cannot go away, while we write it to hw. */
	beacon_skb = skb_clone(dev->wl->current_beacon, GFP_ATOMIC);
	spin_unlock_irqrestore(&dev->wl->beacon_lock, flags);

	if (!beacon_skb) {
		b43dbg(dev->wl, "Could not upload beacon. "
		       "Failed to clone beacon skb.");
		return;
	}

	bcn = (const struct ieee80211_mgmt *)(beacon_skb->data);
	len = min_t(size_t, beacon_skb->len,
		    tpl->bcn_size - tpl->hdr_len);

	b43_write_template_common(dev, (const u8 *)bcn,
				  len, ram_offset, shm_size_offset, rate,
				  &dev->wl->diag.tpl[idx]);

	if (b43_phytxctl_in_shm(dev))
		b43_write_beacon_phytxctl(dev, rate);
	else
		b43_write_beacon_phytxctl_ac(dev, rate);

	/* Find the position of the TIM and the DTIM_period value
	 * and write them to SHM. */
	ie = bcn->u.beacon.variable;
	variable_len = len - offsetof(struct ieee80211_mgmt, u.beacon.variable);
	for (i = 0; i < variable_len - 2; ) {
		uint8_t ie_id, ie_len;

		ie_id = ie[i];
		ie_len = ie[i + 1];
		if (ie_id == 5) {
			/* This is the TIM Information Element */

			/* Check whether the ie_len is in the beacon data range. */
			if (variable_len < ie_len + 2 + i)
				break;
			/* A valid TIM is at least 4 bytes long. */
			if (ie_len < 4)
				break;
			tim_found = true;

			tim_position = tpl->hdr_len;
			tim_position += offsetof(struct ieee80211_mgmt, u.beacon.variable);
			tim_position += i;

			dtim_period = ie[i + 3];

			b43_shm_write16(dev, B43_SHM_SHARED,
					B43_SHM_SH_TIMBPOS, tim_position);
			b43_shm_write16(dev, B43_SHM_SHARED,
					B43_SHM_SH_DTIMPER, dtim_period);
			break;
		}
		i += ie_len + 2;
	}
	if (!tim_found) {
		/*
		 * If ucode wants to modify TIM do it behind the beacon, this
		 * will happen, for example, when doing mesh networking.
		 */
		tim_position = len + tpl->hdr_len;
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_TIMBPOS, tim_position);
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_DTIMPER, 0);
	}
	b43dbg(dev->wl, "Updated beacon template at 0x%x\n", ram_offset);
	b43_bcn_diag_uploaded(dev, idx, (const u8 *)bcn, len, rate,
			      tim_position, dtim_period);

	dev_kfree_skb_any(beacon_skb);
}

static void b43_upload_beacon0(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;

	if (wl->beacon0_uploaded)
		return;
	b43_write_beacon_template(dev, 0, dev->fw.tpl->bcn_base[0],
				  B43_SHM_SH_BTL0);
	wl->beacon0_uploaded = true;
}

static void b43_upload_beacon1(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;

	if (wl->beacon1_uploaded)
		return;
	b43_write_beacon_template(dev, 1, dev->fw.tpl->bcn_base[1],
				  B43_SHM_SH_BTL1);
	wl->beacon1_uploaded = true;
}

static void handle_irq_beacon(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;
	u32 cmd, beacon0_valid, beacon1_valid;

	if (!b43_is_mode(wl, NL80211_IFTYPE_AP) &&
	    !b43_is_mode(wl, NL80211_IFTYPE_MESH_POINT) &&
	    !b43_is_mode(wl, NL80211_IFTYPE_ADHOC))
		return;

	/* This is the bottom half of the asynchronous beacon update. */

	/* Ignore interrupt in the future. */
	dev->irq_mask &= ~B43_IRQ_BEACON;

	cmd = b43_read32(dev, B43_MMIO_MACCMD);
	b43_bcn_diag_irq(dev, cmd);
	beacon0_valid = (cmd & B43_MACCMD_BEACON0_VALID);
	beacon1_valid = (cmd & B43_MACCMD_BEACON1_VALID);

	/* Schedule interrupt manually, if busy. */
	if (beacon0_valid && beacon1_valid) {
		b43_write32(dev, B43_MMIO_GEN_IRQ_REASON, B43_IRQ_BEACON);
		dev->irq_mask |= B43_IRQ_BEACON;
		return;
	}

	if (unlikely(wl->beacon_templates_virgin)) {
		/* We never uploaded a beacon before.
		 * Upload both templates now, but only mark one valid. */
		wl->beacon_templates_virgin = false;
		b43_upload_beacon0(dev);
		b43_upload_beacon1(dev);
		cmd = b43_read32(dev, B43_MMIO_MACCMD);
		cmd |= B43_MACCMD_BEACON0_VALID;
		b43_write32(dev, B43_MMIO_MACCMD, cmd);
	} else {
		if (!beacon0_valid) {
			b43_upload_beacon0(dev);
			cmd = b43_read32(dev, B43_MMIO_MACCMD);
			cmd |= B43_MACCMD_BEACON0_VALID;
			b43_write32(dev, B43_MMIO_MACCMD, cmd);
		} else if (!beacon1_valid) {
			b43_upload_beacon1(dev);
			cmd = b43_read32(dev, B43_MMIO_MACCMD);
			cmd |= B43_MACCMD_BEACON1_VALID;
			b43_write32(dev, B43_MMIO_MACCMD, cmd);
		}
	}
}

static void b43_do_beacon_update_trigger_work(struct b43_wldev *dev)
{
	u32 old_irq_mask = dev->irq_mask;

	/* update beacon right away or defer to irq */
	handle_irq_beacon(dev);
	if (old_irq_mask != dev->irq_mask) {
		/* The handler updated the IRQ mask. */
		B43_WARN_ON(!dev->irq_mask);
		if (b43_read32(dev, B43_MMIO_GEN_IRQ_MASK)) {
			b43_write32(dev, B43_MMIO_GEN_IRQ_MASK, dev->irq_mask);
		} else {
			/* Device interrupts are currently disabled. That means
			 * we just ran the hardirq handler and scheduled the
			 * IRQ thread. The thread will write the IRQ mask when
			 * it finished, so there's nothing to do here. Writing
			 * the mask _here_ would incorrectly re-enable IRQs. */
		}
	}
}

static void b43_beacon_update_trigger_work(struct work_struct *work)
{
	struct b43_wl *wl = container_of(work, struct b43_wl,
					 beacon_update_trigger);
	struct b43_wldev *dev;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (likely(dev && (b43_status(dev) >= B43_STAT_INITIALIZED))) {
		if (b43_bus_host_is_sdio(dev->dev)) {
			/* wl->mutex is enough. */
			b43_do_beacon_update_trigger_work(dev);
		} else {
			spin_lock_irq(&wl->hardirq_lock);
			b43_do_beacon_update_trigger_work(dev);
			spin_unlock_irq(&wl->hardirq_lock);
		}
	}
	mutex_unlock(&wl->mutex);
}

/* Asynchronously update the packet templates in template RAM. */
static void b43_update_templates(struct b43_wl *wl)
{
	struct sk_buff *beacon, *old_beacon;
	unsigned long flags;

	/* This is the top half of the asynchronous beacon update.
	 * The bottom half is the beacon IRQ.
	 * Beacon update must be asynchronous to avoid sending an
	 * invalid beacon. This can happen for example, if the firmware
	 * transmits a beacon while we are updating it. */

	/* We could modify the existing beacon and set the aid bit in
	 * the TIM field, but that would probably require resizing and
	 * moving of data within the beacon template.
	 * Simply request a new beacon and let mac80211 do the hard work. */
	beacon = ieee80211_beacon_get(wl->hw, wl->vif, 0);
	if (unlikely(!beacon))
		return;

	spin_lock_irqsave(&wl->beacon_lock, flags);
	old_beacon = wl->current_beacon;
	wl->current_beacon = beacon;
	wl->beacon0_uploaded = false;
	wl->beacon1_uploaded = false;
	spin_unlock_irqrestore(&wl->beacon_lock, flags);

	ieee80211_queue_work(wl->hw, &wl->beacon_update_trigger);

	if (old_beacon)
		dev_kfree_skb_any(old_beacon);
}

static void b43_set_beacon_int(struct b43_wldev *dev, u16 beacon_int)
{
	b43_time_lock(dev);
	if (dev->dev->core_rev >= 3) {
		b43_write32(dev, B43_MMIO_TSF_CFP_REP, (beacon_int << 16));
		b43_write32(dev, B43_MMIO_TSF_CFP_START, (beacon_int << 10));
	} else {
		b43_write16(dev, 0x606, (beacon_int >> 6));
		b43_write16(dev, 0x610, beacon_int);
	}
	b43_time_unlock(dev);
	b43dbg(dev->wl, "Set beacon interval to %u\n", beacon_int);
}

static void b43_handle_firmware_panic(struct b43_wldev *dev)
{
	u16 reason;

	/* Read the register that contains the reason code for the panic. */
	reason = b43_shm_read16(dev, B43_SHM_SCRATCH, B43_FWPANIC_REASON_REG);
	b43err_restart(dev->wl, "Whoopsy, firmware panic! Reason: %u\n",
		       reason);

	switch (reason) {
	default:
		b43err_restart(dev->wl, "The panic reason is unknown.\n");
		fallthrough;
	case B43_FWPANIC_DIE:
		/* Do not restart the controller or firmware.
		 * The device is nonfunctional from now on.
		 * Restarting would result in this panic to trigger again,
		 * so we avoid that recursion. */
		break;
	case B43_FWPANIC_RESTART:
		b43_controller_restart(dev, "Microcode panic");
		break;
	}
}

static void handle_irq_ucode_debug(struct b43_wldev *dev)
{
	unsigned int i, cnt;
	u16 reason, marker_id, marker_line;
	__le16 *buf;

	/* The proprietary firmware doesn't have this IRQ. */
	if (!dev->fw.opensource)
		return;

	/* Read the register that contains the reason code for this IRQ. */
	reason = b43_shm_read16(dev, B43_SHM_SCRATCH, B43_DEBUGIRQ_REASON_REG);

	switch (reason) {
	case B43_DEBUGIRQ_PANIC:
		b43_handle_firmware_panic(dev);
		break;
	case B43_DEBUGIRQ_DUMP_SHM:
		if (!B43_DEBUG)
			break; /* Only with driver debugging enabled. */
		buf = kmalloc(4096, GFP_ATOMIC);
		if (!buf) {
			b43dbg(dev->wl, "SHM-dump: Failed to allocate memory\n");
			goto out;
		}
		for (i = 0; i < 4096; i += 2) {
			u16 tmp = b43_shm_read16(dev, B43_SHM_SHARED, i);
			buf[i / 2] = cpu_to_le16(tmp);
		}
		b43info(dev->wl, "Shared memory dump:\n");
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET,
			       16, 2, buf, 4096, 1);
		kfree(buf);
		break;
	case B43_DEBUGIRQ_DUMP_REGS:
		if (!B43_DEBUG)
			break; /* Only with driver debugging enabled. */
		b43info(dev->wl, "Microcode register dump:\n");
		for (i = 0, cnt = 0; i < 64; i++) {
			u16 tmp = b43_shm_read16(dev, B43_SHM_SCRATCH, i);
			if (cnt == 0)
				printk(KERN_INFO);
			printk("r%02u: 0x%04X  ", i, tmp);
			cnt++;
			if (cnt == 6) {
				printk("\n");
				cnt = 0;
			}
		}
		printk("\n");
		break;
	case B43_DEBUGIRQ_MARKER:
		if (!B43_DEBUG)
			break; /* Only with driver debugging enabled. */
		marker_id = b43_shm_read16(dev, B43_SHM_SCRATCH,
					   B43_MARKER_ID_REG);
		marker_line = b43_shm_read16(dev, B43_SHM_SCRATCH,
					     B43_MARKER_LINE_REG);
		b43info(dev->wl, "The firmware just executed the MARKER(%u) "
			"at line number %u\n",
			marker_id, marker_line);
		break;
	default:
		b43dbg(dev->wl, "Debug-IRQ triggered for unknown reason: %u\n",
		       reason);
	}
out:
	/* Acknowledge the debug-IRQ, so the firmware can continue. */
	b43_shm_write16(dev, B43_SHM_SCRATCH,
			B43_DEBUGIRQ_REASON_REG, B43_DEBUGIRQ_ACK);
}

static void b43_do_interrupt_thread(struct b43_wldev *dev)
{
	u32 reason;
	u32 dma_reason[ARRAY_SIZE(dev->dma_reason)];
	u32 merged_dma_reason = 0;
	int i;

	if (unlikely(b43_status(dev) != B43_STAT_STARTED))
		return;

	reason = dev->irq_reason;
	for (i = 0; i < ARRAY_SIZE(dma_reason); i++) {
		dma_reason[i] = dev->dma_reason[i];
		merged_dma_reason |= dma_reason[i];
	}

	if (unlikely(reason & B43_IRQ_MAC_TXERR))
		b43err(dev->wl, "MAC transmission error\n");

	/*
	 * The AC runs with PHY_TXERR unmasked, as the stock driver does, and
	 * like brcmsmac only acknowledges it; the other PHYs mask it outside
	 * debug, where this handler counts towards a restart.
	 */
	if (unlikely(reason & B43_IRQ_PHY_TXERR) &&
	    dev->phy.type != B43_PHYTYPE_AC) {
		b43err(dev->wl, "PHY transmission error\n");
		rmb();
		if (unlikely(atomic_dec_and_test(&dev->phy.txerr_cnt))) {
			atomic_set(&dev->phy.txerr_cnt,
				   B43_PHY_TX_BADNESS_LIMIT);
			b43err_restart(dev->wl, "Too many PHY TX errors, "
					"restarting the controller\n");
			b43_controller_restart(dev, "PHY TX errors");
		}
	}

	if (unlikely(merged_dma_reason & (B43_DMAIRQ_FATALMASK))) {
		b43err_restart(dev->wl,
			"Fatal DMA error: 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X\n",
			dma_reason[0], dma_reason[1],
			dma_reason[2], dma_reason[3],
			dma_reason[4], dma_reason[5]);
		b43err_restart(dev->wl, "This device does not support DMA "
			       "on your system. It will now be switched to PIO.\n");
		/* Fall back to PIO transfers if we get fatal DMA errors! */
		dev->use_pio = true;
		b43_controller_restart(dev, "DMA error");
		return;
	}

	if (unlikely(reason & B43_IRQ_UCODE_DEBUG))
		handle_irq_ucode_debug(dev);
	if (dev->phy.type == B43_PHYTYPE_AC) {
		/* As brcmsmac: the PSM watchdog resets, the timer is stopped. */
		if (unlikely(reason & B43_IRQ_TIMER0)) {
			b43err_restart(dev->wl,
				       "PSM microcode watchdog fired\n");
			b43_controller_restart(dev, "PSM watchdog");
			return;
		}
		if (reason & B43_IRQ_TIMEOUT)
			b43_write32(dev, B43_MMIO_GPTIMER, 0);
	}
	if (reason & B43_IRQ_TBTT_INDI)
		handle_irq_tbtt_indication(dev);
	if (reason & B43_IRQ_ATIM_END)
		handle_irq_atim_end(dev);
	if (reason & B43_IRQ_BEACON)
		handle_irq_beacon(dev);
	if (reason & B43_IRQ_PMQ)
		handle_irq_pmq(dev);
	if (reason & B43_IRQ_TXFIFO_FLUSH_OK) {
		;/* TODO */
	}
	if (reason & B43_IRQ_NOISESAMPLE_OK)
		handle_irq_noise(dev);

	/*
	 * Check the DMA reason registers for received data. An underrun is a
	 * full ring, and reading it gives the buffers back.
	 */
	if (dma_reason[0] & B43_DMAIRQ_RDESC_UFLOW)
		b43warn(dev->wl, "RX descriptor underrun\n");
	if (dma_reason[0] & (B43_DMAIRQ_RX_DONE | B43_DMAIRQ_RDESC_UFLOW)) {
		if (b43_using_pio_transfers(dev))
			b43_pio_rx(dev->pio.rx_queue);
		else
			b43_dma_rx(dev->dma.rx_ring);
	}
	B43_WARN_ON(dma_reason[1] & B43_DMAIRQ_RX_DONE);
	B43_WARN_ON(dma_reason[2] & B43_DMAIRQ_RX_DONE);
	B43_WARN_ON(dma_reason[3] & B43_DMAIRQ_RX_DONE);
	B43_WARN_ON(dma_reason[4] & B43_DMAIRQ_RX_DONE);
	B43_WARN_ON(dma_reason[5] & B43_DMAIRQ_RX_DONE);

	if (reason & B43_IRQ_TX_OK)
		handle_irq_transmit_status(dev);

	/* Re-enable interrupts on the device by restoring the current interrupt mask. */
	b43_write32(dev, B43_MMIO_GEN_IRQ_MASK, dev->irq_mask);

#if B43_DEBUG
	if (b43_debug(dev, B43_DBG_VERBOSESTATS)) {
		dev->irq_count++;
		for (i = 0; i < ARRAY_SIZE(dev->irq_bit_count); i++) {
			if (reason & (1 << i))
				dev->irq_bit_count[i]++;
		}
	}
#endif
}

/* Interrupt thread handler. Handles device interrupts in thread context. */
static irqreturn_t b43_interrupt_thread_handler(int irq, void *dev_id)
{
	struct b43_wldev *dev = dev_id;

	mutex_lock(&dev->wl->mutex);
	b43_do_interrupt_thread(dev);
	mutex_unlock(&dev->wl->mutex);

	return IRQ_HANDLED;
}

static irqreturn_t b43_do_interrupt(struct b43_wldev *dev)
{
	u32 reason;

	/* This code runs under wl->hardirq_lock, but _only_ on non-SDIO busses.
	 * On SDIO, this runs under wl->mutex. */

	reason = b43_read32(dev, B43_MMIO_GEN_IRQ_REASON);
	if (reason == 0xffffffff)	/* shared IRQ */
		return IRQ_NONE;
	reason &= dev->irq_mask;
	if (!reason)
		return IRQ_NONE;

	dev->dma_reason[0] = b43_read32(dev, B43_MMIO_DMA0_REASON)
	    & 0x0001FC00;
	dev->dma_reason[1] = b43_read32(dev, B43_MMIO_DMA1_REASON)
	    & 0x0000DC00;
	dev->dma_reason[2] = b43_read32(dev, B43_MMIO_DMA2_REASON)
	    & 0x0000DC00;
	dev->dma_reason[3] = b43_read32(dev, B43_MMIO_DMA3_REASON)
	    & 0x0001DC00;
	dev->dma_reason[4] = b43_read32(dev, B43_MMIO_DMA4_REASON)
	    & 0x0000DC00;
/* Unused ring
	dev->dma_reason[5] = b43_read32(dev, B43_MMIO_DMA5_REASON)
	    & 0x0000DC00;
*/

	/* ACK the interrupt. */
	b43_write32(dev, B43_MMIO_GEN_IRQ_REASON, reason);
	b43_write32(dev, B43_MMIO_DMA0_REASON, dev->dma_reason[0]);
	b43_write32(dev, B43_MMIO_DMA1_REASON, dev->dma_reason[1]);
	b43_write32(dev, B43_MMIO_DMA2_REASON, dev->dma_reason[2]);
	b43_write32(dev, B43_MMIO_DMA3_REASON, dev->dma_reason[3]);
	b43_write32(dev, B43_MMIO_DMA4_REASON, dev->dma_reason[4]);
/* Unused ring
	b43_write32(dev, B43_MMIO_DMA5_REASON, dev->dma_reason[5]);
*/

	/* Disable IRQs on the device. The IRQ thread handler will re-enable them. */
	b43_write32(dev, B43_MMIO_GEN_IRQ_MASK, 0);
	/* Save the reason bitmasks for the IRQ thread handler. */
	dev->irq_reason = reason;

	return IRQ_WAKE_THREAD;
}

/* Interrupt handler top-half. This runs with interrupts disabled. */
static irqreturn_t b43_interrupt_handler(int irq, void *dev_id)
{
	struct b43_wldev *dev = dev_id;
	irqreturn_t ret;

	if (unlikely(b43_status(dev) < B43_STAT_STARTED))
		return IRQ_NONE;

	spin_lock(&dev->wl->hardirq_lock);
	ret = b43_do_interrupt(dev);
	spin_unlock(&dev->wl->hardirq_lock);

	return ret;
}

/* SDIO interrupt handler. This runs in process context. */
static void b43_sdio_interrupt_handler(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;
	irqreturn_t ret;

	mutex_lock(&wl->mutex);

	ret = b43_do_interrupt(dev);
	if (ret == IRQ_WAKE_THREAD)
		b43_do_interrupt_thread(dev);

	mutex_unlock(&wl->mutex);
}

void b43_do_release_fw(struct b43_firmware_file *fw)
{
	release_firmware(fw->data);
	fw->data = NULL;
	fw->filename = NULL;
}

static void b43_release_firmware(struct b43_wldev *dev)
{
	complete(&dev->fw_load_complete);
	b43_do_release_fw(&dev->fw.ucode);
	b43_do_release_fw(&dev->fw.pcm);
	b43_do_release_fw(&dev->fw.initvals);
	b43_do_release_fw(&dev->fw.initvals_band);
}

static void b43_print_fw_helptext(struct b43_wl *wl, bool error)
{
	const char text[] =
		"You must go to " \
		"https://wireless.wiki.kernel.org/en/users/Drivers/b43#devicefirmware " \
		"and download the correct firmware for this driver version. " \
		"Please carefully read all instructions on this website.\n";

	if (error)
		b43err(wl, text);
	else
		b43warn(wl, text);
}

static void b43_fw_cb(const struct firmware *firmware, void *context)
{
	struct b43_request_fw_context *ctx = context;

	ctx->blob = firmware;
	complete(&ctx->dev->fw_load_complete);
}

int b43_do_request_fw(struct b43_request_fw_context *ctx,
		      const char *name,
		      struct b43_firmware_file *fw, bool async)
{
	struct b43_fw_header *hdr;
	u32 size;
	int err;

	if (!name) {
		/* Don't fetch anything. Free possibly cached firmware. */
		/* FIXME: We should probably keep it anyway, to save some headache
		 * on suspend/resume with multiband devices. */
		b43_do_release_fw(fw);
		return 0;
	}
	if (fw->filename) {
		if ((fw->type == ctx->req_type) &&
		    (strcmp(fw->filename, name) == 0))
			return 0; /* Already have this fw. */
		/* Free the cached firmware first. */
		/* FIXME: We should probably do this later after we successfully
		 * got the new fw. This could reduce headache with multiband devices.
		 * We could also redesign this to cache the firmware for all possible
		 * bands all the time. */
		b43_do_release_fw(fw);
	}

	switch (ctx->req_type) {
	case B43_FWTYPE_PROPRIETARY:
		snprintf(ctx->fwname, sizeof(ctx->fwname),
			 "b43%s/%s.fw",
			 modparam_fwpostfix, name);
		break;
	case B43_FWTYPE_OPENSOURCE:
		snprintf(ctx->fwname, sizeof(ctx->fwname),
			 "b43-open%s/%s.fw",
			 modparam_fwpostfix, name);
		break;
	default:
		B43_WARN_ON(1);
		return -ENOSYS;
	}
	if (async) {
		/* do this part asynchronously */
		init_completion(&ctx->dev->fw_load_complete);
		err = request_firmware_nowait(THIS_MODULE, 1, ctx->fwname,
					      ctx->dev->dev->dev, GFP_KERNEL,
					      ctx, b43_fw_cb);
		if (err < 0) {
			pr_err("Unable to load firmware\n");
			return err;
		}
		wait_for_completion(&ctx->dev->fw_load_complete);
		if (ctx->blob)
			goto fw_ready;
	/* On some ARM systems, the async request will fail, but the next sync
	 * request works. For this reason, we fall through here
	 */
	}
	err = request_firmware(&ctx->blob, ctx->fwname,
			       ctx->dev->dev->dev);
	if (err == -ENOENT) {
		snprintf(ctx->errors[ctx->req_type],
			 sizeof(ctx->errors[ctx->req_type]),
			 "Firmware file \"%s\" not found\n",
			 ctx->fwname);
		return err;
	} else if (err) {
		snprintf(ctx->errors[ctx->req_type],
			 sizeof(ctx->errors[ctx->req_type]),
			 "Firmware file \"%s\" request failed (err=%d)\n",
			 ctx->fwname, err);
		return err;
	}
fw_ready:
	if (ctx->blob->size < sizeof(struct b43_fw_header))
		goto err_format;
	hdr = (struct b43_fw_header *)(ctx->blob->data);
	switch (hdr->type) {
	case B43_FW_TYPE_UCODE:
	case B43_FW_TYPE_PCM:
		size = be32_to_cpu(hdr->size);
		if (size != ctx->blob->size - sizeof(struct b43_fw_header))
			goto err_format;
		fallthrough;
	case B43_FW_TYPE_IV:
		if (hdr->ver != 1)
			goto err_format;
		break;
	default:
		goto err_format;
	}

	fw->data = ctx->blob;
	fw->filename = name;
	fw->type = ctx->req_type;

	return 0;

err_format:
	snprintf(ctx->errors[ctx->req_type],
		 sizeof(ctx->errors[ctx->req_type]),
		 "Firmware file \"%s\" format error.\n", ctx->fwname);
	release_firmware(ctx->blob);

	return -EPROTO;
}

/* https://bcm-v4.sipsolutions.net/802.11/Init/Firmware */
static int b43_try_request_fw(struct b43_request_fw_context *ctx)
{
	struct b43_wldev *dev = ctx->dev;
	struct b43_firmware *fw = &ctx->dev->fw;
	struct b43_phy *phy = &dev->phy;
	const u8 rev = ctx->dev->dev->core_rev;
	const char *filename;
	int err;

	/* Get microcode */
	filename = NULL;
	switch (rev) {
	case 42:
		if (phy->type == B43_PHYTYPE_AC)
			filename = "ucode42";
		break;
	case 40:
		if (phy->type == B43_PHYTYPE_AC)
			filename = "ucode40";
		break;
	case 33:
		if (phy->type == B43_PHYTYPE_LCN40)
			filename = "ucode33_lcn40";
		break;
	case 30:
		if (phy->type == B43_PHYTYPE_N)
			filename = "ucode30_mimo";
		break;
	case 29:
		if (phy->type == B43_PHYTYPE_HT)
			filename = "ucode29_mimo";
		break;
	case 26:
		if (phy->type == B43_PHYTYPE_HT)
			filename = "ucode26_mimo";
		break;
	case 28:
	case 25:
		if (phy->type == B43_PHYTYPE_N)
			filename = "ucode25_mimo";
		else if (phy->type == B43_PHYTYPE_LCN)
			filename = "ucode25_lcn";
		break;
	case 24:
		if (phy->type == B43_PHYTYPE_LCN)
			filename = "ucode24_lcn";
		break;
	case 23:
		if (phy->type == B43_PHYTYPE_N)
			filename = "ucode16_mimo";
		break;
	case 16 ... 19:
		if (phy->type == B43_PHYTYPE_N)
			filename = "ucode16_mimo";
		else if (phy->type == B43_PHYTYPE_LP)
			filename = "ucode16_lp";
		break;
	case 15:
		filename = "ucode15";
		break;
	case 14:
		filename = "ucode14";
		break;
	case 13:
		filename = "ucode13";
		break;
	case 11 ... 12:
		filename = "ucode11";
		break;
	case 5 ... 10:
		filename = "ucode5";
		break;
	}
	if (!filename)
		goto err_no_ucode;
	err = b43_do_request_fw(ctx, filename, &fw->ucode, true);
	if (err)
		goto err_load;

	/* Get PCM code */
	if ((rev >= 5) && (rev <= 10))
		filename = "pcm5";
	else if (rev >= 11)
		filename = NULL;
	else
		goto err_no_pcm;
	fw->pcm_request_failed = false;
	err = b43_do_request_fw(ctx, filename, &fw->pcm, false);
	if (err == -ENOENT) {
		/* We did not find a PCM file? Not fatal, but
		 * core rev <= 10 must do without hwcrypto then. */
		fw->pcm_request_failed = true;
	} else if (err)
		goto err_load;

	/* Get initvals */
	filename = NULL;
	switch (dev->phy.type) {
	case B43_PHYTYPE_G:
		if (rev == 13)
			filename = "b0g0initvals13";
		else if (rev >= 5 && rev <= 10)
			filename = "b0g0initvals5";
		break;
	case B43_PHYTYPE_N:
		if (rev == 30)
			filename = "n16initvals30";
		else if (rev == 28 || rev == 25)
			filename = "n0initvals25";
		else if (rev == 24)
			filename = "n0initvals24";
		else if (rev == 23)
			filename = "n0initvals16"; /* What about n0initvals22? */
		else if (rev >= 16 && rev <= 18)
			filename = "n0initvals16";
		else if (rev >= 11 && rev <= 12)
			filename = "n0initvals11";
		break;
	case B43_PHYTYPE_LP:
		if (rev >= 16 && rev <= 18)
			filename = "lp0initvals16";
		else if (rev == 15)
			filename = "lp0initvals15";
		else if (rev == 14)
			filename = "lp0initvals14";
		else if (rev == 13)
			filename = "lp0initvals13";
		break;
	case B43_PHYTYPE_HT:
		if (rev == 29)
			filename = "ht0initvals29";
		else if (rev == 26)
			filename = "ht0initvals26";
		break;
	case B43_PHYTYPE_LCN:
		if (rev == 24)
			filename = "lcn0initvals24";
		break;
	case B43_PHYTYPE_LCN40:
		if (rev == 33)
			filename = "lcn400initvals33";
		break;
	case B43_PHYTYPE_AC:
		if (rev == 42)
			filename = "ac1initvals42";
		else if (rev == 40)
			filename = "ac0initvals40";
		break;
	}
	if (!filename)
		goto err_no_initvals;
	err = b43_do_request_fw(ctx, filename, &fw->initvals, false);
	if (err)
		goto err_load;

	/* Get bandswitch initvals */
	filename = NULL;
	switch (dev->phy.type) {
	case B43_PHYTYPE_G:
		if (rev == 13)
			filename = "b0g0bsinitvals13";
		else if (rev >= 5 && rev <= 10)
			filename = "b0g0bsinitvals5";
		break;
	case B43_PHYTYPE_N:
		if (rev == 30)
			filename = "n16bsinitvals30";
		else if (rev == 28 || rev == 25)
			filename = "n0bsinitvals25";
		else if (rev == 24)
			filename = "n0bsinitvals24";
		else if (rev == 23)
			filename = "n0bsinitvals16"; /* What about n0bsinitvals22? */
		else if (rev >= 16 && rev <= 18)
			filename = "n0bsinitvals16";
		else if (rev >= 11 && rev <= 12)
			filename = "n0bsinitvals11";
		break;
	case B43_PHYTYPE_LP:
		if (rev >= 16 && rev <= 18)
			filename = "lp0bsinitvals16";
		else if (rev == 15)
			filename = "lp0bsinitvals15";
		else if (rev == 14)
			filename = "lp0bsinitvals14";
		else if (rev == 13)
			filename = "lp0bsinitvals13";
		break;
	case B43_PHYTYPE_HT:
		if (rev == 29)
			filename = "ht0bsinitvals29";
		else if (rev == 26)
			filename = "ht0bsinitvals26";
		break;
	case B43_PHYTYPE_LCN:
		if (rev == 24)
			filename = "lcn0bsinitvals24";
		break;
	case B43_PHYTYPE_LCN40:
		if (rev == 33)
			filename = "lcn400bsinitvals33";
		break;
	case B43_PHYTYPE_AC:
		if (rev == 42)
			filename = "ac1bsinitvals42";
		else if (rev == 40)
			filename = "ac0bsinitvals40";
		break;
	}
	if (!filename)
		goto err_no_initvals;
	err = b43_do_request_fw(ctx, filename, &fw->initvals_band, false);
	if (err)
		goto err_load;

	fw->opensource = (ctx->req_type == B43_FWTYPE_OPENSOURCE);

	return 0;

err_no_ucode:
	err = ctx->fatal_failure = -EOPNOTSUPP;
	b43err(dev->wl, "The driver does not know which firmware (ucode) "
	       "is required for your device (wl-core rev %u)\n", rev);
	goto error;

err_no_pcm:
	err = ctx->fatal_failure = -EOPNOTSUPP;
	b43err(dev->wl, "The driver does not know which firmware (PCM) "
	       "is required for your device (wl-core rev %u)\n", rev);
	goto error;

err_no_initvals:
	err = ctx->fatal_failure = -EOPNOTSUPP;
	b43err(dev->wl, "The driver does not know which firmware (initvals) "
	       "is required for your device (wl-core rev %u)\n", rev);
	goto error;

err_load:
	/* We failed to load this firmware image. The error message
	 * already is in ctx->errors. Return and let our caller decide
	 * what to do. */
	goto error;

error:
	b43_release_firmware(dev);
	return err;
}

static int b43_one_core_attach(struct b43_bus_dev *dev, struct b43_wl *wl);
static void b43_one_core_detach(struct b43_bus_dev *dev);
static int b43_rng_init(struct b43_wl *wl);

static void b43_request_firmware(struct work_struct *work)
{
	struct b43_wl *wl = container_of(work,
			    struct b43_wl, firmware_load);
	struct b43_wldev *dev = wl->current_dev;
	struct b43_request_fw_context *ctx;
	unsigned int i;
	int err;
	const char *errmsg;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return;
	ctx->dev = dev;

	ctx->req_type = B43_FWTYPE_PROPRIETARY;
	err = b43_try_request_fw(ctx);
	if (!err)
		goto start_ieee80211; /* Successfully loaded it. */
	/* Was fw version known? */
	if (ctx->fatal_failure)
		goto out;

	/* proprietary fw not found, try open source */
	ctx->req_type = B43_FWTYPE_OPENSOURCE;
	err = b43_try_request_fw(ctx);
	if (!err)
		goto start_ieee80211; /* Successfully loaded it. */
	if(ctx->fatal_failure)
		goto out;

	/* Could not find a usable firmware. Print the errors. */
	for (i = 0; i < B43_NR_FWTYPES; i++) {
		errmsg = ctx->errors[i];
		if (strlen(errmsg))
			b43err(dev->wl, "%s", errmsg);
	}
	b43_print_fw_helptext(dev->wl, 1);
	goto out;

start_ieee80211:
	wl->hw->queues = B43_QOS_QUEUE_NUM;
	if (!modparam_qos || dev->fw.opensource)
		wl->hw->queues = 1;

	err = ieee80211_register_hw(wl->hw);
	if (err)
		goto out;
	wl->hw_registered = true;
	b43_leds_register(wl->current_dev);

	/* Register HW RNG driver */
	b43_rng_init(wl);

out:
	kfree(ctx);
}

static int b43_upload_microcode(struct b43_wldev *dev)
{
	struct wiphy *wiphy = dev->wl->hw->wiphy;
	const size_t hdr_len = sizeof(struct b43_fw_header);
	const __be32 *data;
	unsigned int i, len;
	u16 fwrev, fwpatch, fwdate, fwtime;
	u32 tmp, macctl;
	int err = 0;

	/* Jump the microcode PSM to offset 0 */
	macctl = b43_read32(dev, B43_MMIO_MACCTL);
	B43_WARN_ON(macctl & B43_MACCTL_PSM_RUN);
	macctl |= B43_MACCTL_PSM_JMP0;
	b43_write32(dev, B43_MMIO_MACCTL, macctl);
	/* Zero out all microcode PSM registers and shared memory. */
	for (i = 0; i < 64; i++)
		b43_shm_write16(dev, B43_SHM_SCRATCH, i, 0);
	for (i = 0; i < 4096; i += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, i, 0);

	/* Upload Microcode. */
	data = (__be32 *) (dev->fw.ucode.data->data + hdr_len);
	len = (dev->fw.ucode.data->size - hdr_len) / sizeof(__be32);
	b43_shm_control_word(dev, B43_SHM_UCODE | B43_SHM_AUTOINC_W, 0x0000);
	for (i = 0; i < len; i++) {
		b43_write32(dev, B43_MMIO_SHM_DATA, be32_to_cpu(data[i]));
		udelay(10);
	}

	if (dev->fw.pcm.data) {
		/* Upload PCM data. */
		data = (__be32 *) (dev->fw.pcm.data->data + hdr_len);
		len = (dev->fw.pcm.data->size - hdr_len) / sizeof(__be32);
		b43_shm_control_word(dev, B43_SHM_HW, 0x01EA);
		b43_write32(dev, B43_MMIO_SHM_DATA, 0x00004000);
		/* No need for autoinc bit in SHM_HW */
		b43_shm_control_word(dev, B43_SHM_HW, 0x01EB);
		for (i = 0; i < len; i++) {
			b43_write32(dev, B43_MMIO_SHM_DATA, be32_to_cpu(data[i]));
			udelay(10);
		}
	}

	b43_write32(dev, B43_MMIO_GEN_IRQ_REASON, B43_IRQ_ALL);

	/* Start the microcode PSM */
	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_PSM_JMP0,
		      B43_MACCTL_PSM_RUN);

	/* Wait for the microcode to load and respond */
	i = 0;
	while (1) {
		tmp = b43_read32(dev, B43_MMIO_GEN_IRQ_REASON);
		if (tmp == B43_IRQ_MAC_SUSPENDED)
			break;
		i++;
		if (i >= 20) {
			b43err(dev->wl, "Microcode not responding\n");
			b43_print_fw_helptext(dev->wl, 1);
			err = -ENODEV;
			goto error;
		}
		msleep(50);
	}
	b43_read32(dev, B43_MMIO_GEN_IRQ_REASON);	/* dummy read */

	/* Get and check the revisions. */
	fwrev = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_UCODEREV);
	fwpatch = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_UCODEPATCH);
	fwdate = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_UCODEDATE);
	fwtime = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_UCODETIME);

	if (fwrev <= 0x128) {
		b43err(dev->wl, "YOUR FIRMWARE IS TOO OLD. Firmware from "
		       "binary drivers older than version 4.x is unsupported. "
		       "You must upgrade your firmware files.\n");
		b43_print_fw_helptext(dev->wl, 1);
		err = -EOPNOTSUPP;
		goto error;
	}
	dev->fw.rev = fwrev;
	dev->fw.patch = fwpatch;
	/* The AC microcode is told by the core, as b43_request_firmware()
	 * picks it: its revisions are not ordered against the others'. */
	if (dev->phy.type == B43_PHYTYPE_AC && dev->dev->core_rev == 42)
		dev->fw.hdr_format = B43_FW_HDR_AC;
	else if (dev->fw.rev >= 598)
		dev->fw.hdr_format = B43_FW_HDR_598;
	else if (dev->fw.rev >= 410)
		dev->fw.hdr_format = B43_FW_HDR_410;
	else
		dev->fw.hdr_format = B43_FW_HDR_351;
	dev->fw.tpl = b43_tpl_layout_find(dev);
	if (!dev->fw.tpl)
		b43warn(dev->wl, "Template RAM layout of firmware %u unknown: "
			"no AP, mesh or IBSS\n", dev->fw.rev);
	WARN_ON(dev->fw.opensource != (fwdate == 0xFFFF));

	dev->qos_enabled = dev->wl->hw->queues > 1;
	/*
	 * Default to firmware/hardware crypto acceleration, except on the AC
	 * microcode: its cipher numbers are not b43's (WEP104 is 3 there, and
	 * a 16 byte pairwise key 5), and the key fields of its TX and RX
	 * headers are not mapped. mac80211 does the crypto instead.
	 */
	dev->hwcrypto_enabled = dev->fw.hdr_format != B43_FW_HDR_AC;

	if (dev->fw.opensource) {
		u16 fwcapa;

		/* Patchlevel info is encoded in the "time" field. */
		dev->fw.patch = fwtime;
		b43info(dev->wl, "Loading OpenSource firmware version %u.%u\n",
			dev->fw.rev, dev->fw.patch);

		fwcapa = b43_fwcapa_read(dev);
		if (!(fwcapa & B43_FWCAPA_HWCRYPTO) || dev->fw.pcm_request_failed) {
			b43info(dev->wl, "Hardware crypto acceleration not supported by firmware\n");
			/* Disable hardware crypto and fall back to software crypto. */
			dev->hwcrypto_enabled = false;
		}
		/* adding QoS support should use an offline discovery mechanism */
		WARN(fwcapa & B43_FWCAPA_QOS, "QoS in OpenFW not supported\n");
	} else {
		b43info(dev->wl, "Loading firmware version %u.%u "
			"(20%.2i-%.2i-%.2i %.2i:%.2i:%.2i)\n",
			fwrev, fwpatch,
			(fwdate >> 12) & 0xF, (fwdate >> 8) & 0xF, fwdate & 0xFF,
			(fwtime >> 11) & 0x1F, (fwtime >> 5) & 0x3F, fwtime & 0x1F);
		if (dev->fw.pcm_request_failed) {
			b43warn(dev->wl, "No \"pcm5.fw\" firmware file found. "
				"Hardware accelerated cryptography is disabled.\n");
			b43_print_fw_helptext(dev->wl, 0);
		}
	}

	snprintf(wiphy->fw_version, sizeof(wiphy->fw_version), "%u.%u",
			dev->fw.rev, dev->fw.patch);
	wiphy->hw_version = dev->dev->core_id;

	if (dev->fw.hdr_format == B43_FW_HDR_351) {
		/* We're over the deadline, but we keep support for old fw
		 * until it turns out to be in major conflict with something new. */
		b43warn(dev->wl, "You are using an old firmware image. "
			"Support for old firmware will be removed soon "
			"(official deadline was July 2008).\n");
		b43_print_fw_helptext(dev->wl, 0);
	}

	return 0;

error:
	/* Stop the microcode PSM. */
	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_PSM_RUN,
		      B43_MACCTL_PSM_JMP0);

	return err;
}

static int b43_write_initvals(struct b43_wldev *dev,
			      const struct b43_iv *ivals,
			      size_t count,
			      size_t array_size)
{
	const struct b43_iv *iv;
	u16 offset;
	size_t i;
	bool bit32;

	BUILD_BUG_ON(sizeof(struct b43_iv) != 6);
	iv = ivals;
	for (i = 0; i < count; i++) {
		if (array_size < sizeof(iv->offset_size))
			goto err_format;
		array_size -= sizeof(iv->offset_size);
		offset = be16_to_cpu(iv->offset_size);
		bit32 = !!(offset & B43_IV_32BIT);
		offset &= B43_IV_OFFSET_MASK;
		if (offset >= 0x1000)
			goto err_format;
		if (bit32) {
			u32 value;

			if (array_size < sizeof(iv->data.d32))
				goto err_format;
			array_size -= sizeof(iv->data.d32);

			value = get_unaligned_be32(&iv->data.d32);
			b43_write32(dev, offset, value);

			iv = (const struct b43_iv *)((const uint8_t *)iv +
							sizeof(__be16) +
							sizeof(__be32));
		} else {
			u16 value;

			if (array_size < sizeof(iv->data.d16))
				goto err_format;
			array_size -= sizeof(iv->data.d16);

			value = be16_to_cpu(iv->data.d16);
			b43_write16(dev, offset, value);

			iv = (const struct b43_iv *)((const uint8_t *)iv +
							sizeof(__be16) +
							sizeof(__be16));
		}
	}
	if (array_size)
		goto err_format;

	return 0;

err_format:
	b43err(dev->wl, "Initial Values Firmware file-format error.\n");
	b43_print_fw_helptext(dev->wl, 1);

	return -EPROTO;
}

static int b43_upload_initvals(struct b43_wldev *dev)
{
	const size_t hdr_len = sizeof(struct b43_fw_header);
	const struct b43_fw_header *hdr;
	struct b43_firmware *fw = &dev->fw;
	const struct b43_iv *ivals;
	size_t count;

	hdr = (const struct b43_fw_header *)(fw->initvals.data->data);
	ivals = (const struct b43_iv *)(fw->initvals.data->data + hdr_len);
	count = be32_to_cpu(hdr->size);
	return b43_write_initvals(dev, ivals, count,
				 fw->initvals.data->size - hdr_len);
}

static int b43_upload_initvals_band(struct b43_wldev *dev)
{
	const size_t hdr_len = sizeof(struct b43_fw_header);
	const struct b43_fw_header *hdr;
	struct b43_firmware *fw = &dev->fw;
	const struct b43_iv *ivals;
	size_t count;

	if (!fw->initvals_band.data)
		return 0;

	hdr = (const struct b43_fw_header *)(fw->initvals_band.data->data);
	ivals = (const struct b43_iv *)(fw->initvals_band.data->data + hdr_len);
	count = be32_to_cpu(hdr->size);
	return b43_write_initvals(dev, ivals, count,
				  fw->initvals_band.data->size - hdr_len);
}

/* Initialize the GPIOs
 * https://bcm-specs.sipsolutions.net/GPIO
 */

#ifdef CONFIG_B43_SSB
static struct ssb_device *b43_ssb_gpio_dev(struct b43_wldev *dev)
{
	struct ssb_bus *bus = dev->dev->sdev->bus;

#ifdef CONFIG_SSB_DRIVER_PCICORE
	return (bus->chipco.dev ? bus->chipco.dev : bus->pcicore.dev);
#else
	return bus->chipco.dev;
#endif
}
#endif

#ifdef CONFIG_B43_BCMA
/*
 * The GPIO setup of the AC cores' stock driver, wl 6.30 and 7.14 alike: the
 * MAC gets no pin, and the ChipCommon drives the LED pins, under the LED
 * timer, written dark before their outputs are enabled. b43_led_turn_on()
 * drives them there on these cores.
 */
static void b43_gpio_init_ac(struct b43_wldev *dev)
{
	struct bcma_drv_cc *cc = &dev->dev->bdev->bus->drv_cc;
	u32 leds = b43_leds_gpio_mask(dev->wl);

	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_GPOUTSMSK, 0);
	b43_write16(dev, B43_MMIO_GPIO_MASK, 0);

	bcma_chipco_gpio_control(cc, leds, 0);
	bcma_cc_maskset32(cc, BCMA_CC_GPIOTOUTM, ~leds, leds);
	bcma_chipco_gpio_out(cc, leds, b43_leds_gpio_activelow(dev->wl));
	bcma_chipco_gpio_outen(cc, leds, leds);
}
#endif

static int b43_gpio_init(struct b43_wldev *dev)
{
#ifdef CONFIG_B43_SSB
	struct ssb_device *gpiodev;
#endif
	u32 leds = b43_leds_gpio_mask(dev->wl);
	u32 mask, set;

#ifdef CONFIG_B43_BCMA
	if (b43_is_ac_core(dev)) {
		b43_gpio_init_ac(dev);
		return 0;
	}
#endif

	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_GPOUTSMSK, 0);
	b43_maskset16(dev, B43_MMIO_GPIO_MASK, ~0, 0xF | leds);

	mask = 0x0000001F | leds;
	set = 0x0000000F | leds;
	if (dev->dev->chip_id == 0x4301) {
		mask |= 0x0060;
		set |= 0x0060;
	} else if (dev->dev->chip_id == 0x5354) {
		/* Don't allow overtaking buttons GPIOs */
		set &= 0x2; /* 0x2 is LED GPIO on BCM5354 */
	}

	if (0 /* FIXME: conditional unknown */ ) {
		b43_write16(dev, B43_MMIO_GPIO_MASK,
			    b43_read16(dev, B43_MMIO_GPIO_MASK)
			    | 0x0100);
		/* BT Coexistance Input */
		mask |= 0x0080;
		set |= 0x0080;
		/* BT Coexistance Out */
		mask |= 0x0100;
		set |= 0x0100;
	}
	if (dev->dev->bus_sprom->boardflags_lo & B43_BFL_PACTRL) {
		/* PA is controlled by gpio 9, let ucode handle it */
		b43_write16(dev, B43_MMIO_GPIO_MASK,
			    b43_read16(dev, B43_MMIO_GPIO_MASK)
			    | 0x0200);
		mask |= 0x0200;
		set |= 0x0200;
	}

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		bcma_chipco_gpio_control(&dev->dev->bdev->bus->drv_cc, mask, set);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		gpiodev = b43_ssb_gpio_dev(dev);
		if (gpiodev)
			ssb_write32(gpiodev, B43_GPIO_CONTROL,
				    (ssb_read32(gpiodev, B43_GPIO_CONTROL)
				    & ~mask) | set);
		break;
#endif
	}

	return 0;
}

/* Turn off all GPIO stuff. Call this on module unload, for example. */
static void b43_gpio_cleanup(struct b43_wldev *dev)
{
#ifdef CONFIG_B43_SSB
	struct ssb_device *gpiodev;
#endif

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		bcma_chipco_gpio_control(&dev->dev->bdev->bus->drv_cc, ~0, 0);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		gpiodev = b43_ssb_gpio_dev(dev);
		if (gpiodev)
			ssb_write32(gpiodev, B43_GPIO_CONTROL, 0);
		break;
#endif
	}
}

/* http://bcm-specs.sipsolutions.net/EnableMac */
void b43_mac_enable(struct b43_wldev *dev)
{
	if (b43_debug(dev, B43_DBG_FIRMWARE)) {
		u16 fwstate;

		fwstate = b43_shm_read16(dev, B43_SHM_SHARED,
					 B43_SHM_SH_UCODESTAT);
		if ((fwstate != B43_SHM_SH_UCODESTAT_SUSP) &&
		    (fwstate != B43_SHM_SH_UCODESTAT_SLEEP)) {
			b43err(dev->wl, "b43_mac_enable(): The firmware "
			       "should be suspended, but current state is %u\n",
			       fwstate);
		}
	}

	dev->mac_suspended--;
	B43_WARN_ON(dev->mac_suspended < 0);
	if (dev->mac_suspended == 0) {
		b43_maskset32(dev, B43_MMIO_MACCTL, ~0, B43_MACCTL_ENABLED);
		b43_write32(dev, B43_MMIO_GEN_IRQ_REASON,
			    B43_IRQ_MAC_SUSPENDED);
		/* Commit writes */
		b43_read32(dev, B43_MMIO_MACCTL);
		b43_read32(dev, B43_MMIO_GEN_IRQ_REASON);
		b43_power_saving_ctl_bits(dev, 0);
	}
}

/* https://bcm-specs.sipsolutions.net/SuspendMAC */
void b43_mac_suspend(struct b43_wldev *dev)
{
	int i;
	u32 tmp;

	might_sleep();
	B43_WARN_ON(dev->mac_suspended < 0);

	if (dev->mac_suspended == 0) {
		b43_power_saving_ctl_bits(dev, B43_PS_AWAKE);
		b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_ENABLED, 0);
		/* force pci to flush the write */
		b43_read32(dev, B43_MMIO_MACCTL);
		for (i = 35; i; i--) {
			tmp = b43_read32(dev, B43_MMIO_GEN_IRQ_REASON);
			if (tmp & B43_IRQ_MAC_SUSPENDED)
				goto out;
			udelay(10);
		}
		/* Hm, it seems this will take some time. Use msleep(). */
		for (i = 40; i; i--) {
			tmp = b43_read32(dev, B43_MMIO_GEN_IRQ_REASON);
			if (tmp & B43_IRQ_MAC_SUSPENDED)
				goto out;
			msleep(1);
		}
		b43err(dev->wl, "MAC suspend failed\n");
	}
out:
	dev->mac_suspended++;
}

/* https://bcm-v4.sipsolutions.net/802.11/PHY/N/MacPhyClkSet */
void b43_mac_phy_clock_set(struct b43_wldev *dev, bool on)
{
	u32 tmp;

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		tmp = bcma_aread32(dev->dev->bdev, BCMA_IOCTL);
		if (on)
			tmp |= B43_BCMA_IOCTL_MACPHYCLKEN;
		else
			tmp &= ~B43_BCMA_IOCTL_MACPHYCLKEN;
		bcma_awrite32(dev->dev->bdev, BCMA_IOCTL, tmp);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		tmp = ssb_read32(dev->dev->sdev, SSB_TMSLOW);
		if (on)
			tmp |= B43_TMSLOW_MACPHYCLKEN;
		else
			tmp &= ~B43_TMSLOW_MACPHYCLKEN;
		ssb_write32(dev->dev->sdev, SSB_TMSLOW, tmp);
		break;
#endif
	}
}

/* brcms_b_switch_macfreq */
void b43_mac_switch_freq(struct b43_wldev *dev, u8 spurmode)
{
	u16 chip_id = dev->dev->chip_id;

	if (chip_id == BCMA_CHIP_ID_BCM4352 || chip_id == BCMA_CHIP_ID_BCM4360) {
		/*
		 * bcma programs the PMU PLL of these chips with
		 * pllcontrol[2..3] = 0x0c31, 0x100e: the VCO runs at
		 * 40 MHz * (24 + 0x100e / 2^24) and the MAC at a sixth of it,
		 * 160.0016 MHz, so 2^26 / 160.0016 = 0x66662. The OEM driver
		 * writes it after reading the PLL back, with no spur mode
		 * involved.
		 */
		b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x6662);
		b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x6);
	} else if (chip_id == BCMA_CHIP_ID_BCM4331) {
		switch (spurmode) {
		case 2: /* 168 Mhz: 2^26/168 = 0x61862 */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x1862);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x6);
			break;
		case 1: /* 164 Mhz: 2^26/164 = 0x63e70 */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x3e70);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x6);
			break;
		default: /* 160 Mhz: 2^26/160 = 0x66666 */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x6666);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x6);
			break;
		}
	} else if (chip_id == BCMA_CHIP_ID_BCM43131 ||
	    chip_id == BCMA_CHIP_ID_BCM43217 ||
	    chip_id == BCMA_CHIP_ID_BCM43222 ||
	    chip_id == BCMA_CHIP_ID_BCM43224 ||
	    chip_id == BCMA_CHIP_ID_BCM43225 ||
	    chip_id == BCMA_CHIP_ID_BCM43227 ||
	    chip_id == BCMA_CHIP_ID_BCM43228) {
		switch (spurmode) {
		case 2: /* 126 Mhz */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x2082);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x8);
			break;
		case 1: /* 123 Mhz */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x5341);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x8);
			break;
		default: /* 120 Mhz */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x8889);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0x8);
			break;
		}
	} else if (dev->phy.type == B43_PHYTYPE_LCN) {
		switch (spurmode) {
		case 1: /* 82 Mhz */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0x7CE0);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0xC);
			break;
		default: /* 80 Mhz */
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_LOW, 0xCCCD);
			b43_write16(dev, B43_MMIO_TSF_CLK_FRAC_HIGH, 0xC);
			break;
		}
	}
}

static void b43_adjust_opmode(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;
	u32 ctl;
	u16 cfp_pretbtt;

	ctl = b43_read32(dev, B43_MMIO_MACCTL);
	/* Reset status to STA infrastructure mode. */
	ctl &= ~B43_MACCTL_AP;
	ctl &= ~B43_MACCTL_KEEP_CTL;
	ctl &= ~B43_MACCTL_KEEP_BADPLCP;
	ctl &= ~B43_MACCTL_KEEP_BAD;
	ctl &= ~B43_MACCTL_PROMISC;
	ctl &= ~B43_MACCTL_BEACPROMISC;
	ctl |= B43_MACCTL_INFRA;

	if ((b43_is_mode(wl, NL80211_IFTYPE_AP) ||
	     b43_is_mode(wl, NL80211_IFTYPE_MESH_POINT)) && !dev->cac_pending)
		ctl |= B43_MACCTL_AP;
	else if (b43_is_mode(wl, NL80211_IFTYPE_ADHOC))
		ctl &= ~B43_MACCTL_INFRA;

	if (wl->filter_flags & FIF_CONTROL)
		ctl |= B43_MACCTL_KEEP_CTL;
	if (wl->filter_flags & FIF_FCSFAIL)
		ctl |= B43_MACCTL_KEEP_BAD;
	if (wl->filter_flags & FIF_PLCPFAIL)
		ctl |= B43_MACCTL_KEEP_BADPLCP;
	if (wl->filter_flags & FIF_BCN_PRBRESP_PROMISC)
		ctl |= B43_MACCTL_BEACPROMISC;
	/*
	 * The AC cores' stock driver runs an AP with beacon promiscuity on
	 * (MACCONTROL 0x0416040x at the bus on every board captured); it sets
	 * it wherever an AP is active on an HT-capable band.
	 */
	if ((ctl & B43_MACCTL_AP) && dev->phy.type == B43_PHYTYPE_AC)
		ctl |= B43_MACCTL_BEACPROMISC;

	/* Workaround: On old hardware the HW-MAC-address-filter
	 * doesn't work properly, so always run promisc in filter
	 * it in software. */
	if (dev->dev->core_rev <= 4)
		ctl |= B43_MACCTL_PROMISC;

	b43_write32(dev, B43_MMIO_MACCTL, ctl);

	cfp_pretbtt = 2;
	if ((ctl & B43_MACCTL_INFRA) && !(ctl & B43_MACCTL_AP)) {
		if (dev->dev->chip_id == 0x4306 &&
		    dev->dev->chip_rev == 3)
			cfp_pretbtt = 100;
		else
			cfp_pretbtt = 50;
	}
	b43_write16(dev, 0x612, cfp_pretbtt);

	/*
	 * Beacon TSF offset. Both the 6.30 and the 7.14 AC microcode start from
	 * 0x183, the band initvals' value; 7.14 moves it to 0x3a in the BSS
	 * setup of an AP, and the 6.30 station keeps 0x183. IBSS, which also
	 * beacons, has no capture and takes the AP value.
	 */
	if (dev->dev->core_rev >= 42) {
		bool sta = (ctl & B43_MACCTL_INFRA) && !(ctl & B43_MACCTL_AP);

		/* A beaconing BSS restarts its DTIM count, as on the agcombo. */
		if (!sta)
			b43_shm_write16(dev, B43_SHM_SCRATCH,
					B43_SHM_SC_DTIMC, 0);
		b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_BTSFOFF,
				sta ? 0x0183 : 0x003a);
	}

	/* FIXME: We don't currently implement the PMQ mechanism,
	 *        so always disable it. If we want to implement PMQ,
	 *        we need to enable it here (clear DISCPMQ) in AP mode.
	 */
	if (0  /* ctl & B43_MACCTL_AP */)
		b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_DISCPMQ, 0);
	else
		b43_maskset32(dev, B43_MMIO_MACCTL, ~0, B43_MACCTL_DISCPMQ);
}

static void b43_rate_memory_write(struct b43_wldev *dev, u16 rate, int is_ofdm)
{
	u16 offset;

	if (is_ofdm) {
		offset = 0x480;
		offset += (b43_plcp_get_ratecode_ofdm(rate) & 0x000F) * 2;
	} else {
		offset = 0x4C0;
		offset += (b43_plcp_get_ratecode_cck(rate) & 0x000F) * 2;
	}
	b43_shm_write16(dev, B43_SHM_SHARED, offset + 0x20,
			b43_shm_read16(dev, B43_SHM_SHARED, offset));
}

static void b43_rate_memory_init(struct b43_wldev *dev)
{
	switch (dev->phy.type) {
	case B43_PHYTYPE_G:
	case B43_PHYTYPE_N:
	case B43_PHYTYPE_LP:
	case B43_PHYTYPE_HT:
	case B43_PHYTYPE_LCN:
	case B43_PHYTYPE_AC:
		b43_rate_memory_write(dev, B43_OFDM_RATE_6MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_9MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_12MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_18MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_24MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_36MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_48MB, 1);
		b43_rate_memory_write(dev, B43_OFDM_RATE_54MB, 1);
		fallthrough;
	case B43_PHYTYPE_B:
		b43_rate_memory_write(dev, B43_CCK_RATE_1MB, 0);
		b43_rate_memory_write(dev, B43_CCK_RATE_2MB, 0);
		b43_rate_memory_write(dev, B43_CCK_RATE_5MB, 0);
		b43_rate_memory_write(dev, B43_CCK_RATE_11MB, 0);
		break;
	default:
		B43_WARN_ON(1);
	}
}

/* Set the default values for the PHY TX Control Words. */
static void b43_set_phytxctl_defaults(struct b43_wldev *dev)
{
	u16 ctl = 0;

	if (!b43_phytxctl_in_shm(dev))
		return;

	ctl |= B43_TXH_PHY_ENC_CCK;
	ctl |= B43_TXH_PHY_ANT01AUTO;
	ctl |= B43_TXH_PHY_TXPWR;

	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL, ctl);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_ACKCTSPHYCTL, ctl);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PRPHYCTL, ctl);
}

/* Set the TX-Antenna for management frames sent by firmware. */
static void b43_mgmtframe_txantenna(struct b43_wldev *dev, int antenna)
{
	u16 ant;
	u16 tmp;

	if (!b43_phytxctl_in_shm(dev))
		return;
	ant = b43_antenna_to_phyctl(antenna);

	/* For ACK/CTS */
	tmp = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_ACKCTSPHYCTL);
	tmp = (tmp & ~B43_TXH_PHY_ANT) | ant;
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_ACKCTSPHYCTL, tmp);
	/* For Probe Resposes */
	tmp = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_PRPHYCTL);
	tmp = (tmp & ~B43_TXH_PHY_ANT) | ant;
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PRPHYCTL, tmp);
}


/*
 * TX FIFO geometry of the 802.11 core revision 42.
 *
 * The OEM driver programs it right after the initvals and before touching
 * the PHY: it flushes the FIFO block pool, allocates blocks to the template
 * FIFO (selector 0x17) and to FIFOs 0-5 (selectors 0x10-0x15), then chains
 * the 42 blocks of the pool, each entry naming a block and the two after it.
 * Nothing in this block is documented for this core; the register names are
 * the ones b43.h carries for the same offsets on the older layout, and the
 * values are the OEM driver's, the same on every bring-up of a BCM4352 and a
 * BCM4360. The microcode does not size the FIFOs by itself, so without this
 * they keep whatever geometry the previous driver left in the hardware.
 *
 * Both waits are the OEM driver's, as the bus shows them on the agcombo. The
 * flush polls the command's busy bit and reads it once more to check it.
 * Each chain entry waits for its command field to clear, 21 retries 10 us
 * apart, and on these chips it never does: all 22 reads of every entry
 * return the command with the go bit already clear, the same on each
 * bring-up. So the wait always runs out, and what says whether the entry was
 * taken is the go bit.
 */
#define B43_TXFIFO_CHAIN_RETRIES	21
#define B43_TXFIFO_BLOCKS_REV42		42
#define B43_TXFIFO_CHAIN_GO		0x8000
#define B43_TXFIFO_CHAIN_CMD		0x0007

static void b43_txfifo_alloc(struct b43_wldev *dev, u16 rqpri, u16 txptr,
			     u16 def, u16 aux, u16 sel)
{
	b43_write16(dev, B43_MMIO_XMTFIFORQPRI, rqpri);
	b43_write16(dev, B43_MMIO_XMTTPLATETXPTR, txptr);
	b43_write16(dev, B43_MMIO_XMTFIFODEF, def);
	b43_write16(dev, B43_MMIO_XMTFIFO_54E, aux);
	b43_write16(dev, B43_MMIO_XMTTPLATEPTR, 0x740c);
	b43_write16(dev, B43_MMIO_XMTFIFOPRIRDY, sel);
}

static int b43_txfifo_init_rev42(struct b43_wldev *dev)
{
	const unsigned int n = B43_TXFIFO_BLOCKS_REV42;
	unsigned int i, fifo, tries;

	b43_write16(dev, B43_MMIO_XMTFIFOFLUSH, 0x0600);
	b43_write16(dev, B43_MMIO_XMTFIFOCMD, 0x0005);
	for (tries = 0; b43_read16(dev, B43_MMIO_XMTFIFOCMD) & 0x0001; tries++) {
		if (tries >= 100)
			break;
		udelay(10);
	}
	if (b43_read16(dev, B43_MMIO_XMTFIFOCMD) & 0x0001)
		return -ETIMEDOUT;

	b43_txfifo_alloc(dev, 0x002a, 0x002a, 0x002a, 0x262a, 0x17);
	for (fifo = 0; fifo < 6; fifo++)
		b43_txfifo_alloc(dev, 0x05d6, 0x0020, 0x000b, 0x1216, 0x10 | fifo);

	for (i = 0; i < n; i++) {
		b43_write16(dev, B43_MMIO_AGGFIFODATA2, i);
		b43_write16(dev, B43_MMIO_AGGFIFODATA3, min(i + 2, n - 1));
		b43_write16(dev, B43_MMIO_AGGFIFODATA1, min(3u, n - i));
		b43_write16(dev, B43_MMIO_AGGFIFODATA0,
			    B43_TXFIFO_CHAIN_GO | (i << 4) | B43_TXFIFO_CHAIN_CMD);
		for (tries = 0; ; tries++) {
			u16 v = b43_read16(dev, B43_MMIO_AGGFIFODATA0);

			if (!(v & B43_TXFIFO_CHAIN_CMD))
				break;
			if (tries == B43_TXFIFO_CHAIN_RETRIES) {
				if (v & B43_TXFIFO_CHAIN_GO)
					return -ETIMEDOUT;
				break;
			}
			udelay(10);
		}
	}
	return 0;
}

/*
 * Airtime of an OFDM frame of @len bytes at 5 GHz, preamble and SIGNAL
 * included, from the bits per symbol of its rate.
 */
static u16 b43_ofdm_airtime(unsigned int len, unsigned int ndbps)
{
	return 20 + 4 * DIV_ROUND_UP(16 + 8 * len + 6, ndbps);
}

/*
 * What the AC cores' stock driver writes after the band initvals, the same
 * on the agcombo (7.14) and on the MacBookAir6,1 (6.30), where it repeats
 * on every band init:
 *
 *  - four cells whose meaning is not known: 0x000c = 0x00c0,
 *    0x000e = 0x000a, 0x17d2 = 0x042b, 0x17d4 = 0x0100;
 *  - for each OFDM rate, in the block the direct map points to, SIFS plus
 *    the airtime of a 14-, 32- and 20-byte frame (ACK, BlockAck, RTS) at
 *    +6, +16 and +18;
 *  - the first three words of the 9 Mb/s block, the one rate the initvals
 *    leave empty: 0x0014, 0x01c0 | SIGNAL rate, 0x0002.
 */
#define B43_RT_ACK_TIME		6
#define B43_RT_BA_TIME		16
#define B43_RT_RTS_TIME		18
#define B43_SIFS_5GHZ		16

static void b43_bsinit_ac(struct b43_wldev *dev)
{
	static const struct {
		u8 signal;
		u8 ndbps;
	} rates[] = {
		{ 0xb, 24 }, { 0xf, 36 }, { 0xa, 48 }, { 0xe, 72 },
		{ 0x9, 96 }, { 0xd, 144 }, { 0x8, 192 }, { 0xc, 216 },
	};
	unsigned int i;

	b43_shm_write16(dev, B43_SHM_SHARED, 0x000c, 0x00c0);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x000e, 0x000a);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x17d2, 0x042b);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x17d4, 0x0100);

	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		u16 block = 2 * b43_shm_read16(dev, B43_SHM_SHARED,
					       B43_SHM_SH_OFDMDIRECT +
					       2 * rates[i].signal);

		if (rates[i].signal == 0xf) {
			b43_shm_write16(dev, B43_SHM_SHARED, block, 0x0014);
			b43_shm_write16(dev, B43_SHM_SHARED, block + 2,
					0x01c0 | rates[i].signal);
			b43_shm_write16(dev, B43_SHM_SHARED, block + 4, 0x0002);
		}
		b43_shm_write16(dev, B43_SHM_SHARED, block + B43_RT_ACK_TIME,
				B43_SIFS_5GHZ +
				b43_ofdm_airtime(14, rates[i].ndbps));
		b43_shm_write16(dev, B43_SHM_SHARED, block + B43_RT_BA_TIME,
				B43_SIFS_5GHZ +
				b43_ofdm_airtime(32, rates[i].ndbps));
		b43_shm_write16(dev, B43_SHM_SHARED, block + B43_RT_RTS_TIME,
				B43_SIFS_5GHZ +
				b43_ofdm_airtime(20, rates[i].ndbps));
	}
}

static int b43_bsinit(struct b43_wldev *dev)
{
	int err = b43_upload_initvals_band(dev);

	if (!err && dev->phy.type == B43_PHYTYPE_AC)
		b43_bsinit_ac(dev);
	return err;
}

/* This is the opposite of b43_chip_init() */
static void b43_chip_exit(struct b43_wldev *dev)
{
	b43_phy_exit(dev);
	b43_gpio_cleanup(dev);
	/* firmware is released later */
}

/* Initialize the chip
 * https://bcm-specs.sipsolutions.net/ChipInit
 */
/*
 * The MAC half of the chip init: the operating mode, the TSF CFP timings, the
 * interrupt masks, the MAC-PHY clock and the power-up delay.
 */
static void b43_chip_init_mac(struct b43_wldev *dev)
{
	b43_write32(dev, 0x0100, 0x01000000);
	if (dev->dev->core_rev < 5)
		b43_write32(dev, 0x010C, 0x01000000);

	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_INFRA, 0);
	b43_maskset32(dev, B43_MMIO_MACCTL, ~0, B43_MACCTL_INFRA);

	/* Probe Response Timeout value */
	/* FIXME: Default to 0, has to be set by ioctl probably... :-/ */
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PRMAXTIME, 0);

	/* Initially set the wireless operation mode. */
	b43_adjust_opmode(dev);

	if (dev->dev->core_rev < 3) {
		b43_write16(dev, 0x060E, 0x0000);
		b43_write16(dev, 0x0610, 0x8000);
		b43_write16(dev, 0x0604, 0x0000);
		b43_write16(dev, 0x0606, 0x0200);
	} else {
		b43_write32(dev, 0x0188, 0x80000000);
		b43_write32(dev, 0x018C, 0x02000000);
	}
	b43_write32(dev, B43_MMIO_GEN_IRQ_REASON, 0x00004000);
	b43_write32(dev, B43_MMIO_DMA0_IRQ_MASK, 0x0001FC00);
	b43_write32(dev, B43_MMIO_DMA1_IRQ_MASK, 0x0000DC00);
	b43_write32(dev, B43_MMIO_DMA2_IRQ_MASK, 0x0000DC00);
	b43_write32(dev, B43_MMIO_DMA3_IRQ_MASK, 0x0001DC00);
	b43_write32(dev, B43_MMIO_DMA4_IRQ_MASK, 0x0000DC00);
	b43_write32(dev, B43_MMIO_DMA5_IRQ_MASK, 0x0000DC00);

	b43_mac_phy_clock_set(dev, true);

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		/*
		 * FIXME: 0xE74 is quite common, but should be read from CC.
		 * The AC cores' stock driver writes 1500 us, on the BCM4352 and
		 * the BCM4360 and with both driver versions captured.
		 */
		b43_write16(dev, B43_MMIO_POWERUP_DELAY,
			    dev->phy.type == B43_PHYTYPE_AC ? 1500 : 0xE74);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		b43_write16(dev, B43_MMIO_POWERUP_DELAY,
			    dev->dev->sdev->bus->chipco.fast_pwrup_delay);
		break;
#endif
	}
}

/* Turn the Analog on and initialize the PHY, then the antenna defaults. */
static int b43_phy_bringup(struct b43_wldev *dev)
{
	struct b43_phy *phy = &dev->phy;
	int err;

	phy->ops->switch_analog(dev, 1);
	err = b43_phy_init(dev);
	if (err)
		return err;

	/* Disable Interference Mitigation. */
	if (phy->ops->interf_mitigation)
		phy->ops->interf_mitigation(dev, B43_INTERFMODE_NONE);

	/* Select the antennae */
	if (phy->ops->set_rx_antenna)
		phy->ops->set_rx_antenna(dev, B43_ANTENNA_DEFAULT);
	b43_mgmtframe_txantenna(dev, B43_ANTENNA_DEFAULT);

	return 0;
}

static int b43_chip_init(struct b43_wldev *dev)
{
	struct b43_phy *phy = &dev->phy;
	int err;
	u32 macctl;
	u16 value16;

	/*
	 * Initialize the MAC control. The AC's stock drivers, 6.30 and 7.14,
	 * keep the ucode awake here and never set SHM_ENABLED, nor does
	 * brcmsmac (MCTL_SHM_EN); shared memory is reached through the
	 * object window all the same.
	 */
	macctl = B43_MACCTL_IHR_ENABLED;
	if (dev->phy.type == B43_PHYTYPE_AC)
		macctl |= B43_MACCTL_AWAKE;
	else
		macctl |= B43_MACCTL_SHM_ENABLED;
	if (dev->phy.gmode)
		macctl |= B43_MACCTL_GMODE;
	macctl |= B43_MACCTL_INFRA;
	b43_write32(dev, B43_MMIO_MACCTL, macctl);

	err = b43_upload_microcode(dev);
	if (err)
		goto out;	/* firmware is released later */

	b43info(dev->wl, "init: GPIO\n");
	err = b43_gpio_init(dev);
	if (err)
		goto out;	/* firmware is released later */

	b43info(dev->wl, "init: initvals\n");
	err = b43_upload_initvals(dev);
	if (err)
		goto err_gpio_clean;

	b43info(dev->wl, "init: band initvals\n");
	err = b43_bsinit(dev);
	if (err)
		goto err_gpio_clean;

	if (dev->dev->core_rev == 42) {
		b43info(dev->wl, "init: TX FIFOs\n");
		err = b43_txfifo_init_rev42(dev);
		if (err)
			goto err_gpio_clean;
		b43_mac_switch_freq(dev, 0);
	}

	/*
	 * The AC cores' stock driver sets the whole MAC up before it touches
	 * the PHY: the MAC init here, then the shared-memory cells, host
	 * flags, DMA rings and address tables of b43_wireless_core_init(),
	 * and the PHY init last. Its calibration during the PHY init also
	 * needs the MAC-PHY clock on. So the PHY is brought up at the end of
	 * b43_wireless_core_init() on the AC, and here on the other PHYs.
	 */
	if (phy->type == B43_PHYTYPE_AC) {
		b43info(dev->wl, "init: MAC\n");
		b43_chip_init_mac(dev);
	} else {
		err = b43_phy_bringup(dev);
		if (err)
			goto err_gpio_clean;
	}

	if (phy->type == B43_PHYTYPE_B) {
		value16 = b43_read16(dev, 0x005E);
		value16 |= 0x0004;
		b43_write16(dev, 0x005E, value16);
	}
	if (phy->type != B43_PHYTYPE_AC)
		b43_chip_init_mac(dev);

	err = 0;
	b43dbg(dev->wl, "Chip initialized\n");
out:
	return err;

err_gpio_clean:
	b43_gpio_cleanup(dev);
	return err;
}

static void b43_periodic_every60sec(struct b43_wldev *dev)
{
	const struct b43_phy_operations *ops = dev->phy.ops;

	if (ops->pwork_60sec)
		ops->pwork_60sec(dev);

	/* Force check the TX power emission now. */
	b43_phy_txpower_check(dev, B43_TXPWR_IGNORE_TIME);
}

static void b43_periodic_every30sec(struct b43_wldev *dev)
{
	/* Update device statistics. */
	b43_calculate_link_quality(dev);
}

static void b43_periodic_every15sec(struct b43_wldev *dev)
{
	struct b43_phy *phy = &dev->phy;
	u16 wdr;

	if (dev->fw.opensource) {
		/* Check if the firmware is still alive.
		 * It will reset the watchdog counter to 0 in its idle loop. */
		wdr = b43_shm_read16(dev, B43_SHM_SCRATCH, B43_WATCHDOG_REG);
		if (unlikely(wdr)) {
			b43err_restart(dev->wl,
				       "Firmware watchdog: The firmware died!\n");
			b43_controller_restart(dev, "Firmware watchdog");
			return;
		} else {
			b43_shm_write16(dev, B43_SHM_SCRATCH,
					B43_WATCHDOG_REG, 1);
		}
	}

	if (phy->ops->pwork_15sec)
		phy->ops->pwork_15sec(dev);

	atomic_set(&phy->txerr_cnt, B43_PHY_TX_BADNESS_LIMIT);
	wmb();

#if B43_DEBUG
	if (b43_debug(dev, B43_DBG_VERBOSESTATS)) {
		unsigned int i;

		b43dbg(dev->wl, "Stats: %7u IRQs/sec, %7u TX/sec, %7u RX/sec\n",
		       dev->irq_count / 15,
		       dev->tx_count / 15,
		       dev->rx_count / 15);
		dev->irq_count = 0;
		dev->tx_count = 0;
		dev->rx_count = 0;
		for (i = 0; i < ARRAY_SIZE(dev->irq_bit_count); i++) {
			if (dev->irq_bit_count[i]) {
				b43dbg(dev->wl, "Stats: %7u IRQ-%02u/sec (0x%08X)\n",
				       dev->irq_bit_count[i] / 15, i, (1 << i));
				dev->irq_bit_count[i] = 0;
			}
		}
	}
#endif
}

/*
 * With a PHY that has 1-second work the periodic work ticks every second, and
 * the 15, 30 and 60 second tasks run on every fifteenth tick.
 */
static unsigned int b43_periodic_ticks_per_15sec(struct b43_wldev *dev)
{
	return dev->phy.type == B43_PHYTYPE_AC && dev->phy.ops->pwork_1sec ?
	       15 : 1;
}

static void do_periodic_work(struct b43_wldev *dev)
{
	unsigned int ticks = b43_periodic_ticks_per_15sec(dev);
	unsigned int state;

	if (dev->phy.type == B43_PHYTYPE_AC && dev->phy.ops->pwork_1sec)
		dev->phy.ops->pwork_1sec(dev);
	if (dev->phy.type == B43_PHYTYPE_AC)
		b43_bcn_diag_tick(dev);

	if (dev->periodic_state % ticks)
		return;
	state = dev->periodic_state / ticks;
	if (state % 4 == 0)
		b43_periodic_every60sec(dev);
	if (state % 2 == 0)
		b43_periodic_every30sec(dev);
	b43_periodic_every15sec(dev);
}

/* Periodic work locking policy:
 * 	The whole periodic work handler is protected by
 * 	wl->mutex. If another lock is needed somewhere in the
 * 	pwork callchain, it's acquired in-place, where it's needed.
 */
static void b43_periodic_work_handler(struct work_struct *work)
{
	struct b43_wldev *dev = container_of(work, struct b43_wldev,
					     periodic_work.work);
	struct b43_wl *wl = dev->wl;
	unsigned long delay;

	mutex_lock(&wl->mutex);

	if (unlikely(b43_status(dev) != B43_STAT_STARTED))
		goto out;
	if (b43_debug(dev, B43_DBG_PWORK_STOP))
		goto out_requeue;

	do_periodic_work(dev);

	dev->periodic_state++;
out_requeue:
	if (b43_debug(dev, B43_DBG_PWORK_FAST))
		delay = msecs_to_jiffies(50);
	else if (b43_periodic_ticks_per_15sec(dev) > 1)
		delay = HZ;
	else
		delay = round_jiffies_relative(HZ * 15);
	ieee80211_queue_delayed_work(wl->hw, &dev->periodic_work, delay);
out:
	mutex_unlock(&wl->mutex);
}

/*
 * The detector is read while mac80211 asks for radar detection, which is the
 * availability check and, after it, the whole time the AP stays on the
 * channel.
 */
#define B43_RADAR_POLL_MS	150

static void b43_radar_work_handler(struct work_struct *work)
{
	struct b43_wldev *dev = container_of(work, struct b43_wldev,
					     radar_work.work);
	struct b43_wl *wl = dev->wl;

	mutex_lock(&wl->mutex);
	if (likely(b43_status(dev) == B43_STAT_STARTED) &&
	    wl->hw->conf.radar_enabled) {
		/*
		 * A pulse over the PHY's threshold is reported as a radar: the
		 * pulses are not matched against the radar patterns, so this
		 * errs towards leaving the channel.
		 */
		if (dev->phy.ops->radar_poll(dev))
			ieee80211_radar_detected(wl->hw);
		ieee80211_queue_delayed_work(wl->hw, &dev->radar_work,
					     msecs_to_jiffies(B43_RADAR_POLL_MS));
	}
	mutex_unlock(&wl->mutex);
}

static void b43_periodic_tasks_setup(struct b43_wldev *dev)
{
	struct delayed_work *work = &dev->periodic_work;

	dev->periodic_state = 0;
	INIT_DELAYED_WORK(work, b43_periodic_work_handler);
	/* A 1-second tick starts one second after the core, as the OEM's does. */
	ieee80211_queue_delayed_work(dev->wl->hw, work,
				     b43_periodic_ticks_per_15sec(dev) > 1 ? HZ : 0);

	INIT_DELAYED_WORK(&dev->radar_work, b43_radar_work_handler);
}

/* Check if communication with the device works correctly. */
static int b43_validate_chipaccess(struct b43_wldev *dev)
{
	u32 v, expected, backup0, backup4;

	backup0 = b43_shm_read32(dev, B43_SHM_SHARED, 0);
	backup4 = b43_shm_read32(dev, B43_SHM_SHARED, 4);

	/* Check for read/write and endianness problems. */
	b43_shm_write32(dev, B43_SHM_SHARED, 0, 0x55AAAA55);
	if (b43_shm_read32(dev, B43_SHM_SHARED, 0) != 0x55AAAA55)
		goto error;
	b43_shm_write32(dev, B43_SHM_SHARED, 0, 0xAA5555AA);
	if (b43_shm_read32(dev, B43_SHM_SHARED, 0) != 0xAA5555AA)
		goto error;

	/* Check if unaligned 32bit SHM_SHARED access works properly.
	 * However, don't bail out on failure, because it's noncritical. */
	b43_shm_write16(dev, B43_SHM_SHARED, 0, 0x1122);
	b43_shm_write16(dev, B43_SHM_SHARED, 2, 0x3344);
	b43_shm_write16(dev, B43_SHM_SHARED, 4, 0x5566);
	b43_shm_write16(dev, B43_SHM_SHARED, 6, 0x7788);
	if (b43_shm_read32(dev, B43_SHM_SHARED, 2) != 0x55663344)
		b43warn(dev->wl, "Unaligned 32bit SHM read access is broken\n");
	b43_shm_write32(dev, B43_SHM_SHARED, 2, 0xAABBCCDD);
	if (b43_shm_read16(dev, B43_SHM_SHARED, 0) != 0x1122 ||
	    b43_shm_read16(dev, B43_SHM_SHARED, 2) != 0xCCDD ||
	    b43_shm_read16(dev, B43_SHM_SHARED, 4) != 0xAABB ||
	    b43_shm_read16(dev, B43_SHM_SHARED, 6) != 0x7788)
		b43warn(dev->wl, "Unaligned 32bit SHM write access is broken\n");

	b43_shm_write32(dev, B43_SHM_SHARED, 0, backup0);
	b43_shm_write32(dev, B43_SHM_SHARED, 4, backup4);

	if ((dev->dev->core_rev >= 3) && (dev->dev->core_rev <= 10)) {
		/* The 32bit register shadows the two 16bit registers
		 * with update sideeffects. Validate this. */
		b43_write16(dev, B43_MMIO_TSF_CFP_START, 0xAAAA);
		b43_write32(dev, B43_MMIO_TSF_CFP_START, 0xCCCCBBBB);
		if (b43_read16(dev, B43_MMIO_TSF_CFP_START_LOW) != 0xBBBB)
			goto error;
		if (b43_read16(dev, B43_MMIO_TSF_CFP_START_HIGH) != 0xCCCC)
			goto error;
	}
	b43_write32(dev, B43_MMIO_TSF_CFP_START, 0);

	v = b43_read32(dev, B43_MMIO_MACCTL);
	v |= B43_MACCTL_GMODE;
	expected = B43_MACCTL_GMODE | B43_MACCTL_IHR_ENABLED;
	if (b43_is_ac_core(dev))
		expected |= B43_MACCTL_AWAKE;
	if (v != expected)
		goto error;

	return 0;
error:
	b43err(dev->wl, "Failed to validate the chipaccess\n");
	return -ENODEV;
}

static void b43_security_init(struct b43_wldev *dev)
{
	dev->ktp = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_KTP);
	/* KTP is a word address, but we address SHM bytewise.
	 * So multiply by two.
	 */
	dev->ktp *= 2;
	/* Number of RCMTA address slots; the AC ucode is not told. */
	if (dev->fw.hdr_format != B43_FW_HDR_AC)
		b43_write16(dev, B43_MMIO_RCMTA_COUNT, B43_NR_PAIRWISE_KEYS);
	/* Clear the key memory. */
	b43_clear_keys(dev);
}

#ifdef CONFIG_B43_HWRNG
static int b43_rng_read(struct hwrng *rng, u32 *data)
{
	struct b43_wl *wl = (struct b43_wl *)rng->priv;
	struct b43_wldev *dev;
	int count = -ENODEV;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (likely(dev && b43_status(dev) >= B43_STAT_INITIALIZED)) {
		*data = b43_read16(dev, B43_MMIO_RNG);
		count = sizeof(u16);
	}
	mutex_unlock(&wl->mutex);

	return count;
}
#endif /* CONFIG_B43_HWRNG */

static void b43_rng_exit(struct b43_wl *wl)
{
#ifdef CONFIG_B43_HWRNG
	if (wl->rng_initialized)
		hwrng_unregister(&wl->rng);
#endif /* CONFIG_B43_HWRNG */
}

static int b43_rng_init(struct b43_wl *wl)
{
	int err = 0;

#ifdef CONFIG_B43_HWRNG
	snprintf(wl->rng_name, ARRAY_SIZE(wl->rng_name),
		 "%s_%s", KBUILD_MODNAME, wiphy_name(wl->hw->wiphy));
	wl->rng.name = wl->rng_name;
	wl->rng.data_read = b43_rng_read;
	wl->rng.priv = (unsigned long)wl;
	wl->rng_initialized = true;
	err = hwrng_register(&wl->rng);
	if (err) {
		wl->rng_initialized = false;
		b43err(wl, "Failed to register the random "
		       "number generator (%d)\n", err);
	}
#endif /* CONFIG_B43_HWRNG */

	return err;
}

static void b43_tx_work(struct work_struct *work)
{
	struct b43_wl *wl = container_of(work, struct b43_wl, tx_work);
	struct b43_wldev *dev;
	struct sk_buff *skb;
	int queue_num;
	int err = 0;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (unlikely(!dev || b43_status(dev) < B43_STAT_STARTED)) {
		mutex_unlock(&wl->mutex);
		return;
	}

	for (queue_num = 0; queue_num < B43_QOS_QUEUE_NUM; queue_num++) {
		while (skb_queue_len(&wl->tx_queue[queue_num])) {
			skb = skb_dequeue(&wl->tx_queue[queue_num]);
			b43_bcn_diag_tx(dev, skb);
			if (b43_using_pio_transfers(dev))
				err = b43_pio_tx(dev, skb);
			else
				err = b43_dma_tx(dev, skb);
			if (err == -ENOSPC) {
				wl->tx_queue_stopped[queue_num] = true;
				ieee80211_stop_queue(wl->hw, queue_num);
				skb_queue_head(&wl->tx_queue[queue_num], skb);
				break;
			}
			if (unlikely(err))
				ieee80211_free_txskb(wl->hw, skb);
			err = 0;
		}

		if (!err)
			wl->tx_queue_stopped[queue_num] = false;
	}

#if B43_DEBUG
	dev->tx_count++;
#endif
	mutex_unlock(&wl->mutex);
}

static bool b43_tx_rate_is_mcs(const struct ieee80211_tx_rate *rate)
{
	return rate->idx >= 0 &&
	       (rate->flags & (IEEE80211_TX_RC_MCS | IEEE80211_TX_RC_VHT_MCS));
}

/*
 * Whether the TX header can carry the frame's rates. On the AC the first
 * rate block alone, legacy or a VHT MCS 0-9 at 20, 40 or 80 MHz on a stream
 * count the band's TX map has; on the other PHYs a legacy rate and its
 * fallback. Rate control picks nothing else, see b43_ac_set_ht_vht_cap(),
 * but an injected frame can ask for any rate, and the header would read an
 * MCS as an index into the legacy rate table.
 */
static bool b43_tx_rates_ok(struct ieee80211_hw *hw,
			    const struct ieee80211_tx_info *info)
{
	const struct ieee80211_supported_band *band = hw->wiphy->bands[info->band];
	const struct ieee80211_tx_rate *r = info->control.rates;
	bool vht_band = band && band->vht_cap.vht_supported;

	if (r[0].idx >= 0 && (r[0].flags & IEEE80211_TX_RC_VHT_MCS)) {
		u8 nss = ieee80211_rate_get_vht_nss(&r[0]);
		u16 map;

		if (!vht_band)
			return false;
		map = le16_to_cpu(band->vht_cap.vht_mcs.tx_mcs_map);
		return ((map >> (2 * (nss - 1))) & 3) !=
			IEEE80211_VHT_MCS_NOT_SUPPORTED &&
		       ieee80211_rate_get_vht_mcs(&r[0]) <= 9 &&
		       !(r[0].flags & IEEE80211_TX_RC_160_MHZ_WIDTH);
	}
	return !b43_tx_rate_is_mcs(&r[0]) &&
	       (vht_band || !b43_tx_rate_is_mcs(&r[1]));
}

static void b43_op_tx(struct ieee80211_hw *hw,
		      struct ieee80211_tx_control *control,
		      struct sk_buff *skb)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);

	if (unlikely(skb->len < 2 + 2 + 6)) {
		/* Too short, this can't be a valid frame. */
		ieee80211_free_txskb(hw, skb);
		return;
	}
	B43_WARN_ON(skb_shinfo(skb)->nr_frags);
	if (unlikely(!b43_tx_rates_ok(hw, IEEE80211_SKB_CB(skb)))) {
		ieee80211_free_txskb(hw, skb);
		return;
	}

	skb_queue_tail(&wl->tx_queue[skb->queue_mapping], skb);
	if (!wl->tx_queue_stopped[skb->queue_mapping]) {
		ieee80211_queue_work(wl->hw, &wl->tx_work);
	} else {
		ieee80211_stop_queue(wl->hw, skb->queue_mapping);
	}
}

static void b43_qos_params_upload(struct b43_wldev *dev,
				  const struct ieee80211_tx_queue_params *p,
				  u16 shm_offset)
{
	u16 params[B43_NR_QOSPARAMS];
	int bslots, tmp;
	unsigned int i;

	if (!dev->qos_enabled)
		return;

	bslots = b43_read16(dev, B43_MMIO_RNG) & p->cw_min;

	memset(&params, 0, sizeof(params));

	params[B43_QOSPARAM_TXOP] = p->txop * 32;
	params[B43_QOSPARAM_CWMIN] = p->cw_min;
	params[B43_QOSPARAM_CWMAX] = p->cw_max;
	params[B43_QOSPARAM_CWCUR] = p->cw_min;
	params[B43_QOSPARAM_AIFS] = p->aifs;
	params[B43_QOSPARAM_BSLOTS] = bslots;
	params[B43_QOSPARAM_REGGAP] = bslots + p->aifs;

	for (i = 0; i < ARRAY_SIZE(params); i++) {
		if (i == B43_QOSPARAM_STATUS) {
			tmp = b43_shm_read16(dev, B43_SHM_SHARED,
					     shm_offset + (i * 2));
			/* Mark the parameters as updated. */
			tmp |= 0x100;
			b43_shm_write16(dev, B43_SHM_SHARED,
					shm_offset + (i * 2),
					tmp);
		} else {
			b43_shm_write16(dev, B43_SHM_SHARED,
					shm_offset + (i * 2),
					params[i]);
		}
	}
}

/* Mapping of mac80211 queue numbers to b43 QoS SHM offsets. */
static const u16 b43_qos_shm_offsets[] = {
	/* [mac80211-queue-nr] = SHM_OFFSET, */
	[0] = B43_QOS_VOICE,
	[1] = B43_QOS_VIDEO,
	[2] = B43_QOS_BESTEFFORT,
	[3] = B43_QOS_BACKGROUND,
};

/* Update all QOS parameters in hardware. */
static void b43_qos_upload_all(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;
	struct b43_qos_params *params;
	unsigned int i;

	if (!dev->qos_enabled)
		return;

	BUILD_BUG_ON(ARRAY_SIZE(b43_qos_shm_offsets) !=
		     ARRAY_SIZE(wl->qos_params));

	b43_mac_suspend(dev);
	for (i = 0; i < ARRAY_SIZE(wl->qos_params); i++) {
		params = &(wl->qos_params[i]);
		b43_qos_params_upload(dev, &(params->p),
				      b43_qos_shm_offsets[i]);
	}
	b43_mac_enable(dev);
}

static void b43_qos_clear(struct b43_wl *wl)
{
	struct b43_qos_params *params;
	unsigned int i;

	/* Initialize QoS parameters to sane defaults. */

	BUILD_BUG_ON(ARRAY_SIZE(b43_qos_shm_offsets) !=
		     ARRAY_SIZE(wl->qos_params));

	for (i = 0; i < ARRAY_SIZE(wl->qos_params); i++) {
		params = &(wl->qos_params[i]);

		switch (b43_qos_shm_offsets[i]) {
		case B43_QOS_VOICE:
			params->p.txop = 0;
			params->p.aifs = 2;
			params->p.cw_min = 0x0001;
			params->p.cw_max = 0x0001;
			break;
		case B43_QOS_VIDEO:
			params->p.txop = 0;
			params->p.aifs = 2;
			params->p.cw_min = 0x0001;
			params->p.cw_max = 0x0001;
			break;
		case B43_QOS_BESTEFFORT:
			params->p.txop = 0;
			params->p.aifs = 3;
			params->p.cw_min = 0x0001;
			params->p.cw_max = 0x03FF;
			break;
		case B43_QOS_BACKGROUND:
			params->p.txop = 0;
			params->p.aifs = 7;
			params->p.cw_min = 0x0001;
			params->p.cw_max = 0x03FF;
			break;
		default:
			B43_WARN_ON(1);
		}
	}
}

/* Initialize the core's QOS capabilities */
static void b43_qos_init(struct b43_wldev *dev)
{
	if (!dev->qos_enabled) {
		/* Disable QOS support. */
		b43_hf_write(dev, b43_hf_read(dev) & ~B43_HF_EDCF);
		b43_write16(dev, B43_MMIO_IFSCTL,
			    b43_read16(dev, B43_MMIO_IFSCTL)
			    & ~B43_MMIO_IFSCTL_USE_EDCF);
		b43dbg(dev->wl, "QoS disabled\n");
		return;
	}

	/* Upload the current QOS parameters. */
	b43_qos_upload_all(dev);

	/* Enable QOS support. */
	b43_hf_write(dev, b43_hf_read(dev) | B43_HF_EDCF);
	b43_write16(dev, B43_MMIO_IFSCTL,
		    b43_read16(dev, B43_MMIO_IFSCTL)
		    | B43_MMIO_IFSCTL_USE_EDCF);
	b43dbg(dev->wl, "QoS enabled\n");
}

static int b43_op_conf_tx(struct ieee80211_hw *hw,
			  struct ieee80211_vif *vif,
			  unsigned int link_id, u16 _queue,
			  const struct ieee80211_tx_queue_params *params)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;
	unsigned int queue = (unsigned int)_queue;
	int err = -ENODEV;

	if (queue >= ARRAY_SIZE(wl->qos_params)) {
		/* Queue not available or don't support setting
		 * params on this queue. Return success to not
		 * confuse mac80211. */
		return 0;
	}
	BUILD_BUG_ON(ARRAY_SIZE(b43_qos_shm_offsets) !=
		     ARRAY_SIZE(wl->qos_params));

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (unlikely(!dev || (b43_status(dev) < B43_STAT_INITIALIZED)))
		goto out_unlock;

	memcpy(&(wl->qos_params[queue].p), params, sizeof(*params));
	b43_mac_suspend(dev);
	b43_qos_params_upload(dev, &(wl->qos_params[queue].p),
			      b43_qos_shm_offsets[queue]);
	b43_mac_enable(dev);
	err = 0;

out_unlock:
	mutex_unlock(&wl->mutex);

	return err;
}

static int b43_op_get_stats(struct ieee80211_hw *hw,
			    struct ieee80211_low_level_stats *stats)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);

	mutex_lock(&wl->mutex);
	memcpy(stats, &wl->ieee_stats, sizeof(*stats));
	mutex_unlock(&wl->mutex);

	return 0;
}

static u64 b43_op_get_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;
	u64 tsf;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;

	if (dev && (b43_status(dev) >= B43_STAT_INITIALIZED))
		b43_tsf_read(dev, &tsf);
	else
		tsf = 0;

	mutex_unlock(&wl->mutex);

	return tsf;
}

static void b43_op_set_tsf(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif, u64 tsf)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;

	if (dev && (b43_status(dev) >= B43_STAT_INITIALIZED))
		b43_tsf_write(dev, tsf);

	mutex_unlock(&wl->mutex);
}

static const char *band_to_string(enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_5GHZ:
		return "5";
	case NL80211_BAND_2GHZ:
		return "2.4";
	default:
		break;
	}
	B43_WARN_ON(1);
	return "";
}

/* Expects wl->mutex locked */
static int b43_switch_band(struct b43_wldev *dev,
			   struct ieee80211_channel *chan)
{
	struct b43_phy *phy = &dev->phy;
	bool gmode;
	u32 tmp;

	switch (chan->band) {
	case NL80211_BAND_5GHZ:
		gmode = false;
		break;
	case NL80211_BAND_2GHZ:
		gmode = true;
		break;
	default:
		B43_WARN_ON(1);
		return -EINVAL;
	}

	if (!((gmode && phy->supports_2ghz) ||
	      (!gmode && phy->supports_5ghz))) {
		b43err(dev->wl, "This device doesn't support %s-GHz band\n",
		       band_to_string(chan->band));
		return -ENODEV;
	}

	if (!!phy->gmode == !!gmode) {
		/* This device is already running. */
		return 0;
	}

	b43dbg(dev->wl, "Switching to %s GHz band\n",
	       band_to_string(chan->band));

	/* Some new devices don't need disabling radio for band switching */
	if (!(phy->type == B43_PHYTYPE_N && phy->rev >= 3))
		b43_software_rfkill(dev, true);

	phy->gmode = gmode;
	b43_phy_put_into_reset(dev);
	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		tmp = bcma_aread32(dev->dev->bdev, BCMA_IOCTL);
		if (gmode)
			tmp |= B43_BCMA_IOCTL_GMODE;
		else
			tmp &= ~B43_BCMA_IOCTL_GMODE;
		bcma_awrite32(dev->dev->bdev, BCMA_IOCTL, tmp);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		tmp = ssb_read32(dev->dev->sdev, SSB_TMSLOW);
		if (gmode)
			tmp |= B43_TMSLOW_GMODE;
		else
			tmp &= ~B43_TMSLOW_GMODE;
		ssb_write32(dev->dev->sdev, SSB_TMSLOW, tmp);
		break;
#endif
	}
	b43_phy_take_out_of_reset(dev);

	b43_bsinit(dev);

	b43_phy_init(dev);

	return 0;
}

static void b43_set_beacon_listen_interval(struct b43_wldev *dev, u16 interval)
{
	interval = min_t(u16, interval, (u16)0xFF);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_BCN_LI, interval);
}

/* Write the short and long frame retry limit values. */
static void b43_set_retry_limits(struct b43_wldev *dev,
				 unsigned int short_retry,
				 unsigned int long_retry)
{
	/* The retry limit is a 4-bit counter. Enforce this to avoid overflowing
	 * the chip-internal counter. */
	short_retry = min(short_retry, (unsigned int)0xF);
	long_retry = min(long_retry, (unsigned int)0xF);

	b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_SRLIMIT,
			short_retry);
	b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_LRLIMIT,
			long_retry);
}

static bool b43_ac_beaconing(struct b43_wl *wl)
{
	return wl->vif && wl->vif->bss_conf.enable_beacon;
}

/*
 * mac80211 runs the channel availability check. A driver without channel
 * contexts learns about it from conf->radar_enabled on the config call that
 * tunes the channel, and learns that it is over when beaconing is enabled:
 * after the check mac80211 releases the channel, and start_ap tunes it again,
 * radar detection still on, before it enables the beacon. So until then the
 * channel may not be transmitted on, whichever of the two tunes this is.
 */
static bool b43_ac_cac_needed(struct b43_wl *wl,
			      const struct ieee80211_conf *conf)
{
	return conf->radar_enabled &&
	       (conf->chandef.chan->flags & IEEE80211_CHAN_RADAR) &&
	       !b43_ac_beaconing(wl);
}

/*
 * Whether this config call needs the radio retuned. mac80211 passes
 * IEEE80211_CONF_CHANGE_CHANNEL also when only radar_enabled changes, and
 * around a channel availability check it releases the channel -- 20 MHz
 * without HT on the same primary -- and tunes it again for start_ap. On the AC
 * PHY a retune is a full channel setup with its calibrations, and none of
 * these calls needs one: the radio stays where it is, and nothing transmits
 * while nothing beacons.
 */
static bool b43_ac_retune_needed(struct b43_wldev *dev,
				 const struct cfg80211_chan_def *def)
{
	const struct cfg80211_chan_def *cur = &dev->tuned_chandef;

	if (!cur->chan)
		return true;
	if (cfg80211_chandef_identical(cur, def))
		return false;
	return !(def->chan == cur->chan &&
		 def->width == NL80211_CHAN_WIDTH_20_NOHT &&
		 !b43_ac_beaconing(dev->wl));
}

/*
 * A channel change on a running AC interface. The stock driver changes
 * channel only through a down and an up; this is that cycle for the PHY.
 * The up's core resets, ucode load and PLL setup are left out: the PLL
 * words are the same on every channel of every capture. The PHY's state is
 * kept: op_prepare_structs() goes with the core reset of
 * b43_wireless_core_init(), which clears the hardware state the PHY tracks,
 * and here nothing is reset. phy->do_full_init stays false, so
 * b43_phy_init() takes the hot path and writes the new chanspec first.
 *
 * A failed b43_phy_init() leaves the radio off and asks for a full init,
 * which a running core cannot take; the controller restart gives it one.
 */
static int b43_ac_phy_recycle(struct b43_wldev *dev)
{
	int err;

	b43_software_rfkill(dev, true);
	err = b43_phy_init(dev);
	if (err)
		b43_controller_restart(dev, "PHY cycle failed");
	return err;
}

/*
 * The channel availability check mutes the MAC, so that the BSS neither
 * transmits nor answers on a channel it may not use yet. Three things, in the
 * order the stock driver writes them at the bus:
 *
 *  - the TX FIFOs are suspended, 1, 3, 0, 2, 4, and resumed in the same order;
 *    on the way in MACCONTROL is read before each;
 *  - the station row of the address match table is cleared, address and
 *    flags, and written again with both;
 *  - MACCONTROL does not carry B43_MACCTL_AP. b43_adjust_opmode() keeps
 *    it clear while the check is pending, the opening sets it.
 */
static void b43_ac_cac_match_gate(struct b43_wldev *dev, bool open)
{
	struct b43_dmaring *fifo[] = {
		dev->dma.tx_ring_AC_BE, dev->dma.tx_ring_AC_VO,
		dev->dma.tx_ring_AC_BK, dev->dma.tx_ring_AC_VI,
		dev->dma.tx_ring_mcast,
	};
	bool ap = b43_is_mode(dev->wl, NL80211_IFTYPE_AP);
	bool read = false;
	u32 macctl = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fifo); i++) {
		if (b43_using_pio_transfers(dev) || !fifo[i])
			break;
		if (open) {
			fifo[i]->ops->tx_resume(fifo[i]);
			continue;
		}
		macctl = b43_read32(dev, B43_MMIO_MACCTL);
		read = true;
		fifo[i]->ops->tx_suspend(fifo[i]);
	}
	if (b43_amt_wide(dev))
		b43_amt_write(dev, B43_AMT_SELF,
			      open ? dev->wl->mac_addr : NULL,
			      open ? B43_AMT_F_SELF : 0);
	if (open) {
		if (ap)
			b43_maskset32(dev, B43_MMIO_MACCTL, ~0, B43_MACCTL_AP);
		if (dev->phy.ops->cac_done)
			dev->phy.ops->cac_done(dev);
		return;
	}
	/* The interface may have come up before the check started. */
	if (!read)
		macctl = b43_read32(dev, B43_MMIO_MACCTL);
	if (macctl & B43_MACCTL_AP)
		b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_AP, 0);
}

static int b43_op_config(struct ieee80211_hw *hw, u32 changed)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev = wl->current_dev;
	struct b43_phy *phy = &dev->phy;
	struct ieee80211_conf *conf = &hw->conf;
	bool ac = phy->type == B43_PHYTYPE_AC;
	bool cac_was = dev->cac_pending;
	bool retune = true;
	bool switched = false;
	int antenna;
	int err = 0;

	mutex_lock(&wl->mutex);
	b43_mac_suspend(dev);

	if (changed & IEEE80211_CONF_CHANGE_LISTEN_INTERVAL)
		b43_set_beacon_listen_interval(dev, conf->listen_interval);

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		phy->chandef = &conf->chandef;
		phy->channel = conf->chandef.chan->hw_value;

		/* Switch the band (if necessary). */
		err = b43_switch_band(dev, conf->chandef.chan);
		if (err)
			goto out_mac_enable;

		/*
		 * The check state is settled before the switch, which reads
		 * it. A retune starts from the new channel's own state; on the
		 * same channel only beaconing ends the check, in
		 * b43_op_bss_info_changed(), and the release in between leaves
		 * it as it is.
		 */
		if (ac) {
			bool cac = b43_ac_cac_needed(wl, conf);

			retune = b43_ac_retune_needed(dev, &conf->chandef);
			if (cac || retune)
				dev->cac_pending = cac;
		}

		if (retune) {
			/* Switch to the requested channel.
			 * The firmware takes care of races with the TX handler.
			 */
			if (ac && dev->tuned_chandef.chan) {
				err = b43_ac_phy_recycle(dev);
				if (err)
					goto out_mac_enable;
			} else {
				b43_switch_channel(dev, phy->channel);
			}
			switched = true;
			if (ac)
				dev->tuned_chandef = conf->chandef;
		}

		if (ac && dev->cac_pending != cac_was)
			b43_ac_cac_match_gate(dev, !dev->cac_pending);
		if (ac && conf->radar_enabled && phy->ops->radar_poll)
			ieee80211_queue_delayed_work(hw, &dev->radar_work,
				msecs_to_jiffies(B43_RADAR_POLL_MS));
	}

	if (changed & IEEE80211_CONF_CHANGE_RETRY_LIMITS)
		b43_set_retry_limits(dev, conf->short_frame_max_tx_count,
					  conf->long_frame_max_tx_count);
	changed &= ~IEEE80211_CONF_CHANGE_RETRY_LIMITS;
	if (!changed)
		goto out_mac_enable;

	dev->wl->radiotap_enabled = !!(conf->flags & IEEE80211_CONF_MONITOR);

	/* Adjust the desired TX power level. */
	if (conf->power_level != 0) {
		if (conf->power_level != phy->desired_txpower) {
			phy->desired_txpower = conf->power_level;
			b43_phy_txpower_check(dev, B43_TXPWR_IGNORE_TIME |
						   B43_TXPWR_IGNORE_TSSI);
		}
	}

	/* Antennas for RX and management frame TX. */
	antenna = B43_ANTENNA_DEFAULT;
	b43_mgmtframe_txantenna(dev, antenna);
	antenna = B43_ANTENNA_DEFAULT;
	if (phy->ops->set_rx_antenna)
		phy->ops->set_rx_antenna(dev, antenna);

	if (wl->radio_enabled != phy->radio_on) {
		if (wl->radio_enabled) {
			b43_software_rfkill(dev, false);
			b43info(dev->wl, "Radio turned on by software\n");
			if (!dev->radio_hw_enable) {
				b43info(dev->wl, "The hardware RF-kill button "
					"still turns the radio physically off. "
					"Press the button to turn it on.\n");
			}
		} else {
			b43_software_rfkill(dev, true);
			b43info(dev->wl, "Radio turned off by software\n");
		}
	}

out_mac_enable:
	if (switched && phy->ops->channel_calibrate)
		phy->ops->channel_calibrate(dev);
	else
		b43_mac_enable(dev);
	mutex_unlock(&wl->mutex);

	return err;
}

static void b43_update_basic_rates(struct b43_wldev *dev, u32 brates)
{
	struct ieee80211_supported_band *sband =
		dev->wl->hw->wiphy->bands[b43_current_band(dev->wl)];
	const struct ieee80211_rate *rate;
	int i;
	u16 basic, direct, offset, basic_offset, rateptr;

	for (i = 0; i < sband->n_bitrates; i++) {
		rate = &sband->bitrates[i];

		if (b43_is_cck_rate(rate->hw_value)) {
			direct = B43_SHM_SH_CCKDIRECT;
			basic = B43_SHM_SH_CCKBASIC;
			offset = b43_plcp_get_ratecode_cck(rate->hw_value);
			offset &= 0xF;
		} else {
			direct = B43_SHM_SH_OFDMDIRECT;
			basic = B43_SHM_SH_OFDMBASIC;
			offset = b43_plcp_get_ratecode_ofdm(rate->hw_value);
			offset &= 0xF;
		}

		rate = ieee80211_get_response_rate(sband, brates, rate->bitrate);

		if (b43_is_cck_rate(rate->hw_value)) {
			basic_offset = b43_plcp_get_ratecode_cck(rate->hw_value);
			basic_offset &= 0xF;
		} else {
			basic_offset = b43_plcp_get_ratecode_ofdm(rate->hw_value);
			basic_offset &= 0xF;
		}

		/*
		 * Get the pointer that we need to point to
		 * from the direct map
		 */
		rateptr = b43_shm_read16(dev, B43_SHM_SHARED,
					 direct + 2 * basic_offset);
		/* and write it to the basic map */
		b43_shm_write16(dev, B43_SHM_SHARED, basic + 2 * offset,
				rateptr);
	}
}

static void b43_op_bss_info_changed(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *conf,
				    u64 changed)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;

	mutex_lock(&wl->mutex);

	dev = wl->current_dev;
	if (!dev || b43_status(dev) < B43_STAT_STARTED)
		goto out_unlock_mutex;

	B43_WARN_ON(wl->vif != vif);

	if (changed & BSS_CHANGED_BSSID) {
		if (conf->bssid)
			memcpy(wl->bssid, conf->bssid, ETH_ALEN);
		else
			eth_zero_addr(wl->bssid);
	}

	if (b43_status(dev) >= B43_STAT_INITIALIZED) {
		if (changed & BSS_CHANGED_BEACON &&
		    (b43_is_mode(wl, NL80211_IFTYPE_AP) ||
		     b43_is_mode(wl, NL80211_IFTYPE_MESH_POINT) ||
		     b43_is_mode(wl, NL80211_IFTYPE_ADHOC)))
			b43_update_templates(wl);

		if (changed & BSS_CHANGED_BSSID)
			b43_write_mac_bssid_templates(dev);

		/* Beaconing ends the channel availability check. */
		if (dev->phy.type == B43_PHYTYPE_AC &&
		    changed & BSS_CHANGED_BEACON_ENABLED &&
		    conf->enable_beacon && dev->cac_pending) {
			dev->cac_pending = false;
			b43_ac_cac_match_gate(dev, true);
		}
	}

	b43_mac_suspend(dev);

	/* Update templates for AP/mesh mode. */
	if (changed & BSS_CHANGED_BEACON_INT &&
	    (b43_is_mode(wl, NL80211_IFTYPE_AP) ||
	     b43_is_mode(wl, NL80211_IFTYPE_MESH_POINT) ||
	     b43_is_mode(wl, NL80211_IFTYPE_ADHOC)) &&
	    conf->beacon_int)
		b43_set_beacon_int(dev, conf->beacon_int);

	if (changed & BSS_CHANGED_BASIC_RATES)
		b43_update_basic_rates(dev, conf->basic_rates);

	if (changed & BSS_CHANGED_ERP_SLOT) {
		if (conf->use_short_slot)
			b43_short_slot_timing_enable(dev);
		else
			b43_short_slot_timing_disable(dev);
	}

	b43_mac_enable(dev);
out_unlock_mutex:
	mutex_unlock(&wl->mutex);
}

static int b43_op_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			  struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			  struct ieee80211_key_conf *key)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;
	u8 algorithm;
	u8 index;
	int err;
	static const u8 bcast_addr[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if (modparam_nohwcrypt)
		return -ENOSPC; /* User disabled HW-crypto */

	if ((vif->type == NL80211_IFTYPE_ADHOC ||
	     vif->type == NL80211_IFTYPE_MESH_POINT) &&
	    (key->cipher == WLAN_CIPHER_SUITE_TKIP ||
	     key->cipher == WLAN_CIPHER_SUITE_CCMP) &&
	    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE)) {
		/*
		 * For now, disable hw crypto for the RSN IBSS group keys. This
		 * could be optimized in the future, but until that gets
		 * implemented, use of software crypto for group addressed
		 * frames is a acceptable to allow RSN IBSS to be used.
		 */
		return -EOPNOTSUPP;
	}

	mutex_lock(&wl->mutex);

	dev = wl->current_dev;
	err = -ENODEV;
	if (!dev || b43_status(dev) < B43_STAT_INITIALIZED)
		goto out_unlock;

	if (dev->fw.pcm_request_failed || !dev->hwcrypto_enabled) {
		/* We don't have firmware for the crypto engine.
		 * Must use software-crypto. */
		err = -EOPNOTSUPP;
		goto out_unlock;
	}

	err = -EINVAL;
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		algorithm = B43_SEC_ALGO_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		algorithm = B43_SEC_ALGO_WEP104;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		algorithm = B43_SEC_ALGO_TKIP;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		algorithm = B43_SEC_ALGO_AES;
		break;
	default:
		B43_WARN_ON(1);
		goto out_unlock;
	}
	index = (u8) (key->keyidx);
	if (index > 3)
		goto out_unlock;

	switch (cmd) {
	case SET_KEY:
		if (algorithm == B43_SEC_ALGO_TKIP &&
		    (!(key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ||
		    !modparam_hwtkip)) {
			/* We support only pairwise key */
			err = -EOPNOTSUPP;
			goto out_unlock;
		}

		if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) {
			if (WARN_ON(!sta)) {
				err = -EOPNOTSUPP;
				goto out_unlock;
			}
			/* Pairwise key with an assigned MAC address. */
			err = b43_key_write(dev, -1, algorithm,
					    key->key, key->keylen,
					    sta->addr, key);
		} else {
			/* Group key */
			err = b43_key_write(dev, index, algorithm,
					    key->key, key->keylen, NULL, key);
		}
		if (err)
			goto out_unlock;

		if (algorithm == B43_SEC_ALGO_WEP40 ||
		    algorithm == B43_SEC_ALGO_WEP104) {
			b43_hf_write(dev, b43_hf_read(dev) | B43_HF_USEDEFKEYS);
		} else {
			b43_hf_write(dev,
				     b43_hf_read(dev) & ~B43_HF_USEDEFKEYS);
		}
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
		if (algorithm == B43_SEC_ALGO_TKIP)
			key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
		break;
	case DISABLE_KEY: {
		err = b43_key_clear(dev, key->hw_key_idx);
		if (err)
			goto out_unlock;
		break;
	}
	default:
		B43_WARN_ON(1);
	}

out_unlock:
	if (!err) {
		b43dbg(wl, "%s hardware based encryption for keyidx: %d, "
		       "mac: %pM\n",
		       cmd == SET_KEY ? "Using" : "Disabling", key->keyidx,
		       sta ? sta->addr : bcast_addr);
		b43_dump_keymemory(dev);
	}
	mutex_unlock(&wl->mutex);

	return err;
}

static void b43_op_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed, unsigned int *fflags,
				    u64 multicast)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (!dev) {
		*fflags = 0;
		goto out_unlock;
	}

	*fflags &= FIF_ALLMULTI |
		  FIF_FCSFAIL |
		  FIF_PLCPFAIL |
		  FIF_CONTROL |
		  FIF_OTHER_BSS |
		  FIF_BCN_PRBRESP_PROMISC;

	changed &= FIF_ALLMULTI |
		   FIF_FCSFAIL |
		   FIF_PLCPFAIL |
		   FIF_CONTROL |
		   FIF_OTHER_BSS |
		   FIF_BCN_PRBRESP_PROMISC;

	wl->filter_flags = *fflags;

	if (changed && b43_status(dev) >= B43_STAT_INITIALIZED)
		b43_adjust_opmode(dev);

out_unlock:
	mutex_unlock(&wl->mutex);
}

/* Locking: wl->mutex
 * Returns the current dev. This might be different from the passed in dev,
 * because the core might be gone away while we unlocked the mutex. */
static struct b43_wldev * b43_wireless_core_stop(struct b43_wldev *dev)
{
	struct b43_wl *wl;
	struct b43_wldev *orig_dev;
	u32 mask;
	int queue_num;

	if (!dev)
		return NULL;
	wl = dev->wl;
redo:
	if (!dev || b43_status(dev) < B43_STAT_STARTED)
		return dev;

	/* Cancel work. Unlock to avoid deadlocks. */
	mutex_unlock(&wl->mutex);
	cancel_delayed_work_sync(&dev->periodic_work);
	cancel_delayed_work_sync(&dev->radar_work);
	cancel_work_sync(&wl->tx_work);
	b43_leds_stop(dev);
	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (!dev || b43_status(dev) < B43_STAT_STARTED) {
		/* Whoops, aliens ate up the device while we were unlocked. */
		return dev;
	}

	/* Disable interrupts on the device. */
	b43_set_status(dev, B43_STAT_INITIALIZED);
	if (b43_bus_host_is_sdio(dev->dev)) {
		/* wl->mutex is locked. That is enough. */
		b43_write32(dev, B43_MMIO_GEN_IRQ_MASK, 0);
		b43_read32(dev, B43_MMIO_GEN_IRQ_MASK);	/* Flush */
	} else {
		spin_lock_irq(&wl->hardirq_lock);
		b43_write32(dev, B43_MMIO_GEN_IRQ_MASK, 0);
		b43_read32(dev, B43_MMIO_GEN_IRQ_MASK);	/* Flush */
		spin_unlock_irq(&wl->hardirq_lock);
	}
	/* Synchronize and free the interrupt handlers. Unlock to avoid deadlocks. */
	orig_dev = dev;
	mutex_unlock(&wl->mutex);
	if (b43_bus_host_is_sdio(dev->dev))
		b43_sdio_free_irq(dev);
	else
		free_irq(dev->dev->irq, dev);
	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (!dev)
		return dev;
	if (dev != orig_dev) {
		if (b43_status(dev) >= B43_STAT_STARTED)
			goto redo;
		return dev;
	}
	mask = b43_read32(dev, B43_MMIO_GEN_IRQ_MASK);
	B43_WARN_ON(mask != 0xFFFFFFFF && mask);

	/* Drain all TX queues. */
	for (queue_num = 0; queue_num < B43_QOS_QUEUE_NUM; queue_num++) {
		while (skb_queue_len(&wl->tx_queue[queue_num])) {
			struct sk_buff *skb;

			skb = skb_dequeue(&wl->tx_queue[queue_num]);
			ieee80211_free_txskb(wl->hw, skb);
		}
	}

	b43_mac_suspend(dev);
	b43_leds_exit(dev);
	b43dbg(wl, "Wireless interface stopped\n");

	return dev;
}

/* Locking: wl->mutex */
static int b43_wireless_core_start(struct b43_wldev *dev)
{
	int err;

	B43_WARN_ON(b43_status(dev) != B43_STAT_INITIALIZED);

	drain_txstatus_queue(dev);
	if (b43_bus_host_is_sdio(dev->dev)) {
		err = b43_sdio_request_irq(dev, b43_sdio_interrupt_handler);
		if (err) {
			b43err(dev->wl, "Cannot request SDIO IRQ\n");
			goto out;
		}
	} else {
		err = request_threaded_irq(dev->dev->irq, b43_interrupt_handler,
					   b43_interrupt_thread_handler,
					   IRQF_SHARED, KBUILD_MODNAME, dev);
		if (err) {
			b43err(dev->wl, "Cannot request IRQ-%d\n",
			       dev->dev->irq);
			goto out;
		}
	}

	b43info(dev->wl, "start: enabling the MAC\n");

	/* We are ready to run. */
	ieee80211_wake_queues(dev->wl->hw);
	b43_set_status(dev, B43_STAT_STARTED);

	/* Start data flow (TX/RX). */
	b43_mac_enable(dev);
	b43_write32(dev, B43_MMIO_GEN_IRQ_MASK, dev->irq_mask);

	/* Start maintenance work */
	b43_periodic_tasks_setup(dev);

	b43_leds_init(dev);

	b43dbg(dev->wl, "Wireless interface started\n");
out:
	return err;
}

static char *b43_phy_name(struct b43_wldev *dev, u8 phy_type)
{
	switch (phy_type) {
	case B43_PHYTYPE_A:
		return "A";
	case B43_PHYTYPE_B:
		return "B";
	case B43_PHYTYPE_G:
		return "G";
	case B43_PHYTYPE_N:
		return "N";
	case B43_PHYTYPE_LP:
		return "LP";
	case B43_PHYTYPE_SSLPN:
		return "SSLPN";
	case B43_PHYTYPE_HT:
		return "HT";
	case B43_PHYTYPE_LCN:
		return "LCN";
	case B43_PHYTYPE_LCNXN:
		return "LCNXN";
	case B43_PHYTYPE_LCN40:
		return "LCN40";
	case B43_PHYTYPE_AC:
		return "AC";
	}
	return "UNKNOWN";
}

/* Get PHY and RADIO versioning numbers */
static int b43_phy_versioning(struct b43_wldev *dev)
{
	struct b43_phy *phy = &dev->phy;
	const u8 core_rev = dev->dev->core_rev;
	u32 tmp;
	u8 analog_type;
	u8 phy_type;
	u8 phy_rev;
	u16 radio_manuf;
	u16 radio_id;
	u16 radio_rev;
	u8 radio_ver;
	int unsupported = 0;

	/* Get PHY versioning */
	tmp = b43_read16(dev, B43_MMIO_PHY_VER);
	analog_type = (tmp & B43_PHYVER_ANALOG) >> B43_PHYVER_ANALOG_SHIFT;
	phy_type = (tmp & B43_PHYVER_TYPE) >> B43_PHYVER_TYPE_SHIFT;
	phy_rev = (tmp & B43_PHYVER_VERSION);

	/* LCNXN is continuation of N which run out of revisions */
	if (phy_type == B43_PHYTYPE_LCNXN) {
		phy_type = B43_PHYTYPE_N;
		phy_rev += 16;
	}

	switch (phy_type) {
#ifdef CONFIG_B43_PHY_G
	case B43_PHYTYPE_G:
		if (phy_rev > 9)
			unsupported = 1;
		break;
#endif
#ifdef CONFIG_B43_PHY_N
	case B43_PHYTYPE_N:
		if (phy_rev >= 19)
			unsupported = 1;
		break;
#endif
#ifdef CONFIG_B43_PHY_LP
	case B43_PHYTYPE_LP:
		if (phy_rev > 2)
			unsupported = 1;
		break;
#endif
#ifdef CONFIG_B43_PHY_HT
	case B43_PHYTYPE_HT:
		if (phy_rev > 1)
			unsupported = 1;
		break;
#endif
#ifdef CONFIG_B43_PHY_LCN
	case B43_PHYTYPE_LCN:
		if (phy_rev > 1)
			unsupported = 1;
		break;
#endif
#ifdef CONFIG_B43_PHY_AC
	case B43_PHYTYPE_AC:
		if (phy_rev > 1)
			unsupported = 1;
		break;
#endif
	default:
		unsupported = 1;
	}
	if (unsupported) {
		b43err(dev->wl, "FOUND UNSUPPORTED PHY (Analog %u, Type %d (%s), Revision %u)\n",
		       analog_type, phy_type, b43_phy_name(dev, phy_type),
		       phy_rev);
		return -EOPNOTSUPP;
	}
	b43info(dev->wl, "Found PHY: Analog %u, Type %d (%s), Revision %u\n",
		analog_type, phy_type, b43_phy_name(dev, phy_type), phy_rev);

	/* Get RADIO versioning */
	if (b43_is_ac_core(dev)) {
		radio_manuf = 0x17F;

		b43_write16f(dev, B43_MMIO_RADIO24_CONTROL, 0);
		radio_rev = b43_read16(dev, B43_MMIO_RADIO24_DATA);

		b43_write16f(dev, B43_MMIO_RADIO24_CONTROL, 1);
		radio_id = b43_read16(dev, B43_MMIO_RADIO24_DATA);

		radio_ver = 0; /* Is there version somewhere? */
	} else if (core_rev >= 24) {
		u16 radio24[3];

		for (tmp = 0; tmp < 3; tmp++) {
			b43_write16f(dev, B43_MMIO_RADIO24_CONTROL, tmp);
			radio24[tmp] = b43_read16(dev, B43_MMIO_RADIO24_DATA);
		}

		radio_manuf = 0x17F;
		radio_id = (radio24[2] << 8) | radio24[1];
		radio_rev = (radio24[0] & 0xF);
		radio_ver = (radio24[0] & 0xF0) >> 4;
	} else {
		if (dev->dev->chip_id == 0x4317) {
			if (dev->dev->chip_rev == 0)
				tmp = 0x3205017F;
			else if (dev->dev->chip_rev == 1)
				tmp = 0x4205017F;
			else
				tmp = 0x5205017F;
		} else {
			b43_write16f(dev, B43_MMIO_RADIO_CONTROL,
				     B43_RADIOCTL_ID);
			tmp = b43_read16(dev, B43_MMIO_RADIO_DATA_LOW);
			b43_write16f(dev, B43_MMIO_RADIO_CONTROL,
				     B43_RADIOCTL_ID);
			tmp |= b43_read16(dev, B43_MMIO_RADIO_DATA_HIGH) << 16;
		}
		radio_manuf = (tmp & 0x00000FFF);
		radio_id = (tmp & 0x0FFFF000) >> 12;
		radio_rev = (tmp & 0xF0000000) >> 28;
		radio_ver = 0; /* Probably not available on old hw */
	}

	if (radio_manuf != 0x17F /* Broadcom */)
		unsupported = 1;
	switch (phy_type) {
	case B43_PHYTYPE_B:
		if ((radio_id & 0xFFF0) != 0x2050)
			unsupported = 1;
		break;
	case B43_PHYTYPE_G:
		if (radio_id != 0x2050)
			unsupported = 1;
		break;
	case B43_PHYTYPE_N:
		if (radio_id != 0x2055 && radio_id != 0x2056 &&
		    radio_id != 0x2057)
			unsupported = 1;
		if (radio_id == 0x2057 &&
		    !(radio_rev == 9 || radio_rev == 14))
			unsupported = 1;
		break;
	case B43_PHYTYPE_LP:
		if (radio_id != 0x2062 && radio_id != 0x2063)
			unsupported = 1;
		break;
	case B43_PHYTYPE_HT:
		if (radio_id != 0x2059)
			unsupported = 1;
		break;
	case B43_PHYTYPE_LCN:
		if (radio_id != 0x2064)
			unsupported = 1;
		break;
	case B43_PHYTYPE_AC:
		if (radio_id != 0x2069)
			unsupported = 1;
		break;
	default:
		B43_WARN_ON(1);
	}
	if (unsupported) {
		b43err(dev->wl,
		       "FOUND UNSUPPORTED RADIO (Manuf 0x%X, ID 0x%X, Revision %u, Version %u)\n",
		       radio_manuf, radio_id, radio_rev, radio_ver);
		return -EOPNOTSUPP;
	}
	b43info(dev->wl,
		"Found Radio: Manuf 0x%X, ID 0x%X, Revision %u, Version %u\n",
		radio_manuf, radio_id, radio_rev, radio_ver);

	/* FIXME: b43 treats "id" as "ver" and ignores the real "ver" */
	phy->radio_manuf = radio_manuf;
	phy->radio_ver = radio_id;
	phy->radio_rev = radio_rev;

	phy->analog = analog_type;
	phy->type = phy_type;
	phy->rev = phy_rev;

	return 0;
}

static void setup_struct_phy_for_init(struct b43_wldev *dev,
				      struct b43_phy *phy)
{
	phy->hardware_power_control = !!modparam_hwpctl;
	phy->next_txpwr_check_time = jiffies;
	/* PHY TX errors counter. */
	atomic_set(&phy->txerr_cnt, B43_PHY_TX_BADNESS_LIMIT);

#if B43_DEBUG
	phy->phy_locked = false;
	phy->radio_locked = false;
#endif
}

static void setup_struct_wldev_for_init(struct b43_wldev *dev)
{
	dev->dfq_valid = false;

	/* Assume the radio is enabled. If it's not enabled, the state will
	 * immediately get fixed on the first periodic work run. */
	dev->radio_hw_enable = true;

	/* Stats */
	memset(&dev->stats, 0, sizeof(dev->stats));

	setup_struct_phy_for_init(dev, &dev->phy);

	/* IRQ related flags */
	dev->irq_reason = 0;
	memset(dev->dma_reason, 0, sizeof(dev->dma_reason));
	if (dev->phy.type == B43_PHYTYPE_AC) {
		dev->irq_mask = B43_IRQ_MASKTEMPLATE_AC;
	} else {
		dev->irq_mask = B43_IRQ_MASKTEMPLATE;
		if (b43_modparam_verbose < B43_VERBOSITY_DEBUG)
			dev->irq_mask &= ~B43_IRQ_PHY_TXERR;
	}

	dev->mac_suspended = 1;

	/* Noise calculation context */
	memset(&dev->noisecalc, 0, sizeof(dev->noisecalc));

	/* The PHY init retunes, and its calibrations are owed again. */
	memset(&dev->tuned_chandef, 0, sizeof(dev->tuned_chandef));
}

static void b43_bluetooth_coext_enable(struct b43_wldev *dev)
{
	struct ssb_sprom *sprom = dev->dev->bus_sprom;
	u64 hf;

	if (!modparam_btcoex)
		return;
	if (!(sprom->boardflags_lo & B43_BFL_BTCOEXIST))
		return;
	if (dev->phy.type != B43_PHYTYPE_B && !dev->phy.gmode)
		return;

	hf = b43_hf_read(dev);
	if (sprom->boardflags_lo & B43_BFL_BTCMOD)
		hf |= B43_HF_BTCOEXALT;
	else
		hf |= B43_HF_BTCOEX;
	b43_hf_write(dev, hf);
}

static void b43_bluetooth_coext_disable(struct b43_wldev *dev)
{
	if (!modparam_btcoex)
		return;
	//TODO
}

static void b43_imcfglo_timeouts_workaround(struct b43_wldev *dev)
{
	struct ssb_bus *bus;
	u32 tmp;

#ifdef CONFIG_B43_SSB
	if (dev->dev->bus_type != B43_BUS_SSB)
		return;
#else
	return;
#endif

	bus = dev->dev->sdev->bus;

	if ((bus->chip_id == 0x4311 && bus->chip_rev == 2) ||
	    (bus->chip_id == 0x4312)) {
		tmp = ssb_read32(dev->dev->sdev, SSB_IMCFGLO);
		tmp &= ~SSB_IMCFGLO_REQTO;
		tmp &= ~SSB_IMCFGLO_SERTO;
		tmp |= 0x3;
		ssb_write32(dev->dev->sdev, SSB_IMCFGLO, tmp);
		ssb_commit_settings(bus);
	}
}

static void b43_set_synth_pu_delay(struct b43_wldev *dev, bool idle)
{
	u16 pu_delay;

	/* The time value is in microseconds. The AC cores' stock driver ends
	 * at 512, the value the AC-PHY writes, with wl 6.30 and 7.14. */
	pu_delay = dev->phy.type == B43_PHYTYPE_AC ? 512 : 1050;
	if (b43_is_mode(dev->wl, NL80211_IFTYPE_ADHOC) || idle)
		pu_delay = 500;
	if ((dev->phy.radio_ver == 0x2050) && (dev->phy.radio_rev == 8))
		pu_delay = max(pu_delay, (u16)2400);

	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_SPUWKUP, pu_delay);
}

/* Set the TSF CFP pre-TargetBeaconTransmissionTime. */
static void b43_set_pretbtt(struct b43_wldev *dev)
{
	u16 pretbtt;

	/*
	 * The time value is in microseconds. The AC microcode builds the
	 * beacon from template RAM on its own, and its stock driver writes 2
	 * in AP mode too.
	 */
	if (b43_is_mode(dev->wl, NL80211_IFTYPE_ADHOC) ||
	    dev->phy.type == B43_PHYTYPE_AC)
		pretbtt = 2;
	else
		pretbtt = 250;
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PRETBTT, pretbtt);
	b43_write16(dev, B43_MMIO_TSF_CFP_PRETBTT, pretbtt);
}

/* Shutdown a wireless core */
/* Locking: wl->mutex */
static void b43_wireless_core_exit(struct b43_wldev *dev)
{
	B43_WARN_ON(dev && b43_status(dev) > B43_STAT_INITIALIZED);
	if (!dev || b43_status(dev) != B43_STAT_INITIALIZED)
		return;

	b43_set_status(dev, B43_STAT_UNINIT);

	/*
	 * The AC PHY's down sequence brackets its writes with MAC enable and
	 * suspend, which want the microcode running: the stock driver runs it
	 * before it resets the core, so the PHY goes down before the PSM
	 * stops here.
	 */
	if (dev->phy.type == B43_PHYTYPE_AC)
		b43_chip_exit(dev);

	/* Stop the microcode PSM. */
	b43_maskset32(dev, B43_MMIO_MACCTL, ~B43_MACCTL_PSM_RUN,
		      B43_MACCTL_PSM_JMP0);

	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		bcma_host_pci_down(dev->dev->bdev->bus);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		/* TODO */
		break;
#endif
	}

	b43_dma_free(dev);
	b43_pio_free(dev);
	if (dev->phy.type != B43_PHYTYPE_AC)
		b43_chip_exit(dev);
	dev->phy.ops->switch_analog(dev, 0);
	if (dev->wl->current_beacon) {
		dev_kfree_skb_any(dev->wl->current_beacon);
		dev->wl->current_beacon = NULL;
	}

	b43_device_disable(dev, 0);
	b43_bus_may_powerdown(dev);
}

/* Initialize a wireless core */
static int b43_wireless_core_init(struct b43_wldev *dev)
{
	struct ssb_sprom *sprom = dev->dev->bus_sprom;
	struct b43_phy *phy = &dev->phy;
	int err;
	u64 hf;

	B43_WARN_ON(b43_status(dev) != B43_STAT_UNINIT);

	err = b43_bus_powerup(dev, 0);
	if (err)
		goto out;
	if (!b43_device_is_enabled(dev))
		b43_wireless_core_reset(dev, phy->gmode);

	/* Reset all data structures. */
	setup_struct_wldev_for_init(dev);
	phy->ops->prepare_structs(dev);

	/* Enable IRQ routing to this device. */
	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		bcma_host_pci_irq_ctl(dev->dev->bdev->bus,
				      dev->dev->bdev, true);
		bcma_host_pci_up(dev->dev->bdev->bus);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		ssb_pcicore_dev_irqvecs_enable(&dev->dev->sdev->bus->pcicore,
					       dev->dev->sdev);
		break;
#endif
	}

	b43_imcfglo_timeouts_workaround(dev);
	b43_bluetooth_coext_disable(dev);
	if (phy->ops->prepare_hardware) {
		err = phy->ops->prepare_hardware(dev);
		if (err)
			goto err_busdown;
	}
	err = b43_chip_init(dev);
	if (err)
		goto err_busdown;
	b43_shm_write16(dev, B43_SHM_SHARED,
			B43_SHM_SH_WLCOREREV, dev->dev->core_rev);
	/*
	 * Both stock drivers write this right after the core revision, in AP
	 * and station mode, and keep it in every later MAC command (6.30 on
	 * the archer-t5e, 7.14 on the agcombo). What it gates in the AC
	 * microcode is not known; brcmsmac's older ucode takes it only at the
	 * end of an IBSS ATIM window.
	 */
	if (dev->fw.hdr_format == B43_FW_HDR_AC)
		b43_write32(dev, B43_MMIO_MACCMD, B43_MACCMD_DFQ_VALID);
	hf = b43_hf_read(dev);
	if (phy->type == B43_PHYTYPE_G) {
		hf |= B43_HF_SYMW;
		if (phy->rev == 1)
			hf |= B43_HF_GDCW;
		if (sprom->boardflags_lo & B43_BFL_PACTRL)
			hf |= B43_HF_OFDMPABOOST;
	}
	if (phy->radio_ver == 0x2050) {
		if (phy->radio_rev == 6)
			hf |= B43_HF_4318TSSI;
		if (phy->radio_rev < 6)
			hf |= B43_HF_VCORECALC;
	}
	if (sprom->boardflags_lo & B43_BFL_XTAL_NOSLOW)
		hf |= B43_HF_DSCRQ; /* Disable slowclock requests from ucode. */
#if defined(CONFIG_B43_SSB) && defined(CONFIG_SSB_DRIVER_PCICORE)
	if (dev->dev->bus_type == B43_BUS_SSB &&
	    dev->dev->sdev->bus->bustype == SSB_BUSTYPE_PCI &&
	    dev->dev->sdev->bus->pcicore.dev->id.revision <= 10)
		hf |= B43_HF_PCISCW; /* PCI slow clock workaround. */
#endif
	hf &= ~B43_HF_SKCFPUP;
	b43_hf_write(dev, hf);

	/* tell the ucode MAC capabilities */
	if (dev->dev->core_rev >= 13) {
		u32 mac_hw_cap = b43_read32(dev, B43_MMIO_MAC_HW_CAP);

		/*
		 * The AC microcode gets bit 31 cleared from every stock driver
		 * in the captures -- 6.30.102.7 with ucode 784, 7.14 with 928
		 * on three boards -- except the x86 hybrid 6.30.223, which
		 * leaves it set for its ucode 832. That one is followed too:
		 * the captures cannot tell the ucode from the build.
		 */
		if (dev->fw.hdr_format == B43_FW_HDR_AC && dev->fw.rev != 832)
			mac_hw_cap &= ~0x80000000;

		b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_MACHW_L,
				mac_hw_cap & 0xffff);
		b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_MACHW_H,
				(mac_hw_cap >> 16) & 0xffff);
	}

	b43_set_retry_limits(dev, B43_DEFAULT_SHORT_RETRY_LIMIT,
			     B43_DEFAULT_LONG_RETRY_LIMIT);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_SFFBLIM, 3);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_LFFBLIM, 2);

	/*
	 * The initvals leave IFSCTL at 0x0f0b and the four entries of the
	 * table behind it at 2. Both stock drivers captured at the bus, 7.14
	 * on the agcombo and 6.30 on the MacBookAir6,1, clear bit 3 and set
	 * entry 0, the one IFSCTL then selects, to 1.
	 */
	if (phy->type == B43_PHYTYPE_AC) {
		b43_maskset16(dev, B43_MMIO_IFSCTL, ~B43_MMIO_IFSCTL_BIT3, 0);
		b43_write16(dev, B43_MMIO_IFSTBL, 1);
	}

	/*
	 * Cells b43 has been leaving at whatever the microcode starts them
	 * with, and that the OEM driver writes. Every value below is the same
	 * on a BCM4352 and on a BCM4360, across every channel and both driver
	 * versions, so none of it is board data.
	 *
	 * The meaning of most of these is not established beyond the name
	 * b43.h already carries, and the values are observed rather than
	 * derived. They are written because the hardware is evidently expected
	 * to have them: leaving a cell at a microcode default the OEM driver
	 * always overwrites is a guess too, and the worse one.
	 *
	 * Where a cell is written more than once during bring-up -- host flags
	 * 4 and 5 gain bits as the driver goes -- the value here is the one it
	 * ends at. The beacon TSF offset depends on the mode and is in
	 * b43_adjust_opmode(). b43's core init has a
	 * different shape from the OEM's and the intermediate points have no
	 * counterpart in it, so the end state matches and the order does not.
	 *
	 * Not written: the probe response offload cells, PRSSID, PRSSIDLEN and
	 * PRTLEN. b43 turns that offload off with PRMAXTIME just below, so
	 * they are meant to be untouched.
	 */
	if (dev->dev->core_rev >= 42) {
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_MAXBFRAMES, 8);
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_ANTSWAP, 0x000a);
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_DEFAULTIV, 0x000a);
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_RFATT, 0x0480);
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_HOSTF4, 0x0060);
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_SHM_SH_HOSTF5, 0x8088);
	}

	/*
	 * 5 GHz has one slot time and it is the short one, so set it here
	 * rather than waiting for mac80211 to report an ERP slot change that
	 * will never come. b43_set_slot_time() writes the shared-memory cell
	 * and leaves the 2.4 GHz IFS slot register alone on this band.
	 */
	if (b43_current_band(dev->wl) == NL80211_BAND_5GHZ)
		b43_short_slot_timing_enable(dev);

	/* Disable sending probe responses from firmware.
	 * Setting the MaxTime to one usec will always trigger
	 * a timeout, so we never send any probe resp.
	 * A timeout of zero is infinite. */
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PRMAXTIME, 1);

	b43_rate_memory_init(dev);
	b43_set_phytxctl_defaults(dev);

	/* Minimum Contention Window */
	if (phy->type == B43_PHYTYPE_B)
		b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_MINCONT, 0x1F);
	else
		b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_MINCONT, 0xF);
	/* Maximum Contention Window */
	b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_MAXCONT, 0x3FF);

	/* write phytype and phyvers */
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PHYTYPE, phy->type);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PHYVER, phy->rev);

	b43info(dev->wl, "init: DMA\n");

	if (b43_bus_host_is_pcmcia(dev->dev) ||
	    b43_bus_host_is_sdio(dev->dev)) {
		dev->__using_pio_transfers = true;
		err = b43_pio_init(dev);
	} else if (dev->use_pio && phy->type == B43_PHYTYPE_AC) {
		/*
		 * pio.c reads the receive header in the pre-AC layouts only, so
		 * the AC stays on DMA, also after a fatal DMA error asked for
		 * PIO.
		 */
		b43err(dev->wl, "PIO is not supported on the AC-PHY, using DMA\n");
		dev->use_pio = false;
		dev->__using_pio_transfers = false;
		err = b43_dma_init(dev);
	} else if (dev->use_pio) {
		b43warn(dev->wl, "Forced PIO by use_pio module parameter. "
			"This should not be needed and will result in lower "
			"performance.\n");
		dev->__using_pio_transfers = true;
		err = b43_pio_init(dev);
	} else {
		dev->__using_pio_transfers = false;
		err = b43_dma_init(dev);
	}
	if (err)
		goto err_chip_exit;
	b43_qos_init(dev);
	/* Both stock drivers captured at the bus write it once, here. */
	if (phy->type == B43_PHYTYPE_AC)
		b43_write32(dev, B43_MMIO_UNK_3DC, 10000000);
	b43_set_synth_pu_delay(dev, 1);
	b43_bluetooth_coext_enable(dev);

	b43_bus_powerup(dev, !(sprom->boardflags_lo & B43_BFL_XTAL_NOSLOW));
	/*
	 * Clear the whole address match table before anything is put in it. The
	 * OEM driver does this from its own core init, all 64 rows, and it
	 * matters because a row left over with its valid flag set matches an
	 * address nobody asked for.
	 */
	if (b43_amt_wide(dev)) {
		u16 i;

		for (i = 0; i < B43_AMT_WIDE_ENTRIES; i++)
			b43_amt_write(dev, i, NULL, 0);
	}

	b43info(dev->wl, "init: address match and keys\n");
	b43_upload_card_macaddress(dev);
	b43_security_init(dev);

	/* The AC PHY comes up after the MAC is set up, see b43_chip_init(). */
	if (phy->type == B43_PHYTYPE_AC) {
		b43info(dev->wl, "init: PHY\n");
		err = b43_phy_bringup(dev);
		if (err)
			goto err_dma_free;
	}

	ieee80211_wake_queues(dev->wl->hw);

	b43_set_status(dev, B43_STAT_INITIALIZED);

out:
	return err;

err_dma_free:
	b43_dma_free(dev);
	b43_pio_free(dev);
	b43_gpio_cleanup(dev);
	goto err_busdown;
err_chip_exit:
	if (phy->type == B43_PHYTYPE_AC)
		b43_gpio_cleanup(dev);
	else
		b43_chip_exit(dev);
err_busdown:
	b43_bus_may_powerdown(dev);
	B43_WARN_ON(b43_status(dev) != B43_STAT_UNINIT);
	return err;
}

static int b43_op_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;
	int err = -EOPNOTSUPP;

	/* TODO: allow AP devices to coexist */

	if (vif->type != NL80211_IFTYPE_AP &&
	    vif->type != NL80211_IFTYPE_MESH_POINT &&
	    vif->type != NL80211_IFTYPE_STATION &&
	    vif->type != NL80211_IFTYPE_ADHOC)
		return -EOPNOTSUPP;

	mutex_lock(&wl->mutex);
	if (wl->operating)
		goto out_mutex_unlock;

	b43dbg(wl, "Adding Interface type %d\n", vif->type);

	dev = wl->current_dev;
	if (vif->type != NL80211_IFTYPE_STATION && !dev->fw.tpl)
		goto out_mutex_unlock;

	wl->operating = true;
	wl->vif = vif;
	wl->if_type = vif->type;
	memcpy(wl->mac_addr, vif->addr, ETH_ALEN);

	b43_adjust_opmode(dev);
	b43_set_pretbtt(dev);
	b43_set_synth_pu_delay(dev, 0);
	b43_upload_card_macaddress(dev);

	err = 0;
 out_mutex_unlock:
	mutex_unlock(&wl->mutex);

	if (err == 0)
		b43_op_bss_info_changed(hw, vif, &vif->bss_conf, ~0);

	return err;
}

static void b43_op_remove_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev = wl->current_dev;

	b43dbg(wl, "Removing Interface type %d\n", vif->type);

	mutex_lock(&wl->mutex);

	B43_WARN_ON(!wl->operating);
	B43_WARN_ON(wl->vif != vif);
	wl->vif = NULL;

	wl->operating = false;

	b43_adjust_opmode(dev);
	eth_zero_addr(wl->mac_addr);
	b43_upload_card_macaddress(dev);

	mutex_unlock(&wl->mutex);
}

static int b43_op_start(struct ieee80211_hw *hw)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev = wl->current_dev;
	int did_init = 0;
	int err = 0;

	/* Kill all old instance specific information to make sure
	 * the card won't use it in the short timeframe between start
	 * and mac80211 reconfiguring it. */
	eth_zero_addr(wl->bssid);
	eth_zero_addr(wl->mac_addr);
	wl->filter_flags = 0;
	wl->radiotap_enabled = false;
	b43_qos_clear(wl);
	wl->beacon0_uploaded = false;
	wl->beacon1_uploaded = false;
	wl->beacon_templates_virgin = true;
	wl->radio_enabled = true;

	mutex_lock(&wl->mutex);

	if (b43_status(dev) < B43_STAT_INITIALIZED) {
		err = b43_wireless_core_init(dev);
		if (err)
			goto out_mutex_unlock;
		did_init = 1;
	}

	if (b43_status(dev) < B43_STAT_STARTED) {
		err = b43_wireless_core_start(dev);
		if (err) {
			if (did_init)
				b43_wireless_core_exit(dev);
			goto out_mutex_unlock;
		}
	}

	/* XXX: only do if device doesn't support rfkill irq */
	wiphy_rfkill_start_polling(hw->wiphy);

 out_mutex_unlock:
	mutex_unlock(&wl->mutex);

	/*
	 * Configuration may have been overwritten during initialization.
	 * Reload the configuration, but only if initialization was
	 * successful. Reloading the configuration after a failed init
	 * may hang the system.
	 */
	if (!err)
		b43_op_config(hw, ~0);

	return err;
}

static void b43_op_stop(struct ieee80211_hw *hw)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev = wl->current_dev;

	cancel_work_sync(&(wl->beacon_update_trigger));

	if (!dev)
		goto out;

	mutex_lock(&wl->mutex);
	if (b43_status(dev) >= B43_STAT_STARTED) {
		dev = b43_wireless_core_stop(dev);
		if (!dev)
			goto out_unlock;
	}
	b43_wireless_core_exit(dev);
	wl->radio_enabled = false;

out_unlock:
	mutex_unlock(&wl->mutex);
out:
	cancel_work_sync(&(wl->txpower_adjust_work));
}

static int b43_op_beacon_set_tim(struct ieee80211_hw *hw,
				 struct ieee80211_sta *sta, bool set)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);

	b43_update_templates(wl);

	return 0;
}

static void b43_op_sta_notify(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      enum sta_notify_cmd notify_cmd,
			      struct ieee80211_sta *sta)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);

	B43_WARN_ON(!vif || wl->vif != vif);
}

static void b43_op_sw_scan_start_notifier(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif,
					  const u8 *mac_addr)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (dev && (b43_status(dev) >= B43_STAT_INITIALIZED)) {
		/* Disable CFP update during scan on other channels. */
		b43_hf_write(dev, b43_hf_read(dev) | B43_HF_SKCFPUP);
	}
	mutex_unlock(&wl->mutex);
}

static void b43_op_sw_scan_complete_notifier(struct ieee80211_hw *hw,
					     struct ieee80211_vif *vif)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev;

	mutex_lock(&wl->mutex);
	dev = wl->current_dev;
	if (dev && (b43_status(dev) >= B43_STAT_INITIALIZED)) {
		/* Re-enable CFP update. */
		b43_hf_write(dev, b43_hf_read(dev) & ~B43_HF_SKCFPUP);
	}
	mutex_unlock(&wl->mutex);
}

static int b43_op_get_survey(struct ieee80211_hw *hw, int idx,
			     struct survey_info *survey)
{
	struct b43_wl *wl = hw_to_b43_wl(hw);
	struct b43_wldev *dev = wl->current_dev;
	struct ieee80211_conf *conf = &hw->conf;

	if (idx != 0)
		return -ENOENT;

	survey->channel = conf->chandef.chan;
	survey->filled = SURVEY_INFO_NOISE_DBM;
	survey->noise = dev->stats.link_noise;

	return 0;
}

static const struct ieee80211_ops b43_hw_ops = {
	.tx			= b43_op_tx,
	.wake_tx_queue		= ieee80211_handle_wake_tx_queue,
	.conf_tx		= b43_op_conf_tx,
	.add_interface		= b43_op_add_interface,
	.remove_interface	= b43_op_remove_interface,
	.config			= b43_op_config,
	.bss_info_changed	= b43_op_bss_info_changed,
	.configure_filter	= b43_op_configure_filter,
	.set_key		= b43_op_set_key,
	.update_tkip_key	= b43_op_update_tkip_key,
	.get_stats		= b43_op_get_stats,
	.get_tsf		= b43_op_get_tsf,
	.set_tsf		= b43_op_set_tsf,
	.start			= b43_op_start,
	.stop			= b43_op_stop,
	.set_tim		= b43_op_beacon_set_tim,
	.sta_notify		= b43_op_sta_notify,
	.sw_scan_start		= b43_op_sw_scan_start_notifier,
	.sw_scan_complete	= b43_op_sw_scan_complete_notifier,
	.get_survey		= b43_op_get_survey,
	.rfkill_poll		= b43_rfkill_poll,
};

/* Hard-reset the chip. Do not call this directly.
 * Use b43_controller_restart()
 */
static void b43_chip_reset(struct work_struct *work)
{
	struct b43_wldev *dev =
	    container_of(work, struct b43_wldev, restart_work);
	struct b43_wl *wl = dev->wl;
	int err = 0;
	int prev_status;

	mutex_lock(&wl->mutex);

	prev_status = b43_status(dev);
	/* Bring the device down... */
	if (prev_status >= B43_STAT_STARTED) {
		dev = b43_wireless_core_stop(dev);
		if (!dev) {
			err = -ENODEV;
			goto out;
		}
	}
	if (prev_status >= B43_STAT_INITIALIZED)
		b43_wireless_core_exit(dev);

	/* ...and up again. */
	if (prev_status >= B43_STAT_INITIALIZED) {
		err = b43_wireless_core_init(dev);
		if (err)
			goto out;
	}
	if (prev_status >= B43_STAT_STARTED) {
		err = b43_wireless_core_start(dev);
		if (err) {
			b43_wireless_core_exit(dev);
			goto out;
		}
	}
out:
	if (err)
		wl->current_dev = NULL; /* Failed to init the dev. */
	mutex_unlock(&wl->mutex);

	if (err) {
		b43err_restart(wl, "Controller restart FAILED\n");
		return;
	}

	/* reload configuration */
	b43_op_config(wl->hw, ~0);
	if (wl->vif)
		b43_op_bss_info_changed(wl->hw, wl->vif, &wl->vif->bss_conf, ~0);

	b43info(wl, "Controller restarted\n");
}

static const struct ieee80211_iface_limit b43_if_limits_dfs[] = {
	{ .max = 1, .types = BIT(NL80211_IFTYPE_AP) },
};

static const struct ieee80211_iface_combination b43_if_comb_dfs = {
	.limits = b43_if_limits_dfs,
	.n_limits = ARRAY_SIZE(b43_if_limits_dfs),
	.max_interfaces = 1,
	.num_different_channels = 1,
	.radar_detect_widths = BIT(NL80211_CHAN_WIDTH_20_NOHT) |
			       BIT(NL80211_CHAN_WIDTH_20) |
			       BIT(NL80211_CHAN_WIDTH_40) |
			       BIT(NL80211_CHAN_WIDTH_80),
};

/*
 * What the AC PHY tells cfg80211 and mac80211 it can do in HT and VHT. The
 * starting point is what the stock driver puts in its beacon (agcombo,
 * 7.14.43, ch36/80): HT 20/40 with the short guard interval on both, SM
 * power save disabled; VHT at 80 MHz with the short guard interval; MCS 0-7
 * and 0-9 on every receive chain. Left out, because b43 does not do them:
 *
 *  - aggregation: the A-MPDU parameters stay at their minimum, and with no
 *    ampdu_action mac80211 opens no block ack session either way;
 *  - transmitting HT rates: the TX MCS set is not defined, so mac80211
 *    picks legacy rates for an HT station, see b43_op_tx(). VHT goes out
 *    in the layout of the stock driver's descriptors (b43_txhdr_ac_vht()),
 *    MCS 0-9 on as many streams as the board has TX chains;
 *  - the long MPDUs and A-MSDUs, beyond the receive buffer;
 *  - LDPC reception, beamforming and the link adaptation the stock driver
 *    announces on VHT: nothing configures or decodes them. A VHT frame is
 *    sent with LDPC when mac80211 asks, for a station that receives it, as
 *    every VHT frame of the stock driver's is.
 *
 * The stock beacon carries the 4360's three streams; the streams here are
 * the board's chains, the SROM rxchain the PHY also uses for receiving and
 * the txchain for transmitting.
 */
static void b43_ac_set_ht_vht_cap(struct b43_wldev *dev,
				  struct ieee80211_supported_band *band)
{
	struct ieee80211_sta_ht_cap *ht = &band->ht_cap;
	struct ieee80211_sta_vht_cap *vht = &band->vht_cap;
	u8 chains = dev->dev->bus_sprom->rxchain & 0x07;
	u8 txchains = dev->dev->bus_sprom->txchain & 0x07;
	unsigned int nss, tx_nss, i;
	u16 map = 0, tx_map = 0;

	if (!chains)
		chains = 0x03;
	if (!txchains)
		txchains = chains;
	nss = hweight8(chains);
	tx_nss = hweight8(txchains);

	memset(ht, 0, sizeof(*ht));
	ht->ht_supported = true;
	ht->cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		  IEEE80211_HT_CAP_SGI_20 |
		  IEEE80211_HT_CAP_SGI_40 |
		  (WLAN_HT_CAP_SM_PS_DISABLED << IEEE80211_HT_CAP_SM_PS_SHIFT);
	ht->ampdu_factor = IEEE80211_HT_MAX_AMPDU_8K;
	ht->ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE;
	for (i = 0; i < nss; i++)
		ht->mcs.rx_mask[i] = 0xff;

	for (i = 0; i < 8; i++) {
		map |= (i < nss ? IEEE80211_VHT_MCS_SUPPORT_0_9 :
				  IEEE80211_VHT_MCS_NOT_SUPPORTED) << (2 * i);
		tx_map |= (i < tx_nss ? IEEE80211_VHT_MCS_SUPPORT_0_9 :
					IEEE80211_VHT_MCS_NOT_SUPPORTED) << (2 * i);
	}
	memset(vht, 0, sizeof(*vht));
	vht->vht_supported = true;
	vht->cap = IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_3895 |
		   IEEE80211_VHT_CAP_SHORT_GI_80;
	vht->vht_mcs.rx_mcs_map = cpu_to_le16(map);
	vht->vht_mcs.tx_mcs_map = cpu_to_le16(tx_map);
}

static int b43_setup_bands(struct b43_wldev *dev,
			   bool have_2ghz_phy, bool have_5ghz_phy)
{
	struct ieee80211_hw *hw = dev->wl->hw;
	struct b43_phy *phy = &dev->phy;
	bool limited_2g;
	bool limited_5g;

	/* We don't support all 2 GHz channels on some devices */
	limited_2g = phy->radio_ver == 0x2057 &&
		     (phy->radio_rev == 9 || phy->radio_rev == 14);
	limited_5g = phy->radio_ver == 0x2057 &&
		     phy->radio_rev == 9;

	if (have_2ghz_phy)
		hw->wiphy->bands[NL80211_BAND_2GHZ] = limited_2g ?
			&b43_band_2ghz_limited : &b43_band_2GHz;
	if (dev->phy.type == B43_PHYTYPE_AC) {
		if (have_5ghz_phy) {
			struct ieee80211_supported_band *band =
				&dev->wl->band_5ghz_ac;

			*band = b43_band_5GHz_acphy;
			b43_ac_set_ht_vht_cap(dev, band);
			hw->wiphy->bands[NL80211_BAND_5GHZ] = band;
		}
	} else
	if (dev->phy.type == B43_PHYTYPE_N) {
		if (have_5ghz_phy)
			hw->wiphy->bands[NL80211_BAND_5GHZ] = limited_5g ?
				&b43_band_5GHz_nphy_limited :
				&b43_band_5GHz_nphy;
	} else {
		if (have_5ghz_phy)
			hw->wiphy->bands[NL80211_BAND_5GHZ] = &b43_band_5GHz_aphy;
	}

	dev->phy.supports_2ghz = have_2ghz_phy;
	dev->phy.supports_5ghz = have_5ghz_phy;

	/*
	 * An AP on a radar channel needs the combination to say which widths
	 * radar detection covers, or cfg80211 refuses the check. Behind
	 * CONFIG_B43_DFS, as ath9k and ath10k do: any pulse over the PHY's
	 * threshold is reported, without matching it against the radar
	 * patterns.
	 */
	if (IS_ENABLED(CONFIG_B43_DFS) && have_5ghz_phy &&
	    dev->phy.type == B43_PHYTYPE_AC && dev->phy.ops->radar_poll) {
		hw->wiphy->iface_combinations = &b43_if_comb_dfs;
		hw->wiphy->n_iface_combinations = 1;
	}

	return 0;
}

static void b43_wireless_core_detach(struct b43_wldev *dev)
{
	/* We release firmware that late to not be required to re-request
	 * is all the time when we reinit the core. */
	b43_release_firmware(dev);
	b43_phy_free(dev);
}

static void b43_supported_bands(struct b43_wldev *dev, bool *have_2ghz_phy,
				bool *have_5ghz_phy)
{
	u16 dev_id = 0;

#ifdef CONFIG_B43_BCMA
	if (dev->dev->bus_type == B43_BUS_BCMA &&
	    dev->dev->bdev->bus->hosttype == BCMA_HOSTTYPE_PCI)
		dev_id = dev->dev->bdev->bus->host_pci->device;
#endif
#ifdef CONFIG_B43_SSB
	if (dev->dev->bus_type == B43_BUS_SSB &&
	    dev->dev->sdev->bus->bustype == SSB_BUSTYPE_PCI)
		dev_id = dev->dev->sdev->bus->host_pci->device;
#endif
	/* Override with SPROM value if available */
	if (dev->dev->bus_sprom->dev_id)
		dev_id = dev->dev->bus_sprom->dev_id;

	/* Note: below IDs can be "virtual" (not maching e.g. real PCI ID) */
	switch (dev_id) {
	case 0x4324: /* BCM4306 */
	case 0x4312: /* BCM4311 */
	case 0x4319: /* BCM4318 */
	case 0x4328: /* BCM4321 */
	case 0x432b: /* BCM4322 */
	case 0x4350: /* BCM43222 */
	case 0x4353: /* BCM43224 */
	case 0x0576: /* BCM43224 */
	case 0x435f: /* BCM6362 */
	case 0x4331: /* BCM4331 */
	case 0x4359: /* BCM43228 */
	case 0x43a0: /* BCM4360 */
	case 0x43b1: /* BCM4352 */
		/* Dual band devices */
		*have_2ghz_phy = true;
		*have_5ghz_phy = true;
		return;
	case 0x4321: /* BCM4306 */
		/* There are 14e4:4321 PCI devs with 2.4 GHz BCM4321 (N-PHY) */
		if (dev->phy.type != B43_PHYTYPE_G)
			break;
		fallthrough;
	case 0x4313: /* BCM4311 */
	case 0x431a: /* BCM4318 */
	case 0x432a: /* BCM4321 */
	case 0x432d: /* BCM4322 */
	case 0x4352: /* BCM43222 */
	case 0x435a: /* BCM43228 */
	case 0x4333: /* BCM4331 */
	case 0x43a2: /* BCM4360 */
	case 0x43b3: /* BCM4352 */
		/* 5 GHz only devices */
		*have_2ghz_phy = false;
		*have_5ghz_phy = true;
		return;
	}

	/* As a fallback, try to guess using PHY type */
	switch (dev->phy.type) {
	case B43_PHYTYPE_AC:
		/* Dual-band (BCM4352/4360). Real dev_ids are resolved above;
		 * this only guards the by-PHY fallback. */
		*have_2ghz_phy = true;
		*have_5ghz_phy = true;
		return;
	case B43_PHYTYPE_G:
	case B43_PHYTYPE_N:
	case B43_PHYTYPE_LP:
	case B43_PHYTYPE_HT:
	case B43_PHYTYPE_LCN:
		*have_2ghz_phy = true;
		*have_5ghz_phy = false;
		return;
	}

	B43_WARN_ON(1);
}

static int b43_wireless_core_attach(struct b43_wldev *dev)
{
	struct b43_wl *wl = dev->wl;
	struct b43_phy *phy = &dev->phy;
	int err;
	u32 tmp;
	bool have_2ghz_phy = false, have_5ghz_phy = false;

	/* Do NOT do any device initialization here.
	 * Do it in wireless_core_init() instead.
	 * This function is for gathering basic information about the HW, only.
	 * Also some structs may be set up here. But most likely you want to have
	 * that in core_init(), too.
	 */

	err = b43_bus_powerup(dev, 0);
	if (err) {
		b43err(wl, "Bus powerup failed\n");
		goto out;
	}

	phy->do_full_init = true;

	/* Try to guess supported bands for the first init needs */
	switch (dev->dev->bus_type) {
#ifdef CONFIG_B43_BCMA
	case B43_BUS_BCMA:
		tmp = bcma_aread32(dev->dev->bdev, BCMA_IOST);
		have_2ghz_phy = !!(tmp & B43_BCMA_IOST_2G_PHY);
		have_5ghz_phy = !!(tmp & B43_BCMA_IOST_5G_PHY);
		break;
#endif
#ifdef CONFIG_B43_SSB
	case B43_BUS_SSB:
		if (dev->dev->core_rev >= 5) {
			tmp = ssb_read32(dev->dev->sdev, SSB_TMSHIGH);
			have_2ghz_phy = !!(tmp & B43_TMSHIGH_HAVE_2GHZ_PHY);
			have_5ghz_phy = !!(tmp & B43_TMSHIGH_HAVE_5GHZ_PHY);
		} else
			B43_WARN_ON(1);
		break;
#endif
	}

	dev->phy.gmode = have_2ghz_phy;
	b43_wireless_core_reset(dev, dev->phy.gmode);

	/* Get the PHY type. */
	err = b43_phy_versioning(dev);
	if (err)
		goto err_powerdown;

	/* Get real info about supported bands */
	b43_supported_bands(dev, &have_2ghz_phy, &have_5ghz_phy);

	/* We don't support 5 GHz on some PHYs yet */
	if (have_5ghz_phy) {
		switch (dev->phy.type) {
		case B43_PHYTYPE_G:
		case B43_PHYTYPE_LP:
		case B43_PHYTYPE_HT:
			b43warn(wl, "5 GHz band is unsupported on this PHY\n");
			have_5ghz_phy = false;
		}
	}

	if (!have_2ghz_phy && !have_5ghz_phy) {
		b43err(wl, "b43 can't support any band on this device\n");
		err = -EOPNOTSUPP;
		goto err_powerdown;
	}

	err = b43_phy_allocate(dev);
	if (err)
		goto err_powerdown;

	dev->phy.gmode = have_2ghz_phy;
	b43_wireless_core_reset(dev, dev->phy.gmode);

	err = b43_validate_chipaccess(dev);
	if (err)
		goto err_phy_free;
	err = b43_setup_bands(dev, have_2ghz_phy, have_5ghz_phy);
	if (err)
		goto err_phy_free;

	/* Now set some default "current_dev" */
	if (!wl->current_dev)
		wl->current_dev = dev;
	INIT_WORK(&dev->restart_work, b43_chip_reset);

	dev->phy.ops->switch_analog(dev, 0);
	b43_device_disable(dev, 0);
	b43_bus_may_powerdown(dev);

out:
	return err;

err_phy_free:
	b43_phy_free(dev);
err_powerdown:
	b43_bus_may_powerdown(dev);
	return err;
}

static void b43_one_core_detach(struct b43_bus_dev *dev)
{
	struct b43_wldev *wldev;

	/* Do not cancel ieee80211-workqueue based work here.
	 * See comment in b43_remove(). */

	wldev = b43_bus_get_wldev(dev);
	b43_debugfs_remove_device(wldev);
	b43_wireless_core_detach(wldev);
	list_del(&wldev->list);
	b43_bus_set_wldev(dev, NULL);
	kfree(wldev);
}

static int b43_one_core_attach(struct b43_bus_dev *dev, struct b43_wl *wl)
{
	struct b43_wldev *wldev;
	int err = -ENOMEM;

	wldev = kzalloc(sizeof(*wldev), GFP_KERNEL);
	if (!wldev)
		goto out;

	wldev->use_pio = b43_modparam_pio;
	wldev->dev = dev;
	wldev->wl = wl;
	b43_set_status(wldev, B43_STAT_UNINIT);
	wldev->bad_frames_preempt = modparam_bad_frames_preempt;
	INIT_LIST_HEAD(&wldev->list);

	err = b43_wireless_core_attach(wldev);
	if (err)
		goto err_kfree_wldev;

	b43_bus_set_wldev(dev, wldev);
	b43_debugfs_add_device(wldev);

      out:
	return err;

      err_kfree_wldev:
	kfree(wldev);
	return err;
}

#define IS_PDEV(pdev, _vendor, _device, _subvendor, _subdevice)		( \
	(pdev->vendor == PCI_VENDOR_ID_##_vendor) &&			\
	(pdev->device == _device) &&					\
	(pdev->subsystem_vendor == PCI_VENDOR_ID_##_subvendor) &&	\
	(pdev->subsystem_device == _subdevice)				)

#ifdef CONFIG_B43_SSB
static void b43_sprom_fixup(struct ssb_bus *bus)
{
	struct pci_dev *pdev;

	/* boardflags workarounds */
	if (bus->boardinfo.vendor == SSB_BOARDVENDOR_DELL &&
	    bus->chip_id == 0x4301 && bus->sprom.board_rev == 0x74)
		bus->sprom.boardflags_lo |= B43_BFL_BTCOEXIST;
	if (bus->boardinfo.vendor == PCI_VENDOR_ID_APPLE &&
	    bus->boardinfo.type == 0x4E && bus->sprom.board_rev > 0x40)
		bus->sprom.boardflags_lo |= B43_BFL_PACTRL;
	if (bus->bustype == SSB_BUSTYPE_PCI) {
		pdev = bus->host_pci;
		if (IS_PDEV(pdev, BROADCOM, 0x4318, ASUSTEK, 0x100F) ||
		    IS_PDEV(pdev, BROADCOM, 0x4320,    DELL, 0x0003) ||
		    IS_PDEV(pdev, BROADCOM, 0x4320,      HP, 0x12f8) ||
		    IS_PDEV(pdev, BROADCOM, 0x4320, LINKSYS, 0x0015) ||
		    IS_PDEV(pdev, BROADCOM, 0x4320, LINKSYS, 0x0014) ||
		    IS_PDEV(pdev, BROADCOM, 0x4320, LINKSYS, 0x0013) ||
		    IS_PDEV(pdev, BROADCOM, 0x4320, MOTOROLA, 0x7010))
			bus->sprom.boardflags_lo &= ~B43_BFL_BTCOEXIST;
	}
}

static void b43_wireless_exit(struct b43_bus_dev *dev, struct b43_wl *wl)
{
	struct ieee80211_hw *hw = wl->hw;

	ssb_set_devtypedata(dev->sdev, NULL);
	ieee80211_free_hw(hw);
}
#endif

static struct b43_wl *b43_wireless_init(struct b43_bus_dev *dev)
{
	struct ssb_sprom *sprom = dev->bus_sprom;
	struct ieee80211_hw *hw;
	struct b43_wl *wl;
	char chip_name[6];
	int queue_num;

	hw = ieee80211_alloc_hw(sizeof(*wl), &b43_hw_ops);
	if (!hw) {
		b43err(NULL, "Could not allocate ieee80211 device\n");
		return ERR_PTR(-ENOMEM);
	}
	wl = hw_to_b43_wl(hw);

	/* fill hw info */
	ieee80211_hw_set(hw, RX_INCLUDES_FCS);
	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, MFP_CAPABLE);
	hw->wiphy->interface_modes =
		BIT(NL80211_IFTYPE_AP) |
		BIT(NL80211_IFTYPE_MESH_POINT) |
		BIT(NL80211_IFTYPE_STATION) |
		BIT(NL80211_IFTYPE_ADHOC);

	hw->wiphy->flags |= WIPHY_FLAG_IBSS_RSN;

	wiphy_ext_feature_set(hw->wiphy, NL80211_EXT_FEATURE_CQM_RSSI_LIST);

	wl->hw_registered = false;
	hw->max_rates = 2;
	SET_IEEE80211_DEV(hw, dev->dev);
	if (is_valid_ether_addr(sprom->et1mac))
		SET_IEEE80211_PERM_ADDR(hw, sprom->et1mac);
	else
		SET_IEEE80211_PERM_ADDR(hw, sprom->il0mac);

	/* Initialize struct b43_wl */
	wl->hw = hw;
	mutex_init(&wl->mutex);
	spin_lock_init(&wl->hardirq_lock);
	spin_lock_init(&wl->beacon_lock);
	INIT_WORK(&wl->beacon_update_trigger, b43_beacon_update_trigger_work);
	INIT_WORK(&wl->txpower_adjust_work, b43_phy_txpower_adjust_work);
	INIT_WORK(&wl->tx_work, b43_tx_work);

	/* Initialize queues and flags. */
	for (queue_num = 0; queue_num < B43_QOS_QUEUE_NUM; queue_num++) {
		skb_queue_head_init(&wl->tx_queue[queue_num]);
		wl->tx_queue_stopped[queue_num] = false;
	}

	snprintf(chip_name, ARRAY_SIZE(chip_name),
		 (dev->chip_id > 0x9999) ? "%d" : "%04X", dev->chip_id);
	b43info(wl, "Broadcom %s WLAN found (core revision %u)\n", chip_name,
		dev->core_rev);
	return wl;
}

#ifdef CONFIG_B43_BCMA
static int b43_bcma_probe(struct bcma_device *core)
{
	struct b43_bus_dev *dev;
	struct b43_wl *wl;
	int err;

	if (!modparam_allhwsupport &&
	    (core->id.rev == 0x17 || core->id.rev == 0x18)) {
		pr_err("Support for cores revisions 0x17 and 0x18 disabled by module param allhwsupport=0. Try b43.allhwsupport=1\n");
		return -ENOTSUPP;
	}

	dev = b43_bus_dev_bcma_init(core);
	if (!dev)
		return -ENODEV;

	wl = b43_wireless_init(dev);
	if (IS_ERR(wl)) {
		err = PTR_ERR(wl);
		goto bcma_out;
	}

	err = b43_one_core_attach(dev, wl);
	if (err)
		goto bcma_err_wireless_exit;

	/* setup and start work to load firmware */
	INIT_WORK(&wl->firmware_load, b43_request_firmware);
	schedule_work(&wl->firmware_load);

	return err;

bcma_err_wireless_exit:
	ieee80211_free_hw(wl->hw);
bcma_out:
	kfree(dev);
	return err;
}

static void b43_bcma_remove(struct bcma_device *core)
{
	struct b43_wldev *wldev = bcma_get_drvdata(core);
	struct b43_bus_dev *dev = wldev->dev;
	struct b43_wl *wl = wldev->wl;

	/* We must cancel any work here before unregistering from ieee80211,
	 * as the ieee80211 unreg will destroy the workqueue. */
	cancel_work_sync(&wldev->restart_work);
	cancel_work_sync(&wl->firmware_load);

	B43_WARN_ON(!wl);
	if (!wldev->fw.ucode.data)
		return;			/* NULL if firmware never loaded */
	if (wl->current_dev == wldev && wl->hw_registered) {
		b43_leds_stop(wldev);
		ieee80211_unregister_hw(wl->hw);
	}

	b43_one_core_detach(dev);

	/* Unregister HW RNG driver */
	b43_rng_exit(wl);

	b43_leds_unregister(wl);
	ieee80211_free_hw(wl->hw);
	kfree(dev);
}

static struct bcma_driver b43_bcma_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= b43_bcma_tbl,
	.probe		= b43_bcma_probe,
	.remove		= b43_bcma_remove,
};
#endif

#ifdef CONFIG_B43_SSB
static
int b43_ssb_probe(struct ssb_device *sdev, const struct ssb_device_id *id)
{
	struct b43_bus_dev *dev;
	struct b43_wl *wl;
	int err;

	dev = b43_bus_dev_ssb_init(sdev);
	if (!dev)
		return -ENOMEM;

	wl = ssb_get_devtypedata(sdev);
	if (wl) {
		b43err(NULL, "Dual-core devices are not supported\n");
		err = -ENOTSUPP;
		goto err_ssb_kfree_dev;
	}

	b43_sprom_fixup(sdev->bus);

	wl = b43_wireless_init(dev);
	if (IS_ERR(wl)) {
		err = PTR_ERR(wl);
		goto err_ssb_kfree_dev;
	}
	ssb_set_devtypedata(sdev, wl);
	B43_WARN_ON(ssb_get_devtypedata(sdev) != wl);

	err = b43_one_core_attach(dev, wl);
	if (err)
		goto err_ssb_wireless_exit;

	/* setup and start work to load firmware */
	INIT_WORK(&wl->firmware_load, b43_request_firmware);
	schedule_work(&wl->firmware_load);

	return err;

err_ssb_wireless_exit:
	b43_wireless_exit(dev, wl);
err_ssb_kfree_dev:
	kfree(dev);
	return err;
}

static void b43_ssb_remove(struct ssb_device *sdev)
{
	struct b43_wl *wl = ssb_get_devtypedata(sdev);
	struct b43_wldev *wldev = ssb_get_drvdata(sdev);
	struct b43_bus_dev *dev = wldev->dev;

	/* We must cancel any work here before unregistering from ieee80211,
	 * as the ieee80211 unreg will destroy the workqueue. */
	cancel_work_sync(&wldev->restart_work);
	cancel_work_sync(&wl->firmware_load);

	B43_WARN_ON(!wl);
	if (!wldev->fw.ucode.data)
		return;			/* NULL if firmware never loaded */
	if (wl->current_dev == wldev && wl->hw_registered) {
		b43_leds_stop(wldev);
		ieee80211_unregister_hw(wl->hw);
	}

	b43_one_core_detach(dev);

	/* Unregister HW RNG driver */
	b43_rng_exit(wl);

	b43_leds_unregister(wl);
	b43_wireless_exit(dev, wl);
	kfree(dev);
}

static struct ssb_driver b43_ssb_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= b43_ssb_tbl,
	.probe		= b43_ssb_probe,
	.remove		= b43_ssb_remove,
};
#endif /* CONFIG_B43_SSB */

/* Perform a hardware reset. This can be called from any context. */
void b43_controller_restart(struct b43_wldev *dev, const char *reason)
{
	/* Must avoid requeueing, if we are in shutdown. */
	if (b43_status(dev) < B43_STAT_INITIALIZED) {
		b43info(dev->wl, "Controller RESET (%s) ignored, device down\n",
			reason);
		return;
	}
	b43err_restart(dev->wl, "Controller RESET (%s) ...\n", reason);
	ieee80211_queue_work(dev->wl->hw, &dev->restart_work);
}

static void b43_print_driverinfo(void)
{
	const char *feat_pci = "", *feat_nphy = "",
		   *feat_leds = "", *feat_sdio = "";

#ifdef CONFIG_B43_PCI_AUTOSELECT
	feat_pci = "P";
#endif
#ifdef CONFIG_B43_PHY_N
	feat_nphy = "N";
#endif
#ifdef CONFIG_B43_LEDS
	feat_leds = "L";
#endif
#ifdef CONFIG_B43_SDIO
	feat_sdio = "S";
#endif
	printk(KERN_INFO "Broadcom 43xx driver loaded "
	       "[ Features: %s%s%s%s ]\n",
	       feat_pci, feat_nphy, feat_leds, feat_sdio);
}

static int __init b43_init(void)
{
	int err;

	b43_debugfs_init();
	err = b43_sdio_init();
	if (err)
		goto err_dfs_exit;
#ifdef CONFIG_B43_BCMA
	err = bcma_driver_register(&b43_bcma_driver);
	if (err)
		goto err_sdio_exit;
#endif
#ifdef CONFIG_B43_SSB
	err = ssb_driver_register(&b43_ssb_driver);
	if (err)
		goto err_bcma_driver_exit;
#endif
	b43_print_driverinfo();

	return err;

#ifdef CONFIG_B43_SSB
err_bcma_driver_exit:
#endif
#ifdef CONFIG_B43_BCMA
	bcma_driver_unregister(&b43_bcma_driver);
err_sdio_exit:
#endif
	b43_sdio_exit();
err_dfs_exit:
	b43_debugfs_exit();
	return err;
}

static void __exit b43_exit(void)
{
#ifdef CONFIG_B43_SSB
	ssb_driver_unregister(&b43_ssb_driver);
#endif
#ifdef CONFIG_B43_BCMA
	bcma_driver_unregister(&b43_bcma_driver);
#endif
	b43_sdio_exit();
	b43_debugfs_exit();
}

module_init(b43_init)
module_exit(b43_exit)
