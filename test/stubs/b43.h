/* SPDX-License-Identifier: GPL-2.0
 * Minimal stub of drivers/net/wireless/broadcom/b43/b43.h for the
 * userspace test build. Only the fields and macros touched by the
 * scratch code we compile are declared; the concrete storage lives in
 * test/main.c which owns the mock device instance.
 *
 * The b43_{phy,radio}_{read,write,mask,maskset,set,read_log,force_clock}
 * accessors and b43_{read,write,write16f}16 are declared here as extern;
 * they are provided by wrap.c which emits the wl-diag trace line and
 * simulates the return value from an in-memory register mirror.
 */
#ifndef _STUB_B43_H
#define _STUB_B43_H

#include <linux/types.h>
#include <linux/kernel.h>

/* Forward decls used by the header shape. */
struct b43_wldev;
struct b43_wl;
struct b43_phy;
struct b43_phy_ac;
struct ieee80211_channel;
struct ieee80211_hw;

/* Bus discriminator; scratch code checks == B43_BUS_BCMA. */
enum b43_bus_type {
	B43_BUS_SSB  = 0,
	B43_BUS_BCMA = 1,
};

/* Bands (mac80211 style). */
enum nl80211_band {
	NL80211_BAND_2GHZ = 0,
	NL80211_BAND_5GHZ = 1,
};
enum nl80211_channel_type {
	NL80211_CHAN_NO_HT = 0,
	NL80211_CHAN_HT20,
	NL80211_CHAN_HT40MINUS,
	NL80211_CHAN_HT40PLUS,
};
#define NL80211_CHAN_WIDTH_20  0
#define NL80211_CHAN_WIDTH_40  1
#define NL80211_CHAN_WIDTH_80  2
#define NL80211_CHAN_WIDTH_160 3

/* Debug switch used inside b43_phy_read_log macros. */
#define B43_DEBUG 0

/*
 * B43_WARN_ON — mirrors drivers/net/wireless/broadcom/b43/b43.h upstream:
 * WARN_ON active when CONFIG_B43_DEBUG is set, evaluated-and-dropped
 * otherwise. Test build follows the DEBUG=0 branch, so it evaluates x
 * and drops it (keeps -Werror=unused-value quiet).
 */
#define B43_WARN_ON(x) ({ int __b43_warn = !!(x); __b43_warn; })

/*
 * Trimmed mock mac80211 chandef so scratch code can read chan/width.
 */
struct cfg80211_chan_def {
	struct ieee80211_channel *chan;
	int width;
	int center_freq1;
	int center_freq2;
};

struct ieee80211_channel {
	int band;
	u16 center_freq;
	u16 hw_value;
	u32 flags;
};

struct ieee80211_conf {
	struct cfg80211_chan_def chandef;
};

struct ieee80211_hw {
	struct ieee80211_conf conf;
};

/*
 * SROM rev 11 per-chain power info. Exactly the shape defined by
 * sprom-rev11/0001-*.patch's addition to include/linux/ssb/ssb.h —
 * keep field order/types byte-identical so this stub interoperates
 * with anything using the same offsets.
 */
struct ssb_sprom_core_pwr_info {
	u8 itssi_2g, itssi_5g;
	u8 maxpwr_2g, maxpwr_5gl, maxpwr_5g, maxpwr_5gh;
	u16 pa_2g[4], pa_5gl[4], pa_5g[4], pa_5gh[4];
	/* rev 11 additions */
	u8 maxp2ga;
	u8 maxp5ga[4];
	u16 pa2ga[3];
	u16 pa5ga[12];
};

struct ssb_sprom_rxgains {
	u8 elnagain[3];
	u8 triso[3];
	u8 trelnabyp[3];
};

/*
 * Bus-level device abstraction. On real hardware this is either
 * struct bcma_device or struct ssb_device wrapped in b43_bus_dev; here
 * we hold only the fields the scratch code reads.
 */
struct ssb_sprom {
	u8 revision;
	u8 rxchain;
	u8 subband;
	u8 subband5gver;
	struct ssb_sprom_core_pwr_info core_pwr_info[4];
	struct ssb_sprom_rxgains rxgains_2g;
	struct ssb_sprom_rxgains rxgains_5gl;
	struct ssb_sprom_rxgains rxgains_5gm;
	struct ssb_sprom_rxgains rxgains_5gh;
	/* extend on demand */
};

struct bcma_drv_cc { int _dummy; };
struct bcma_bus    { struct bcma_drv_cc drv_cc; };
struct bcma_device { struct bcma_bus *bus; };

struct b43_bus_dev {
	enum b43_bus_type bus_type;
	u16 chip_id;
	struct ssb_sprom *bus_sprom;
	struct bcma_device *bdev;   /* only meaningful when bus_type==BCMA */
};

/*
 * Radio version constants (only the bits the code compares are used).
 */
struct b43_phy {
	u8  rev;
	u16 radio_ver;
	u8  radio_rev;
	struct b43_phy_ac *ac;
	/* Some scratch code touches phy.dacbuf_cap / phy.lpf_cap directly. */
	u8 dacbuf_cap;
	u8 lpf_cap;
};

struct b43_wl {
	struct ieee80211_hw *hw;
};

struct b43_wldev {
	struct b43_bus_dev *dev;
	struct b43_wl *wl;
	struct b43_phy phy;
};

/*
 * Log macros. Route everything to stderr so it does not interleave
 * with the wl-diag trace on stdout.
 */
#define b43dbg(wl, fmt, ...)  fprintf(stderr, "b43dbg: "  fmt, ##__VA_ARGS__)
#define b43info(wl, fmt, ...) fprintf(stderr, "b43info: " fmt, ##__VA_ARGS__)
#define b43warn(wl, fmt, ...) fprintf(stderr, "b43warn: " fmt, ##__VA_ARGS__)
#define b43err(wl, fmt, ...)  fprintf(stderr, "b43err: "  fmt, ##__VA_ARGS__)

/*
 * Low-level hardware accessors -- provided by wrap.c. On real hardware
 * these live in main.c / phy_common.c.
 */
u16  b43_phy_read(struct b43_wldev *dev, u16 reg);
void b43_phy_write(struct b43_wldev *dev, u16 reg, u16 val);
void b43_phy_mask(struct b43_wldev *dev, u16 reg, u16 mask);
void b43_phy_set(struct b43_wldev *dev, u16 reg, u16 val);
void b43_phy_maskset(struct b43_wldev *dev, u16 reg, u16 mask, u16 set);
void b43_phy_force_clock(struct b43_wldev *dev, bool force);

u16  b43_radio_read(struct b43_wldev *dev, u16 reg);
void b43_radio_write(struct b43_wldev *dev, u16 reg, u16 val);
void b43_radio_mask(struct b43_wldev *dev, u16 reg, u16 mask);
void b43_radio_set(struct b43_wldev *dev, u16 reg, u16 val);
void b43_radio_maskset(struct b43_wldev *dev, u16 reg, u16 mask, u16 set);

u16  b43_read16(struct b43_wldev *dev, u16 offset);
void b43_write16(struct b43_wldev *dev, u16 offset, u16 val);
void b43_write16f(struct b43_wldev *dev, u16 offset, u16 val);

/* MAC gating -- no-op in test (state mask in b43_phy_ac tracks the bit). */
void b43_mac_enable(struct b43_wldev *dev);
void b43_mac_suspend(struct b43_wldev *dev);
void b43_mac_suspend_enable(struct b43_wldev *dev);
void b43_mac_phy_clock_set(struct b43_wldev *dev, bool on);

/*
 * b43_maccontrol_set: r/m/w del registro MAC MMIO_MACCTL (0x120).
 *   new = (old & mask) | set
 * Wrap emette `MAC.MCTRL val=<set> mask=<~mask>` — dove `mask` nel tracer
 * indica i bit toccati (bits_touched = ~b43_mask). In b43 mainline è
 * static in main.c, qui è extern per essere chiamato da phy_ac.c.
 */
void b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set);

/*
 * Master Host Feature maskset — helper AC-PHY-specifico.
 *
 * b43 mainline ha `void b43_hf_write(struct b43_wldev *dev, u64 value)`
 * che scrive 3 word MHF (SHM HOSTF1/2/3, slot 0-2). Il blob vendor AC-PHY
 * usa 5 slot (0-4) e ha una semantica maskset atomica visibile al tracer
 * WL-diag come `MAC.MHF addr=<slot> val=<val> mask=<mask>`.
 *
 * L'implementazione reale è in src/helpers_phy_ac.c e fa r/m/w SHM sui
 * registri B43_SHM_SH_HOSTFn (tutti già definiti nel kernel b43.h). Nel
 * test env il --wrap del linker intercetta la chiamata prima che raggiunga
 * l'implementazione.
 *
 * slot: 0..4  (0 = HOSTF1, 4 = HOSTF5)
 * mask: bit da preservare (~mask sono i bit modificati)
 * val:  valore dei bit modificati
 */
void b43_phy_ac_mhf_maskset(struct b43_wldev *dev,
			    u16 slot, u16 mask, u16 val);

/* Generic switch-analog helper called by ops. */
void b43_phyop_switch_analog_generic(struct b43_wldev *dev, bool on);

/* Band query. Backed by a mutable global in wrap.c. */
enum nl80211_band b43_current_band(struct b43_wl *wl);

/* Generic PHY init (used by phy_common as a fallback). */
int b43_phy_init(struct b43_wldev *dev);

/* Kernel helper used by chan-def code. */
static inline enum nl80211_channel_type
cfg80211_get_chandef_type(const struct cfg80211_chan_def *c)
{
	(void)c;
	return NL80211_CHAN_NO_HT;
}

/*
 * bcma chipcommon accessors used by phy_ac op_init / op_software_rfkill /
 * set_channel. Real bodies live in drivers/bcma/driver_chipcommon*.c.
 * In the test build we provide no-op logging stubs in wrap.c that record
 * the call in the trace stream and return 0.
 */
u32 bcma_chipco_gpio_out(struct bcma_drv_cc *cc, u32 mask, u32 value);
u32 bcma_chipco_gpio_outen(struct bcma_drv_cc *cc, u32 mask, u32 value);
u32 bcma_chipco_gpio_control(struct bcma_drv_cc *cc, u32 mask, u32 value);
#ifndef BCMA_CC_PMU_PLL_CTL3
#define BCMA_CC_PMU_PLL_CTL3 3
#endif
u32 bcma_chipco_pll_read(struct bcma_drv_cc *cc, u32 offset);
void b43_test_pll_set(u32 offset, u32 val);
void bcma_chipco_regctl_maskset(struct bcma_drv_cc *cc, u32 offset,
				u32 mask, u32 set);

/* MMIO offsets touched via b43_write16/b43_read16 in scratch. */
#define B43_MMIO_PHY_CONTROL      0x3FC
#define B43_MMIO_PHY_DATA         0x3FE
#define B43_MMIO_RADIO24_CONTROL  0x1F8
#define B43_MMIO_RADIO24_DATA     0x1FA

#endif /* _STUB_B43_H */
