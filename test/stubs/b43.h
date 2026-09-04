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
/*
 * Same order and values as include/uapi/linux/nl80211.h. The previous stub
 * numbered these from zero without NL80211_CHAN_WIDTH_20_NOHT, so every
 * value was one below the real one -- harmless while both sides used the
 * stub, wrong the moment the driver source is read against upstream.
 */
enum nl80211_chan_width {
	NL80211_CHAN_WIDTH_20_NOHT,
	NL80211_CHAN_WIDTH_20,
	NL80211_CHAN_WIDTH_40,
	NL80211_CHAN_WIDTH_80,
	NL80211_CHAN_WIDTH_80P80,
	NL80211_CHAN_WIDTH_160,
};

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
	enum nl80211_chan_width width;
	int center_freq1;
	int center_freq2;
};

struct ieee80211_channel {
	int band;
	u16 center_freq;
	u16 hw_value;
	/*
	 * Regulatory ceiling for this channel, in whole dBm, as cfg80211
	 * fills it from the wiphy's regulatory domain. The TX power
	 * derivation consumes it; see b43_phy_ac_txpwr_target().
	 */
	int max_power;
	u32 flags;
};

struct ieee80211_conf {
	struct cfg80211_chan_def chandef;
};

struct wiphy;

struct ieee80211_hw {
	struct ieee80211_conf conf;
	struct wiphy *wiphy;
};

/*
 * Per-channel regulatory lookup. Upstream this walks the wiphy's band
 * definitions; the harness scripts a small table so the per-sub-channel path
 * can be exercised without a regulatory core.
 */
struct ieee80211_channel *ieee80211_get_channel(struct wiphy *wiphy, int freq);
void b43_test_reg_init(int dflt, const char *map);
/*
 * Hook del vendor su wlc_bmac_bw_set. NON e' una scrittura di registro: il suo
 * equivalente GPL in brcmsmac, brcms_b_bw_set(), fa `pi->bw = bw` piu' un reset
 * e un init del PHY, e nelle catture fra il record e il prologo radio non c'e'
 * nessuna scrittura. In b43 la larghezza sta in phy.chandef e la imposta
 * b43_phy_init(): non c'e' niente da chiamare, e nessun codice del port emette
 * questa op. Vedi SOLO_VENDOR in test/compare.py.
 *
 * Il prototipo resta per non rompere link di scratch che lo usassero.
 */
void b43_mac_bw_set(struct b43_wldev *dev, u32 bw);
void b43_test_oracle_coverage_report(void);

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

/*
 * Rev-11 per-rate power offsets, as added to struct ssb_sprom by
 * patches/0001-ssb-bcma-add-SPROM-revision-11-extraction.patch. Four u32 per
 * band in the real layout (20/40/80/160 MHz); only the two the TX power
 * derivation consumes are mirrored here.
 */
struct ssb_sprom_mcsbw_po {
	u32 bw20;
	u32 bw40;
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
	/* Per-band rev-11 power offsets: index 0 = 5gl, 1 = 5gm, 2 = 5gh. */
	struct ssb_sprom_mcsbw_po mcsbw5g_po[3];
	/* Antenna gain per band, quarter-dB, SROM ag0/ag1. Index 1 is 5 GHz. */
	s8 antenna_gain_qdb[2];

	u8 revision;
	u8 rxchain;
	u8 subband;
	u8 subband5gver;
	/* Blocco FEM/PA (SROM11_FEM_CFG1/2), decodificato come in bcma. */
	u8 tssiposslope2g, epagain2g, pdgain2g, tworangetssi2g, papdcap2g, femctrl;
	u8 tssiposslope5g, epagain5g, pdgain5g, tworangetssi5g, papdcap5g, gainctrlsph;
	u16 tssifloor2g;
	u16 tssifloor5g[4];
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
	/*
	 * Larghezza e canale correnti. In b43 li punta b43_phy_init() alla
	 * config dell'hardware, prima di switch_analog() e di
	 * b43_software_rfkill(), e b43_op_config() li ripunta a ogni cambio di
	 * canale; b43_is_40mhz() legge da qui. L'harness lo fa in mount_board(),
	 * cosi' il codice del PHY vede lo stesso oggetto.
	 */
	const struct cfg80211_chan_def *chandef;
	u8  rev;
	u16 radio_ver;
	u8  radio_rev;
	struct b43_phy_ac *ac;
	/* Some scratch code touches phy.dacbuf_cap / phy.lpf_cap directly. */
	u8 dacbuf_cap;
	u8 lpf_cap;
	/*
	 * Primo bring-up contro bring-up successivo. In-tree lo gestisce
	 * b43_phy_init(): true fino al primo ops->init riuscito. Qui lo imposta
	 * il flow, cosi' lo stesso codice si misura contro una cattura attach
	 * o contro una down->up.
	 */
	bool do_full_init;
};

struct b43_wl {
	struct ieee80211_hw *hw;
};

struct b43_wldev {
	struct b43_bus_dev *dev;
	/* Come in-tree: profondita' di annidamento di b43_mac_suspend. */
	int mac_suspended;
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

/* SHM (ucode shared memory) -- kernel: b43/main.h; il tracer li emette
 * come OBJ.RD/OBJ.WR (implementazione in wrap.c). */
#define B43_SHM_SHARED 0x0001
u16  b43_shm_read16(struct b43_wldev *dev, u16 routing, u16 offset);
void b43_shm_write16(struct b43_wldev *dev, u16 routing, u16 offset, u16 val);

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

/*
 * Celle di shared memory delle cinque word di host flag, e MACCTL: valori del
 * b43.h del kernel, serviti qui perche' helpers_phy_ac.c ora e' nel link (le
 * sue decisioni, non solo la sua etichetta, sono cio' che il confronto
 * verifica).
 */
#define B43_SHM_SH_HOSTF1         0x005E
#define B43_SHM_SH_HOSTF2         0x0060
#define B43_SHM_SH_HOSTF3         0x0062
#define B43_SHM_SH_HOSTF4         0x0078
#define B43_SHM_SH_HOSTF5         0x00D4
#define B43_SHM_SH_RFATT          0x0064
#define B43_MMIO_MACCTL           0x120

void b43_maskset32(struct b43_wldev *dev, u16 offset, u32 mask, u32 set);

#endif /* _STUB_B43_H */
