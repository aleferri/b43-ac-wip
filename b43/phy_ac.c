// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver IEEE 802.11ac AC-PHY support Copyright (c) 2015
 * Rafał Miłecki <zajec5@gmail.com>
 */

/*
 * Transcription conventions for the whole file:
 *
 * - b43_phy_maskset() and b43_radio_maskset() are used even for a plain OR or
 *   AND: the stock driver emits PHY.MOD with an explicit mask, which
 *   phy_set() and phy_mask() would emit as mask=0x0000. A pulse is therefore
 *   a pair of masksets: val=<bit> mask=<bit>, then val=0 mask=<bit>.
 * - reads whose value is not needed go through b43_phy_read_log() or
 *   b43_radio_read_log(): they keep the bus order, and a read with no
 *   consumer may be a logic error, so it is logged rather than hidden. A few
 *   still use the plain accessor.
 * - "self-contained" is a table write that opens the gate, writes and closes
 *   it in its own scope (b43_actab_*_scoped), as opposed to the variants that
 *   work inside a gate the caller holds open.
 * - "the observed order" means the order comes from a capture and must not
 *   be rearranged; the op-for-op comparison checks it.
 *
 * The #NNNNN indices are episode positions in a reference capture. The index
 * spaces of the captures overlap, so a bare index can resolve to several ops:
 * check it with `reverse-tools/anchors.py span` before relying on it.
 */

#include <linux/slab.h>
#include "b43.h"
#include "phy_ac.h"
#include "phy_common.h"
#include "tables_phy_ac.h"
#include "radio_2069.h"
#include "main.h"

/* The 2.4 GHz channel set is incomplete: the band is refused unless the
 * build sets ALLOW_24. */
#ifndef ALLOW_24
#define ALLOW_24 false
#endif

/* Basic PHY ops */

static int b43_phy_ac_op_allocate(struct b43_wldev *dev)
{
	struct b43_phy_ac *phy_ac;

	phy_ac = kzalloc_obj(*phy_ac);
	if (!phy_ac)
		return -ENOMEM;
	dev->phy.ac = phy_ac;

	return 0;
}

static void b43_phy_ac_op_free(struct b43_wldev *dev)
{
	struct b43_phy *phy = &dev->phy;
	struct b43_phy_ac *phy_ac = phy->ac;

	kfree(phy_ac);
	phy->ac = NULL;
}

static void b43_phy_ac_op_maskset(struct b43_wldev *dev, u16 reg, u16 mask,
				  u16 set)
{
	b43_write16f(dev, B43_MMIO_PHY_CONTROL, reg);
	b43_write16(dev, B43_MMIO_PHY_DATA,
		    (b43_read16(dev, B43_MMIO_PHY_DATA) & mask) | set);
}

static u16 b43_phy_ac_op_radio_read(struct b43_wldev *dev, u16 reg)
{
	b43_write16f(dev, B43_MMIO_RADIO24_CONTROL, reg);
	return b43_read16(dev, B43_MMIO_RADIO24_DATA);
}

static void b43_phy_ac_op_radio_write(struct b43_wldev *dev, u16 reg,
				      u16 value)
{
	b43_write16f(dev, B43_MMIO_RADIO24_CONTROL, reg);
	b43_write16(dev, B43_MMIO_RADIO24_DATA, value);
}

static unsigned int b43_phy_ac_op_get_default_chan(struct b43_wldev *dev)
{
	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ)
		return 11;
	return 36;
}

static void b43_phy_ac_txpwrctrl_setup(struct b43_wldev *dev, u16 freq);

/*
 * The periodic work, split the way the b43 core splits it.
 *
 * 1. recalc_txpower / adjust_txpower are the TX power target: the core calls
 *    recalc from b43_op_config() and every minute, and adjust writes the
 *    target to the PHY only when recalc says it moved, as the vendor and
 *    b43_nphy_op_recalc_txpower() do.
 *    The computation is b43_phy_ac_txpwr_recalc(). The channel setup computes
 *    the same target before its own two write sites, so the core's calls
 *    change nothing unless the regulatory ceiling did.
 *
 * 2. CRS min power is the pwork_60sec hook. The high byte of 0x0324 and
 *    friends is reset once in op_switch_channel; the low byte is the
 *    threshold recalculated by the crsmin chain (ladder, per-bandwidth
 *    anchoring, clamp and cold bump, verified against the d6220 7.14 blob)
 *    from the per-chain ring the watchdog fills from the noise window.
 *
 * 3. The periodic cycle on 0x0725/0x0925 is the tempsense in
 *    b43_phy_ac_watchdog(), from the pwork_1sec hook, every temps_period
 *    turns.
 */
static enum b43_txpwr_result
b43_phy_ac_op_recalc_txpower(struct b43_wldev *dev, bool ignore_tssi)
{
	B43_AC_FN();
	bool recomputed = b43_phy_ac_txpwr_recalc(dev);

	if (!recomputed && !dev->phy.ac->txpwr_adjust_due)
		return B43_TXPWR_RES_DONE;

	return B43_TXPWR_RES_NEED_ADJUST;
}

static void b43_phy_ac_txpwr_adjust(struct b43_wldev *dev);

/*
 * Runs from the TX power work with the MAC enabled, and the tables it
 * programs want it suspended. The stock driver's BSS mode block that
 * precedes it -- TBTT hold, AP and INFRA, PRETBTT, beacon promiscuity -- is
 * b43_adjust_opmode(), b43_set_beacon_int() and b43_set_pretbtt() in main.c.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   13665-14082]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9295-9714]
 */
static void b43_phy_ac_op_adjust_txpower(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_mac_suspend(dev);
	b43_phy_ac_txpwr_adjust(dev);
	b43_mac_enable(dev);
	dev->phy.ac->txpwr_adjust_due = false;
}

/*
 * prepare_structs is called by the b43 core after allocate and before init.
 */
static void b43_phy_ac_op_prepare_structs(struct b43_wldev *dev)
{
	struct b43_phy_ac *phy_ac = dev->phy.ac;
	u16 mhfs[ARRAY_SIZE(phy_ac->mhfs)];
	bool writethrough, attached;
	u16 pmu_req;

	/*
	 * b43_wireless_core_init() calls this on every ifconfig up, but some
	 * state must survive a down/up and be reset only by a module reload:
	 *
	 * - the host-flag shadow: a warm cycle flushes the values the previous
	 *   cycle accumulated (0x0060 in HOSTF4, 0x0088 in HOSTF5) where a cold
	 *   one flushes 0x0040 and 0x0080;
	 * - the attach part of the cold preamble, which runs once per probe;
	 * - the PMU request that preamble leaves raised until the first
	 *   bring-up, since no reset lowers the chipcommon bit.
	 *
	 * All three are zeroed by the kzalloc in op_allocate().
	 */
	memcpy(mhfs, phy_ac->mhfs, sizeof(mhfs));
	writethrough = phy_ac->mhf_writethrough;
	attached = phy_ac->attach_preamble_done;
	pmu_req = phy_ac->status_mask & B43_PHY_AC_STATE_PMU_REQ;

	memset(phy_ac, 0, sizeof(*phy_ac));

	memcpy(phy_ac->mhfs, mhfs, sizeof(mhfs));
	phy_ac->mhf_writethrough = writethrough;
	phy_ac->attach_preamble_done = attached;
	phy_ac->status_mask = pmu_req;
}

/*
 * The two AFE power banks. OFF is the 0x173e..0x1720 bank with 0x1721=0xffff,
 * 0x1725=0x1fff and 0x1720=0x03ff, every AFE block powered down: it closes
 * every `wl down` and opens the attach, twice. ON is the four-write bank
 * with 0x1721=0x5000 and 0x1720=0x0180: the front end powering up for the
 * channel switch.
 */
enum b43_phy_ac_afe_mode {
	B43_PHY_AC_AFE_ON,	/* front-end powered, ready for the switch */
	B43_PHY_AC_AFE_OFF,	/* front-end powered down: attach and wl down */
};

static void b43_phy_ac_enable_afe(struct b43_wldev *dev,
				  enum b43_phy_ac_afe_mode mode);
static void b43_phy_ac_down(struct b43_wldev *dev);

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   1252-1271]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   618-637]
 */
static void b43_phy_ac_mode_init(struct b43_wldev *dev)
{
	B43_AC_FN();

	b43_phy_write(dev, 0x0410, 0x0077);

	/* 0x17xx-page clears */
	b43_phy_write(dev, 0x173e, 0x0000);
	b43_phy_write(dev, 0x1725, 0x0000);
	b43_phy_write(dev, 0x1722, 0x0000);
	b43_phy_write(dev, 0x1723, 0x0000);
	b43_phy_write(dev, 0x1724, 0x0000);
	b43_phy_write(dev, 0x1725, 0x0000);
	b43_phy_write(dev, 0x1726, 0x0000);
	b43_phy_write(dev, 0x1727, 0x0000);
	b43_phy_write(dev, 0x1750, 0x0000);

	/*
	 * Through the helper rather than inline, so that @status_mask follows
	 * the hardware: b43_phy_ac_channel_setup() requires the shadow to match
	 * the registers.
	 */
	b43_phy_ac_enable_afe(dev, B43_PHY_AC_AFE_ON);

	/*
	 * RMW pairs: read the base-page register, OR in the bit, write the
	 * 0x17xx mirror, each read right before its write as in the stock
	 * driver. Both base values read 0 on the reference board.
	 */
	b43_phy_write(dev, 0x173a, b43_phy_read_log(dev, 0x073a) | 0x0100);
	b43_phy_write(dev, 0x1725, b43_phy_read_log(dev, B43_PHY_AC_AFE_C1) | 0x0400);
}

/*
 * Initial ADC gain words. Both the value pair and the number of passes
 * follow the bring-up phase, not the chip. adc_reset() later rewrites
 * 0x03ac and 0x032c.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   4988-5006]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   638-676]
 */
static void b43_phy_ac_init_regs(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const u16 hi_regs[8] = { 0x033a, 0x033b, 0x033e, 0x033f,
					0x0342, 0x0343, 0x0346, 0x0347 };
	static const u16 lo_regs[8] = { 0x033c, 0x033d, 0x0340, 0x0341,
					0x0344, 0x0345, 0x0348, 0x0349 };
	/*
	 * First bring-up: one pass with 0x097a/0x08fa. Later ones: two passes
	 * with 0x03bf/0x0340. Both seen on the d6220 and on agcombo.
	 *
	 * TODO: the DSL-3580L, on wl 6.30, writes 0x0395/0x0315 and no 0x1645.
	 */
	u16 saved;
	bool first = dev->phy.do_full_init;
	bool two_pass = !first;
	u16 hi = first ? 0x097a : 0x03bf;
	u16 lo = first ? 0x08fa : 0x0340;
	unsigned int pass, passes = two_pass ? 2 : 1;
	unsigned int i;

	/*
	 * On a warm cycle init_regs runs before tables_init and cycles the
	 * table-write gate itself (0x019e bit 1 set, then cleared). On a first
	 * bring-up tables_init has already run and released it.
	 */
	if (!first) {
		saved = b43_phy_ac_tbl_write_lock(dev);
		b43_phy_ac_tbl_write_unlock(dev, saved);
	}

	b43_phy_write(dev, 0x1645, 0x025c);

	for (pass = 0; pass < passes; pass++) {
		for (i = 0; i < 8; i++)
			b43_phy_write(dev, hi_regs[i], hi);
		for (i = 0; i < 8; i++)
			b43_phy_write(dev, lo_regs[i], lo);
	}
}

/* 5 GHz pa5ga subband group (0..3) for a channel freq, per subband5gver. */
static unsigned int b43_phy_ac_pa5g_group(struct b43_wldev *dev, u16 freq)
{
	switch (dev->dev->bus_sprom->subband5gver) {
	case 4:
		if (freq < 5250)
			return 0;
		if (freq < 5500)
			return 1;
		if (freq > 5744)
			return 3;
		return 2;
	case 1:
		if (freq < 5250)
			return 0;
		if (freq > 5744)
			return 2;
		return 1;
	case 0:
		if (freq < 5500)
			return 0;
		if (freq < 5745)
			return 1;
		return 2;
	default:
		if (freq < 5100)
			return 0;
		if (freq > 5499)
			return 2;
		return 1;
	}
}

static void b43_phy_ac_clip_det(struct b43_wldev *dev, bool enable);
static void b43_phy_ac_cca_pulse(struct b43_wldev *dev);
static void b43_phy_ac_rxgain_perchan_tail(struct b43_wldev *dev);
static void b43_phy_ac_iq_acc_peek(struct b43_wldev *dev, unsigned int core,
				   bool measurement);
static void b43_phy_ac_loopback_gain_search(struct b43_wldev *dev);
static void b43_phy_ac_pmu_req(struct b43_wldev *dev, bool on);
static void b43_phy_ac_wd_stats_clear(struct b43_wldev *dev);
static void b43_phy_ac_noise_sample_request(struct b43_wldev *dev);
static void b43_phy_ac_radio_percore_setup_1(struct b43_wldev *dev);
static void b43_phy_ac_tx_gain_bbmult_load(struct b43_wldev *dev);
static bool b43_phy_ac_may_calibrate_tx(struct b43_wldev *dev);
static void b43_phy_ac_radar_thresh(struct b43_wldev *dev);

/*
 * Warn once per call site that the driver writes something it cannot
 * derive, and what may go wrong on hardware other than the one the value
 * was measured on.
 *
 * Once, because some sites sit in a calibration loop and would flood the
 * log; per site, because which site was hit is the useful information.
 * Not B43_WARN_ON(): these are reached by design, not by a broken state.
 */
#define b43_phy_ac_todo(dev, fmt, ...)					\
	do {								\
		static bool __ac_todo_said;				\
									\
		if (!__ac_todo_said) {					\
			__ac_todo_said = true;				\
			b43warn((dev)->wl, "AC-PHY: " fmt, ##__VA_ARGS__); \
		}							\
	} while (0)

static void b43_phy_ac_post_bringup_tail(struct b43_wldev *dev);
static void b43_phy_ac_crs_block_e(struct b43_wldev *dev);
static void b43_phy_ac_wd_stats_tail(struct b43_wldev *dev);
static void b43_phy_ac_tempsense(struct b43_wldev *dev);
static bool b43_phy_ac_cal_reads_temp(struct b43_wldev *dev);
static bool b43_phy_ac_rfseq_wait_done(struct b43_wldev *dev, u16 busy,
				       unsigned int turns);
static void b43_phy_ac_run_samples(struct b43_wldev *dev, u16 nsamp,
				   bool iqmode);
/*
 * The four scattered cells that open the watchdog sweep, and the flat sweep
 * of 0x0768-0x078a. They are two functions because on entering the probe
 * phase the phase reads and the mode change fall between them.
 *
 * The four come in two orders, chosen by the turn: on entering the phase
 * `0x010e 0x010c 0x0158 0x015e`, in steady state `0x010e 0x0158 0x010c
 * 0x015e`. Over 87 segments the first order appears 45 times, always
 * followed by the peek, the second 4639 times and never followed by it, so
 * the order marks the path and is not capture noise.
 */
static void b43_phy_ac_wd_head_words(struct b43_wldev *dev, bool entry)
{
	static const u16 head_entry[4] = { 0x010e, 0x010c, 0x0158, 0x015e };
	static const u16 head_steady[4] = { 0x010e, 0x0158, 0x010c, 0x015e };
	const u16 *head = entry ? head_entry : head_steady;
	unsigned int i;

	for (i = 0; i < 4; i++)
		b43_shm_read16(dev, B43_SHM_SHARED, head[i]);
}

static void b43_phy_ac_wd_flat_sweep(struct b43_wldev *dev)
{
	u16 off;

	for (off = 0x0768; off <= 0x078a; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
}

static void b43_phy_ac_wd_stats_poll_opt(struct b43_wldev *dev,
					 bool head_sweep,
					 unsigned int ctr32_passes,
					 bool ctr32_tail);
static void b43_phy_ac_txpwr_target_write(struct b43_wldev *dev);
static void b43_phy_ac_farrow_setup(struct b43_wldev *dev,
				    struct ieee80211_channel *channel);

/*
 * Per-active-core ADC hold bracket, and the only helper that touches
 * 0x02ed/f1/f5/f9. The stock driver always emits the same four MODs, in this
 * order, on bit 0x0010:
 *   hold = true  sets it: ADC hold, i.e. RX release
 *   hold = false clears it: RX arm
 * Keep it a call: inlining it breaks the op order.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5014-5017, 11403-11406, 11436-11439, 12186-12189, 14971-14974,
 *   15721-15724, 15747-15750, 16497-16500, 16535-16538, 22896-22899,
 *   22937-22940, 27686-27689, 27700-27703, 30648-30651]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   684-687, 7075-7078, 7124-7127, 7874-7877, 10385-10388, 11135-11138,
 *   11161-11164, 11911-11914, 11949-11952, 18024-18027, 18065-18068,
 *   22898-22901, 22912-22915, 25930-25933]
 */
static void b43_phy_ac_adc_hold(struct b43_wldev *dev, bool hold)
{
	B43_AC_FN();
	u16 set = hold ? 0x0010 : 0x0000;

	b43_phy_maskset(dev, 0x02ed, (u16)~0x0010, set);
	b43_phy_maskset(dev, 0x02f1, (u16)~0x0010, set);
	b43_phy_maskset(dev, 0x02f5, (u16)~0x0010, set);
	b43_phy_maskset(dev, 0x02f9, (u16)~0x0010, set);
}

/*
 * Bandwidth step: 0 at 20 MHz, 1 at 40, 2 at 80.
 *
 * Several registers move by a fixed amount per step. Each user says which
 * law it follows: the law is fitted to three measured points and is a
 * compact way of writing them, not a prediction. There is no 160 MHz
 * capture.
 */
static unsigned int b43_phy_ac_bw_step(struct b43_wldev *dev)
{
	switch (dev->phy.ac->cal_width) {
	case NL80211_CHAN_WIDTH_80:
		return 2;
	case NL80211_CHAN_WIDTH_40:
		return 1;
	default:
		return 0;
	}
}

/*
 * The 0x0463 word of the RX-IQ correlator kick: 0x27 at 20 MHz, 0x4f at 40,
 * 0x9f at 80. The value plus one doubles with the bandwidth, i.e. a sample
 * count over a fixed time.
 */
static u16 b43_phy_ac_rxiqcal_kick_len(struct b43_wldev *dev)
{
	return (u16)(0x28 * (1u << b43_phy_ac_bw_step(dev)) - 1);
}

/*
 * Load a tone table into table 0x000e, the PHY sample-play buffer the RX-IQ
 * cal plays through the B43_PHY_AC_SAMP_PLAY_* block, in loopback.
 *
 * Every tone has a period of 20 entries, and the table length is two
 * entries per MHz of channel width: 40 at 20 MHz, 80 at 40, 160 at 80
 * (d6220 cold sweep, every segment of each width).
 *
 * The stock driver loads it as a single bulk write, so the tiled table goes
 * out in one call rather than one per period.
 *
 * Callers pass the 20-entry period; the tiling and the length belong here.
 */
#define B43_PHY_AC_TONE_PERIOD		20
#define B43_PHY_AC_TONE_MAX_ENTRIES	(B43_PHY_AC_TONE_PERIOD << 3)

static void b43_phy_ac_tone_table_write(struct b43_wldev *dev,
					const u32 *period, int step)
{
	u32 tone[B43_PHY_AC_TONE_MAX_ENTRIES];
	unsigned int n, i;

	n = B43_PHY_AC_TONE_PERIOD << (b43_phy_ac_bw_step(dev) + 1);
	if (B43_WARN_ON(n > ARRAY_SIZE(tone)))
		return;

	for (i = 0; i < n; i++) {
		int k = ((int)i * step) % (int)B43_PHY_AC_TONE_PERIOD;

		if (k < 0)
			k += B43_PHY_AC_TONE_PERIOD;
		tone[i] = period[k];
	}

	b43_actab_write_bulk_scoped(dev, 0x000e, 0x0000, 32, n, tone);
}

/*
 * The tone period the RX-IQ measurement uses, and its steps.
 *
 * The stock driver's "second" and "third" tones are this period at steps +1
 * and -1; at 80 MHz it loads four more, at +3, -3, +4 and -4 (verified on
 * the eleven 160-entry loads of cold24).
 *
 * The step is the tone frequency in units of rate/20: one pair of tones
 * around the carrier at 20 and 40 MHz, three pairs at 80.
 */
static const u32 b43_phy_ac_tone_period[B43_PHY_AC_TONE_PERIOD] = {
	0x0002d400, 0x0002b038, 0x0002486a, 0x0001a892, 0x0000e0ac,
	0x000000b5, 0x000f20ac, 0x000e5892, 0x000db86a, 0x000d5038,
	0x000d2c00, 0x000d53c8, 0x000dbb96, 0x000e5b6e, 0x000f2354,
	0x0000034b, 0x0000e354, 0x0001ab6e, 0x00024b96, 0x0002b3c8,
};

static const int b43_phy_ac_tone_steps[] = { 1, -1, 3, -3, 4, -4 };

/* Measurement passes: two up to 40 MHz, six at 80. */
static unsigned int b43_phy_ac_meas_passes(struct b43_wldev *dev)
{
	unsigned int n = b43_phy_ac_bw_step(dev) == 2 ? 6 : 2;

	return n < ARRAY_SIZE(b43_phy_ac_tone_steps)
	       ? n : ARRAY_SIZE(b43_phy_ac_tone_steps);
}

/*
 * The per-rate blocks in shared memory, and how to reach one of their fields.
 *
 * The address is read, not computed: the rate's pointer from the direct-map
 * table, doubled, as brcmsmac's brcms_b_rate_shm_offset():
 *
 *     block = 2 * shm_read(M_RT_DIRMAP_A + index * 2)
 *
 * The index is the low nibble of the PLCP SIGNAL field, so 6, 9, 12, 18, 24,
 * 36, 48 and 54 Mbit/s are at indices 11, 15, 10, 14, 9, 13, 8 and 12.
 *
 * The blocks happen to be 0x14 apart on this board, but the rate table is
 * driver state, not ucode state: the DSL-3580L has its pointers at
 * 0x49e-0x4e4 where the d6220 has them at 0x4c6-0x50c. A fixed stride would
 * hit the right cells only for one rateset, and no gate would notice.
 *
 * Offsets within a block are in bytes, like brcmsmac's M_RT_*.
 */
#define B43_AC_RT_DIRMAP_A	0x01c0		/* M_RT_DIRMAP_A, 0xe0 * 2 */
#define B43_AC_RT_DIRMAP_B	0x0200		/* M_RT_DIRMAP_B, 0x100 * 2 */
#define B43_AC_RT_BBRSMAP_A	0x01e0		/* M_RT_BBRSMAP_A, 0xf0 * 2 */
/*
 * The block layout differs from brcmsmac's for some fields: SIGNAL is at +8
 * and +10 and the duration at +12, where brcmsmac has PLCP_POS 10 and
 * PRS_DUR_POS 16 (decoding SIGNAL settles it: at +10 the rate nibble reads
 * zero). OFDM_PCTL1 is at 18 in both; brcmsmac has nothing at +14.
 */
#define B43_AC_RT_PLCP		8		/* SIGNAL at +8/+10, duration at +12 */
#define B43_AC_RT_RATE_PO	14		/* mcsbw*po nibble, << 3 */
#define B43_AC_RT_OFDM_PCTL1	18		/* M_RT_OFDM_PCTL1_POS */

static const u8 b43_phy_ac_ofdm_dirmap[8] = { 11, 15, 10, 14, 9, 13, 8, 12 };

static u16 b43_phy_ac_rate_shm_offset(struct b43_wldev *dev, unsigned int rate)
{
	u16 ptr = b43_shm_read16(dev, B43_SHM_SHARED, B43_AC_RT_DIRMAP_A +
				 b43_phy_ac_ofdm_dirmap[rate] * 2);

	return (u16)(2 * ptr);
}

/*
 * Read and rewrite OFDM_PCTL1 over the eight OFDM rate blocks.
 *
 * This is brcmsmac's brcms_upd_ofdm_pctl1_table(): it reads
 * entry_ptr + M_RT_OFDM_PCTL1_POS, puts the STF mode bits back and rewrites.
 * On one chain hw_stf_ss_opmode is zero, so the value read goes straight
 * back.
 *
 * The stock driver runs it twice per attach: in the MAC config block, and
 * after the MHF3 write in channel_setup().
 */
static void b43_phy_ac_ofdm_pctl1_readback(struct b43_wldev *dev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_ofdm_dirmap); i++) {
		u16 cell = b43_phy_ac_rate_shm_offset(dev, i) +
			   B43_AC_RT_OFDM_PCTL1;
		u16 cur = b43_shm_read16(dev, B43_SHM_SHARED, cell);

		b43_shm_write16(dev, B43_SHM_SHARED, cell, cur);
	}
}

/*
 * Shared-memory block between the PHY write of 0x0339 and the host flag that
 * follows: CW and slot time, PHYTYPE/PHYVER, and the OFDM_PCTL1 read-back.
 *
 * Nothing written here is transcribed. The read-back values are constant
 * (0x44, 0x3c, 0x34, 0x30, 0x2c, 0x2c, 0x28, 0x28 on two D6220 channels and
 * on a BCM4360), so they are microcode defaults; why the stock driver
 * touches them is not known.
 *
 * These are MAC cells and belong in the core, but the core has no hook at
 * this point; see docs/retrace-todo.md.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   12194-12240]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7882-7928]
 */
static void b43_phy_ac_shm_readback_block(struct b43_wldev *dev)
{
	B43_AC_FN();

	/*
	 * TODO 0x0092: read, never written, not named in b43.h. It reads 0xacc
	 * on both boards, so it is published by the microcode; nothing consumes
	 * it. It is also read once during core init.
	 *
	 * Then CWmin/CWmax in scratch memory and the slot time in shared
	 * memory. b43's core init writes all three too; the captures put them
	 * here. (The wl-diag captures print scratch words at four times their
	 * index, so CWmin and CWmax appear there as 0x000c and 0x0010.)
	 */
	b43_shm_read16_log(dev, B43_SHM_SHARED, 0x0092);
	b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_MINCONT, 0x000f);
	b43_shm_write16(dev, B43_SHM_SCRATCH, B43_SHM_SC_MAXCONT, 0x03ff);
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_SLOTT, 9);

	/*
	 * PHYTYPE and PHYVER, which the ucode reads to know which PHY it
	 * drives. b43_wireless_core_init() writes them too, from phy->type and
	 * phy->rev; the captures put them here.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0052, dev->phy.type);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0050, dev->phy.rev);

	b43_phy_ac_ofdm_pctl1_readback(dev);
}

/*
 * The twelve rates of the rateset in increasing bitrate, the order the stock
 * driver visits the blocks, CCK interleaved with OFDM. @dirmap is the index
 * in the direct-map table (the SIGNAL low nibble), @ofdm the legacy rate
 * index for b43_ppr_ac_ofdm(), 0 = 6 Mbit/s to 7 = 54.
 */
struct b43_phy_ac_prb_rsp_rate {
	u8 dirmap;
	u8 ofdm;
	bool cck;
};

static const struct b43_phy_ac_prb_rsp_rate b43_phy_ac_prb_rsp_rates[12] = {
	{ 10, 0, true  },	/*  1 Mbit/s */
	{  4, 0, true  },	/*  2 */
	{  7, 0, true  },	/*  5.5 */
	{ 11, 0, false },	/*  6 */
	{ 15, 1, false },	/*  9 */
	{ 14, 0, true  },	/* 11 */
	{ 10, 2, false },	/* 12 */
	{ 14, 3, false },	/* 18 */
	{  9, 4, false },	/* 24 */
	{ 13, 5, false },	/* 36 */
	{  8, 6, false },	/* 48 */
	{ 12, 7, false },	/* 54 */
};

/*
 * The field for the CCK rates, the cck[4] group of the PPR.
 *
 * Rev 11 has no per-rate field for them on 5 GHz (cckbw202gpo is the 2.4 GHz
 * one and reads zero), and the stock driver puts them at the 1 dBm floor:
 * the field is the target's distance from it, saturated at 0xf8. Measured on
 * all 86 cold segments of the d6220 and the tg789vac: 0xd0 at target 56,
 * 0xe0 at 60, 0xe8 at 62, 0xf0 at 64, 0xf8 from 66 up, on every width and
 * sub-band.
 *
 * Where maxp5ga is 0 (the d6220's UNII-3) the target is the floor itself
 * and the stock driver writes 0xf8, not 0. What it computes there is not
 * known; the board is treated as saturated.
 */
static u16 b43_phy_ac_cck_rate_po(struct b43_phy_ac *ac)
{
	u8 target = b43_ppr_ac_get_max(&ac->txpwr_ppr);

	if (!ac->txpwr_maxp)
		return 0x00f8;
	return (u16)min((target - B43_PHY_AC_QDB(1)) * 4, 0xf8);
}

/*
 * BSS basic rate map: for each of the eight OFDM rates, write into
 * M_RT_BBRSMAP_A the block pointer of its basic rate, as brcmsmac's
 * brcms_c_write_rate_shm(). The observed basic set is {6, 12, 24} with "the
 * highest one not above": 0x04c6 for 6 and 9, 0x04da for 12 and 18, 0x04ee
 * for 24 to 54. The pointers come from the table.
 *
 * The stock driver runs it twice per attach: in the MAC config block and
 * after the third sweep. Only the first is preceded by the three invariant
 * cells 0x0082/0x00ba/0x003c, which therefore belong to the call site.
 */
static void b43_phy_ac_basic_rate_map(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Index, among the eight OFDM rates, of each one's basic rate. */
	static const u8 basic_of[8] = { 0, 0, 2, 2, 4, 4, 4, 4 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(basic_of); i++)
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_AC_RT_BBRSMAP_A +
				b43_phy_ac_ofdm_dirmap[i] * 2,
				dev->phy.ac->rate_ptr[basic_of[i]]);
}

/*
 * Seed one tone of the RX-IQ measurement, at @step of the period.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   28360-28446, 28732-28818, 29108-29194, 29433-29519, 29656-29742,
 *   29958-30044]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23572-23658, 23944-24030, 24320-24406, 24645-24731, 24868-24954,
 *   25292-25378]
 */
void b43_phy_ac_rxiqcal_dds_seed_tone(struct b43_wldev *dev, int step)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	b43_phy_ac_tone_table_write(dev, b43_phy_ac_tone_period, step);
}

/*
 * The chain masks of the 0x05d4-0x05dc block, which b43.h calls KEYIDXBLOCK
 * for the v4 firmware and which the AC core uses for something else.
 *
 * 0x05d4 and 0x05dc are coremask on every segment of every sweep (0x3 on the
 * d6220, 0x7 on the agcombo and the tg789vac).
 *
 * 0x05da follows 0x05d8 where that keeps more than one chain, and is
 * coremask where 0x05d8 drops to one. This is the 7.14.89 behaviour; the
 * agcombo's 7.14.43 writes coremask there throughout. It is not
 * temperature-driven: tempsense reads mid-range on the deviating segments.
 *
 * 0x05d6 and 0x05d8 are the chain choice of two rate classes and follow the
 * regulatory headroom, not the sub-band. Per class, the stock driver takes
 * the chain count with the highest total power
 *
 *   min(board, limit - class_offset[n]) + 10 log10(n)
 *
 * with the fewer chains on a tie. limit is the locale's Local Max for the
 * channel and width less the board's antenna gain; board is the top row of
 * the operating width; the offsets are those `wl curpower` prints
 * (router-data/agcombo/stats.txt, dsl3580l/wl1_curpower_ch52-bw80.txt):
 * CDD on 2 and 3 chains 3 and 5 dB under one chain, TXBF 6 and 9.75. 0x05d6
 * behaves as the TXBF rows and 0x05d8 as the CDD rows; that assignment is
 * the best fit of the four tried, not a known meaning.
 *
 * The Local Max is not in cfg80211, and is not the cap that binds the target
 * (b43_phy_ac_locale_ceiling()): at ch64/20 the target caps at 20.5 dBm
 * while the masks need 30 or more. The table below is fitted to the masks of
 * the three boards, from OBJ.WR 0x05d6/0x05d8 in every cold segment of the
 * d6220, tg789vac-v2 and agcombo; the ranges intersect on 18 of the 19
 * channel/width pairs. A channel not in the table takes coremask. The
 * tg789vac's ch100 and ch116 at 80 MHz do not fit (0x05d8 is 0x5 where
 * ch132/80, same rows, has 0x7) and are left out.
 *
 * Above 20 MHz the TX power site's second write of the block sees a level
 * 1 dB higher; at 20 MHz every site writes the same pair. Why is not known
 * (docs/retrace-todo.md).
 */
struct b43_phy_ac_local_max_row {
	u8 first, last;		/* primary channel range, inclusive */
	u8 level;		/* Local Max, quarter-dBm EIRP */
};

static const struct b43_phy_ac_local_max_row b43_phy_ac_local_max_20[] = {
	{  36,  48,  84 },
	{  52,  64, 120 },
	{ 100, 144, 124 },
};

static const struct b43_phy_ac_local_max_row b43_phy_ac_local_max_40[] = {
	{  36,  44, 106 },
	{ 108, 140, 136 },
};

static const struct b43_phy_ac_local_max_row b43_phy_ac_local_max_80[] = {
	{  36,  36, 104 },
	{ 100, 132, 132 },
};

static u16 b43_phy_ac_local_max(struct b43_phy_ac *ac)
{
	const struct b43_phy_ac_local_max_row *rows;
	unsigned int n, i;

	switch (ac->cal_width) {
	case NL80211_CHAN_WIDTH_80:
		rows = b43_phy_ac_local_max_80;
		n = ARRAY_SIZE(b43_phy_ac_local_max_80);
		break;
	case NL80211_CHAN_WIDTH_40:
		rows = b43_phy_ac_local_max_40;
		n = ARRAY_SIZE(b43_phy_ac_local_max_40);
		break;
	default:
		rows = b43_phy_ac_local_max_20;
		n = ARRAY_SIZE(b43_phy_ac_local_max_20);
		break;
	}
	for (i = 0; i < n; i++)
		if (ac->cal_channel >= rows[i].first &&
		    ac->cal_channel <= rows[i].last)
			return rows[i].level;
	return 0;
}

enum b43_phy_ac_chain_site {
	B43_PHY_AC_CHAIN_SETUP,		/* channel setup (x2) and down */
	B43_PHY_AC_CHAIN_TXPWR,		/* b43_phy_ac_txpwr_adjust() */
};

/* Chain count of the class whose per-chain offsets are @off, see above. */
static unsigned int b43_phy_ac_chain_count(int board, int limit,
					   const u8 *off, unsigned int chains)
{
	static const u8 gain[] = { 0, 0, 12, 19 };	/* 10 log10(n), quarter dB */
	unsigned int n, best = 1;
	int best_total = min(board, limit);

	for (n = 2; n <= chains; n++) {
		int total = min(board, limit - off[n]) + gain[n];

		if (total > best_total) {
			best_total = total;
			best = n;
		}
	}
	return best;
}

/* @n chains out of coremask, spread as far apart as the mask allows. */
static u16 b43_phy_ac_chain_mask(u16 coremask, unsigned int n)
{
	unsigned int chains = hweight8((u8)coremask);
	u16 lowest = coremask & (u16)-coremask;

	if (n >= chains)
		return coremask;
	if (n == 1)
		return lowest;
	/* two of three: the outer ones */
	return coremask & ~(u16)(lowest << 1);
}

/* The pair @site writes on 0x05d6/0x05d8, into @out. */
static void b43_phy_ac_chain_pair(struct b43_wldev *dev,
				  enum b43_phy_ac_chain_site site, u16 *out)
{
	static const u8 off_cdd[] = { 0, 0, 12, 20 };
	static const u8 off_txbf[] = { 0, 0, 24, 39 };
	struct b43_phy_ac *ac = dev->phy.ac;
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	unsigned int chains = hweight8(ac->coremask);
	u16 level = b43_phy_ac_local_max(ac);
	int board, limit, antgain;

	out[0] = out[1] = ac->coremask;
	if (!level || !ac->txpwr_maxp)
		return;

	antgain = sprom->antenna_gain_qdb[1];
	if (antgain < 0)
		antgain = 0;
	limit = (int)level - antgain;
	if (site == B43_PHY_AC_CHAIN_TXPWR &&
	    ac->cal_width != NL80211_CHAN_WIDTH_20)
		limit += 4;
	board = (int)ac->txpwr_maxp -
		(0x7f - b43_ppr_ac_row_max(&ac->txpwr_spacing, ac->cal_width));

	out[0] = b43_phy_ac_chain_mask(ac->coremask,
				       b43_phy_ac_chain_count(board, limit,
							      off_txbf, chains));
	out[1] = b43_phy_ac_chain_mask(ac->coremask,
				       b43_phy_ac_chain_count(board, limit,
							      off_cdd, chains));
}

static void b43_phy_ac_chainmask_block(struct b43_wldev *dev,
				       enum b43_phy_ac_chain_site site)
{
	u16 mask = dev->phy.ac->coremask;
	u16 pair[2];

	b43_phy_ac_chain_pair(dev, site, pair);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x05d4, mask);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x05d6, pair[0]);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x05d8, pair[1]);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x05da,
			hweight8((u8)pair[1]) > 1 ? pair[1] : mask);
}

/*
 * shm 0x00cc carries in bits 8:6 the chain mask of 0x05d6, which the
 * chain-mask block of the same site writes a few ops later, and a constant
 * 0x0004 below. Bit 0 is the core's, set by the BSS configuration.
 *
 * From every write of the cell on the two cold sweeps: 0x44, 0xc4, 0x144 and
 * 0x1c4 where 0x05d6 is 1, 3, 5 and 7, following the site where the mask
 * changes between setup and TX power passes.
 */
static void b43_phy_ac_bss_cc_update(struct b43_wldev *dev,
				     enum b43_phy_ac_chain_site site)
{
	u16 pair[2];
	u16 cc = b43_shm_read16(dev, B43_SHM_SHARED, 0x00cc);

	b43_phy_ac_chain_pair(dev, site, pair);
	cc = (u16)((cc & ~0x01c0) | (pair[0] << 6));
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
}

/*
 * Distance of a legacy OFDM rate from the target, in sixteenths of a dB,
 * for the per-rate block and the beacon cell, on the BCM4352.
 *
 * @spacing is the rate's entry in txpwr_spacing, the SROM rows laid out from
 * 0x7f so that no entry saturates. The rows keep their distance from a capped
 * target -- on the d6220's ch100/20 the target sits 1 dB under the top row
 * and the fields are still the SROM's 0/16/32/48 -- and the legacy limit of
 * b43_phy_ac_legacy_cap(), @cap, saturates the rates it would put higher.
 */
static u16 b43_phy_ac_rate_po(const struct b43_phy_ac *ac, u8 spacing,
			      u16 cap)
{
	u16 dist = (u16)(b43_ppr_ac_get_max(&ac->txpwr_spacing) - spacing) * 4;
	int target = b43_ppr_ac_get_max(&ac->txpwr_ppr);

	if (cap && target > (int)cap - 6)
		dist = max(dist, (u16)(target - ((int)cap - 6)) * 4);
	return dist;
}

/* Whether legacy rates take the capped form of b43_phy_ac_rate_po_capped(). */
static bool b43_phy_ac_legacy_capped(struct b43_wldev *dev)
{
	return dev->dev->chip_id == 0x4360;
}

/*
 * The same distance on the BCM4360, where a legacy rate sits at its own entry
 * of the finished table, @power, capped like every rate by the ceiling and
 * under the legacy limit, instead of keeping its SROM distance from a capped
 * target. Where a cap binds every legacy rate below it lands on it: on
 * ch36-48/20 the tg789vac and the agcombo write zero on all eight under a
 * target of 56, where their SROM spacing goes to 2 dB, and on ch100/20 the
 * tg789vac writes zero at 76 over rows that go to 2.5 dB. The d6220, a
 * BCM4352, keeps its spacing under the same cap (ch100/20, 0/16/32/48).
 */
static u16 b43_phy_ac_rate_po_capped(const struct b43_phy_ac *ac, u8 power,
				     u16 cap)
{
	int target = b43_ppr_ac_get_max(&ac->txpwr_ppr);
	int p = power;

	if (cap)
		p = min(p, (int)cap - 6);
	return target > p ? (u16)(target - p) * 4 : 0;
}

/* A limit per primary channel range, for the tables below and the locale's. */
struct b43_phy_ac_locale_row {
	u8 first, last;		/* primary channel range, inclusive */
	u8 limit;		/* quarter-dBm */
};

/*
 * The legacy limit for one chain, in quarter-dBm before the margin, per
 * primary channel and width. The legacy rates go out on the chains of
 * 0x05d6, and their limit is this one less the CDD offset of that many
 * chains (3 and 5 dB on two and three). The same table serves both chips:
 * the tg789vac (4360, 7.14.89), the agcombo (4360, 7.14.43) and the d6220
 * (4352, 7.14.89), whose 0x05d6 differ across many configurations, give the
 * same single-chain limit on every one they share, to 1 dB on the agcombo's
 * ch36 first pass and ch100/80 and on the d6220's ch60/40 and ch100/40.
 * Measured from the per-rate fields of the three cold sweeps.
 */
static const struct b43_phy_ac_locale_row b43_phy_ac_legacy_20[] = {
	{  36,  48, 62 },
	{  52, 144, 94 },
};

static const struct b43_phy_ac_locale_row b43_phy_ac_legacy_40[] = {
	{  36,  44, 74 },
	{  52,  52, 98 },
	{  60,  60, 78 },
	{ 100, 100, 86 },
	{ 108, 140, 98 },
};

static const struct b43_phy_ac_locale_row b43_phy_ac_legacy_80[] = {
	{  36,  36, 74 },
	{  52,  52, 90 },
	{ 100, 128, 90 },
	{ 132, 144, 98 },
};

static void b43_phy_ac_chain_pair(struct b43_wldev *dev,
				  enum b43_phy_ac_chain_site site, u16 *out);

static u16 b43_phy_ac_legacy_cap(struct b43_wldev *dev,
				      enum b43_phy_ac_chain_site site,
				      enum nl80211_chan_width width)
{
	static const u8 off_cdd[] = { 0, 0, 12, 20 };
	struct b43_phy_ac *ac = dev->phy.ac;
	const struct b43_phy_ac_locale_row *rows;
	unsigned int n, i, chains;
	u16 pair[2];

	switch (width) {
	case NL80211_CHAN_WIDTH_80:
		rows = b43_phy_ac_legacy_80;
		n = ARRAY_SIZE(b43_phy_ac_legacy_80);
		break;
	case NL80211_CHAN_WIDTH_40:
		rows = b43_phy_ac_legacy_40;
		n = ARRAY_SIZE(b43_phy_ac_legacy_40);
		break;
	default:
		rows = b43_phy_ac_legacy_20;
		n = ARRAY_SIZE(b43_phy_ac_legacy_20);
		break;
	}
	b43_phy_ac_chain_pair(dev, site, pair);
	chains = min_t(unsigned int, hweight8((u8)pair[0]), 3);
	for (i = 0; i < n; i++)
		if (ac->cal_channel >= rows[i].first &&
		    ac->cal_channel <= rows[i].last)
			return rows[i].limit - off_cdd[chains];
	return 0;
}

/*
 * Field at +0x0e of the per-rate block: the rate's power offset, in
 * sixteenths of a dB. brcmsmac has nothing at this offset.
 *
 * It is the rate's distance from the top of the per-rate table, read on the
 * spacing table, which never saturates: on the d6220's UNII-3, with maxp5ga 0
 * and the power table flat at the floor, the stock driver still writes the
 * SROM spacing.
 *
 * The legacy OFDM rates are read on the row of the operating width, not the
 * 20 MHz one: a legacy rate on a bonded channel goes out duplicated over the
 * whole block. Within the row each rate reads the group of its modulation
 * class (ppr_ac.h): 6 to 18 Mb/s the first, 24 to 54 the next four.
 *
 * The target is the maximum over every row the channel loads, which is what
 * the PHY closes its loop on. The regulatory ceiling applies to the power
 * table only (b43_ppr_ac_load_max_from_sprom()), not to the distances; the
 * legacy limit does, through b43_phy_ac_rate_po().
 *
 * Not reproduced: at 40 and 80 MHz the stock driver writes max(distance, K)
 * for a K constant across a segment's rates (1 dB on ch60/40, 2 dB on
 * ch116/80, 3 dB on ch36/80), and nothing derives K yet; ch100 at 40 and 80
 * MHz do not even take that form (docs/retrace-todo.md).
 *
 * The CCK rates have no 5 GHz SROM field in rev 11; see
 * b43_phy_ac_cck_rate_po().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   13009-13068, 13683-13742, 36065-36124]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8703-8762, 9313-9372, 28607-28634]
 */
static void b43_phy_ac_prb_rsp_rate_po(struct b43_wldev *dev,
				       enum b43_phy_ac_chain_site site)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;
	const struct b43_ppr_ac *sp = &ac->txpwr_spacing;
	bool capped = b43_phy_ac_legacy_capped(dev);
	u16 cap = b43_phy_ac_legacy_cap(dev, site, ac->cal_width);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_prb_rsp_rates); i++) {
		const struct b43_phy_ac_prb_rsp_rate *r =
			&b43_phy_ac_prb_rsp_rates[i];
		u16 tab = r->cck ? B43_AC_RT_DIRMAP_B : B43_AC_RT_DIRMAP_A;
		u16 ptr = b43_shm_read16(dev, B43_SHM_SHARED,
					 tab + r->dirmap * 2);
		u16 cell = (u16)(2 * ptr + B43_AC_RT_RATE_PO);
		u16 val;

		if (r->cck)
			val = b43_phy_ac_cck_rate_po(ac);
		else if (capped)
			val = b43_phy_ac_rate_po_capped(ac,
					b43_ppr_ac_ofdm(&ac->txpwr_ppr,
							ac->cal_width, r->ofdm),
					cap);
		else
			val = b43_phy_ac_rate_po(ac,
						 b43_ppr_ac_ofdm(sp, ac->cal_width,
								 r->ofdm),
						 cap);

		b43_shm_read16(dev, B43_SHM_SHARED, cell);
		b43_shm_write16(dev, B43_SHM_SHARED, cell, val);
	}
}

/*
 * shm 0x00ce: the power offset of the beacon rate.
 *
 * Same form as the per-rate field of b43_phy_ac_prb_rsp_rate_po(), with two
 * differences: it is for the beacon rate only (6 Mbit/s at 5 GHz), and it is
 * always read on the 20 MHz row, not the operating width's.
 *
 * The vendor writes it from the 20 MHz power of the beacon rate, which
 * depends only on the per-rate table, so it is derived from that.
 *
 * Verified on the 43 cold segments: it equals the 6 Mbit/s per-rate field on
 * 37, and the other six are exactly the 40 and 80 MHz ones where the 20 MHz
 * and operating rows differ. The legacy limit applies here too.
 *
 * The stock driver overlays a field masked 0x700, left at zero here: no
 * capture shows the cell above 0x100, so its meaning is unknown.
 */
static u16 b43_phy_ac_beacon_pwr_offset_at(struct b43_wldev *dev,
					   enum b43_phy_ac_chain_site site)
{
	const struct b43_phy_ac *ac = dev->phy.ac;
	enum nl80211_chan_width w = ac->cal_width;
	u16 cap;

	/*
	 * The cell is the 6 Mbit/s field, in each chip's form: at 20 and
	 * 40 MHz that of the operating width; at 80 MHz that of the 20 MHz row
	 * under the 20 MHz legacy limit, which on ch36, ch52, ch116 and ch132
	 * gives the beacon power of the three boards (ch100 is 2 to 3 dB above
	 * it on all of them).
	 */
	if (w == NL80211_CHAN_WIDTH_80)
		w = NL80211_CHAN_WIDTH_20;
	cap = b43_phy_ac_legacy_cap(dev, site, w);
	if (b43_phy_ac_legacy_capped(dev))
		return b43_phy_ac_rate_po_capped(ac,
				b43_ppr_ac_ofdm(&ac->txpwr_ppr, w, 0), cap);
	return b43_phy_ac_rate_po(ac, b43_ppr_ac_ofdm(&ac->txpwr_spacing,
						      w, 0), cap);
}

/*
 * The invariant part of the MAC config block, right after the two zeroings.
 *
 * Every value is the same on every segment of the d6220 cold sweep, and the
 * tg789vac writes the same ones except for the chain-count cells. They are
 * transcribed; their purpose is not known.
 *
 * Not here yet: the fifteen cells at stride 0x14 that follow the width, the
 * twelve at stride 0x1c that follow the centre frequency, the read of 0x00b0
 * (the core's EXTNPHYCTL in b43.h) and 0x05dc (coremask, in KEYIDXBLOCK).
 * Who should emit the last two is not settled.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   12860-12892]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8547-8579]
 */
static void b43_phy_ac_shm_mac_config_block(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Sixteen consecutive words from 0x092c. */
	static const u16 blocco_092c[] = {
		0x3475, 0x3475, 0x3475, 0x217c, 0x237b, 0x217c, 0x217c, 0x217c,
		0x3276, 0x3276, 0x3475, 0x187e, 0x217c, 0x167e, 0x1d7d, 0x1f7c,
	};
	/* Seven consecutive words from 0x0902. */
	static const u16 blocco_0902[] = {
		0x41c2, 0x0000, 0x0017, 0x024b, 0x0097, 0x0500, 0x0000,
	};
	unsigned int i;

	b43_shm_write16(dev, B43_SHM_SHARED, 0x0020, 0x0800);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x08ec, 0x186a);
	/*
	 * The d6220, with one of its three cores unwired, writes 0x0910 and
	 * 0x08f4 and then 0x08f0 = 1; the agcombo and the tg789vac, all three
	 * wired, skip the pair and write 0x08f0 = 2. Two boards cannot tell "a
	 * core is unwired" from "two chains".
	 */
	if (hweight8((u8)dev->phy.ac->coremask) != dev->phy.ac->num_cores) {
		b43_shm_write16(dev, B43_SHM_SHARED, 0x0910, 0x80c3);
		b43_shm_write16(dev, B43_SHM_SHARED, 0x08f4, 0x80c2);
	}
	b43_shm_write16(dev, B43_SHM_SHARED, 0x08f0,
			(u16)(hweight8((u8)dev->phy.ac->coremask) - 1));

	/*
	 * Read and written back: the value is zero in the capture, and the
	 * read-rewrite form avoids inventing it.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0eec,
			b43_shm_read16(dev, B43_SHM_SHARED, 0x0eec));

	for (i = 0; i < ARRAY_SIZE(blocco_092c); i++)
		b43_shm_write16(dev, B43_SHM_SHARED,
				(u16)(0x092c + i * 2), blocco_092c[i]);
	for (i = 0; i < ARRAY_SIZE(blocco_0902); i++)
		b43_shm_write16(dev, B43_SHM_SHARED,
				(u16)(0x0902 + i * 2), blocco_0902[i]);
}

/*
 * CLASSCTL write with a status_mask update: no peek and no clip detect. The
 * capture has two cases of an isolated write with no preceding peek.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11402-11402, 11435-11435, 12185-12185, 14970-14970, 15720-15720,
 *   15746-15746, 16496-16496, 16534-16534, 16549-16549, 22892-22892,
 *   22895-22895, 22936-22936, 22951-22951, 27682-27682, 27685-27685,
 *   27699-27699, 30647-30647]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7074-7074, 7123-7123, 7873-7873, 10384-10384, 11134-11134, 11160-11160,
 *   11910-11910, 11948-11948, 11963-11963, 18020-18020, 18023-18023,
 *   18064-18064, 18079-18079, 22894-22894, 22897-22897, 22911-22911,
 *   25929-25929]
 */
static void b43_phy_ac_classctl_write(struct b43_wldev *dev, bool arm)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;

	/*
	 * Bit 0x0800 is set at 20 MHz and clear above: 0x0df4/0x0df6 at 20 MHz,
	 * 0x05f4/0x05f6 at 40 and 80. The only write per attach that does not
	 * follow the width is the first, in channel_switch_prep(), which
	 * carries bit 11 over from its peek before coeff_bank_init() has set
	 * it.
	 */
	b43_phy_write(dev, 0x0140,
		      (u16)((arm ? 0x0df4 : 0x0df6) &
			    (b43_phy_ac_bw_step(dev) ? ~0x0800 : ~0)));
	phy_ac->status_mask = (phy_ac->status_mask & ~B43_PHY_AC_STATE_RX_ANY) |
			      (arm ? B43_PHY_AC_STATE_RX_WAITED
				   : (B43_PHY_AC_STATE_RX_WAITED |
				      B43_PHY_AC_STATE_RX_OFDM));
}

/*
 * Peeked CLASSCTL write, the common pattern (35 cases in the capture). Do
 * not inline the write.
 */
static void b43_phy_ac_classctl_write_peeked(struct b43_wldev *dev, bool arm)
{
	b43_phy_read_log(dev, 0x0140);
	b43_phy_ac_classctl_write(dev, arm);
}

/*
 * The RX gate: the three helpers above, each called by name, in the order
 * the stock driver uses (35 cases in the capture). Armed is 0x0140 = 0x0df4
 * (WAITED only) with clip detect on; released is 0x0df6 (OFDM | WAITED) with
 * clip detect off.
 *   1. classctl_write_peeked(arm), a peek plus a write of 0x0140
 *   2. adc_hold(!arm), four MODs on 0x02?d bit 0x0010
 *   3. clip_det(!arm), three MODs on 0x?d4 bit 0x4000
 */
static void b43_phy_ac_rx_gate_with_adc_hold(struct b43_wldev *dev, bool arm)
{
	b43_phy_ac_classctl_write_peeked(dev, arm);
	b43_phy_ac_adc_hold(dev, !arm);
	b43_phy_ac_clip_det(dev, !arm);
}

/*
 * Sampling passes per core for one idle-TSSI measurement: 1 at 20 and
 * 40 MHz, 256 at 80 (d6220 sweep: 6 reads of 0x0012 per segment against
 * 1536).
 *
 * Transcribed: the setup before the sampling is identical at 20 and 80 MHz,
 * so the count lives in the stock driver's loop, not in a register.
 */
static unsigned int b43_phy_ac_idle_tssi_passes(struct b43_wldev *dev)
{
	return dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80 ? 256 : 1;
}

/*
 * RF_SEQ_MODE (0x0401) with every sequencer field on: the two high nibbles
 * are 7 on every board, the two low ones carry the coremask -- 0x7733 on
 * the d6220, 0x7777 on the agcombo and the tg789vac.
 */
static u16 b43_phy_ac_rfseq_mode_all(struct b43_wldev *dev)
{
	u16 m = (u16)dev->phy.ac->coremask;

	return (u16)(0x7700 | (m << 4) | m);
}

/*
 * Read back each wired chain's bbmult cell, 0x63 + 4 * core, and discard it.
 * The vendor's read is self-contained -- lock, read, unlock -- and
 * actab_read_bulk() does not unlock, so the unlock is here.
 */
static void b43_phy_ac_bbmult_peek(struct b43_wldev *dev)
{
	unsigned int c;

	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		b43_actab_read_log(dev, 0x000c, 0x0063 + 4 * c, 1);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}
}

/*
 * Write @val into both bbmult cells of @core, 0x63 + 4 * core and 0x73 +
 * 4 * core, under its own lock of the table write gate.
 */
static void b43_phy_ac_bbmult_write(struct b43_wldev *dev,
					unsigned int core, const u16 *val)
{
	u16 saved = b43_phy_ac_tbl_write_lock(dev);

	b43_actab_write_bulk(dev, 0x000c, (u16)(0x0063 + 4 * core), 16, 1, val);
	b43_actab_write_bulk(dev, 0x000c, (u16)(0x0073 + 4 * core), 16, 1, val);
	b43_phy_ac_tbl_write_unlock(dev, saved);
}


/*
 * The measurement control the idle-TSSI and temperature readings borrow:
 * 0x0394 selects what 0x0393 arms, and bit 9 of 0x040f is held clear while
 * they run. Saved before, restored after.
 */
struct b43_phy_ac_meas_ctl {
	u16 r040f, r0394, r0393;
};

static void b43_phy_ac_meas_ctl_save(struct b43_wldev *dev,
				     struct b43_phy_ac_meas_ctl *ctl)
{
	ctl->r040f = b43_phy_read(dev, 0x040f);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
	ctl->r0394 = b43_phy_read(dev, 0x0394);
	ctl->r0393 = b43_phy_read(dev, 0x0393);
}

static void b43_phy_ac_meas_ctl_restore(struct b43_wldev *dev,
					const struct b43_phy_ac_meas_ctl *ctl)
{
	b43_phy_write(dev, 0x0394, ctl->r0394);
	b43_phy_write(dev, 0x0393, ctl->r0393);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, ctl->r040f & 0x0200);
}

/*
 * Idle-TSSI: measure and commit the per-core base index. The index is
 * measured, not constant, and the loop is gated on the coremask. Three
 * iterations per bring-up, from op_switch_channel(), post_cal_finalize() and
 * post_cal_finalize_iter3().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11446-12192, 14981-15727, 15757-16503]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7134-7880, 10395-11141, 11171-11917]
 */
static void b43_phy_ac_idle_tssi_meas(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * The phase is absent above 5250 MHz on a first bring-up: the stock
	 * driver makes 18 accesses to RAD 0x0548 at ch36 and 9 at ch52, and the
	 * 9 left are from b43_radio_2069_pwron() and radio_percore_setup_1(),
	 * which run on both sides. RAD 0x004e/0x0166/0x024e/0x0366 and PHY
	 * 0x0747/0x0732/0x0733 agree.
	 *
	 * The gate is here rather than at the three call sites because the
	 * whole phase does not run.
	 *
	 * It does not hand back the RX gate: the bracket is gated at both ends
	 * (the arm in txpwrctrl_enable(), the close in op_switch_channel()),
	 * and the counts of 0x0140, 0x0339 and 0x02ed on cold05 are exact.
	 */
	if (!b43_phy_ac_may_calibrate_tx(dev))
		return;
	/*
	 * The per-core base index is measured here: it is the idle-TSSI
	 * readback divided by four, see the write of 0x0645 below.
	 *
	 * The REQUIRE preconditions are the caller's: iteration 1 (set_channel)
	 * runs with the MAC suspended, iteration 2 (post_cal_finalize) with the
	 * MAC up, iteration 3 suspended again.
	 */
	static const u16 zero;
	struct b43_phy_ac *ac = dev->phy.ac;
	/* Each chain's state, saved before the measurement and put back. */
	struct {
		u16 r747, r732, r733, r734, r727, r73c;
		u16 r739, r73a, r725, r4e, r166;
		u16 inner;
	} st[B43_PHY_AC_MAX_CORES] = { 0 };
	unsigned int core, c;
	u16 r013 = 0, r012 = 0, r464 = 0;
	u16 idle_tssi = 0;

	/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
	 *   11446-11520, 14981-15055, 15757-15831]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   7134-7208, 10395-10469, 11171-11245]
	 */
	B43_AC_BLOCK(dev, "tssi_path_enable");
	/* Enable the TSSI path on each chain. */
	{
		unsigned int c;

		for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
			b43_phy_maskset(dev, 0x0072, (u16)~0x0004, 0x0004);
			b43_phy_maskset(dev, 0x0727 + c * 0x200, (u16)~0x0004, 0x0004);
			b43_phy_maskset(dev, 0x073c + c * 0x200, (u16)~0x0010, 0x0000);
		}
	}
	b43_phy_ac_radio_percore_setup_1(dev);
	b43_phy_read_log(dev, 0x0401);
	/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
	 *   11522-12192, 15057-15727, 15833-16503]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   7210-7880, 10471-11141, 11247-11917]
	 */
	B43_AC_BLOCK(dev, "rf_seq_mode_percore");
	/*
	 * The per-core fields of RF_SEQ_MODE are coremask and coremask << 12:
	 * 0x0003/0x3000 on a 2x2, 0x0007/0x7000 on a 3x3.
	 */
	b43_phy_maskset(dev, 0x0401, (u16)~0x0007, dev->phy.ac->coremask);
	b43_phy_maskset(dev, 0x0401, (u16)~0x7000,
			(u16)(dev->phy.ac->coremask << 12));

	for_each_set_bit(core, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 p = (u16)(core * 0x0200);
		struct b43_phy_ac_meas_ctl ctl;
		u16 base_index, gate;

		/*
		 * Prologue, once per measured core. The 0x0140 gate is already
		 * armed by the rx_gate_with_adc_hold(true) at the end of
		 * adc_reset(), and is released and re-armed at the end of the
		 * per-core body.
		 *
		 * The table gate is peeked here and locked four reads later;
		 * the peek is what the unlock before the bbmult read-back
		 * restores.
		 */
		gate = b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_ac_meas_ctl_save(dev, &ctl);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Save each chain's bbmult and gain state. */
		for_each_set_bit(c, &ac->coremask, ac->num_cores) {
			u16 s = (u16)(c * 0x200);

			b43_actab_read_bulk(dev, 0x0c, (u16)(0x63 + 4 * c), 16, 1,
					    &ac->bbmult_saved[c]);
			st[c].r747 = b43_phy_read(dev, 0x0747 + s);
			st[c].r732 = b43_phy_read(dev, 0x0732 + s);
			st[c].r733 = b43_phy_read(dev, 0x0733 + s);
			st[c].r734 = b43_phy_read(dev, 0x0734 + s);
			b43_phy_read_log(dev, 0x0722 + s);
			st[c].r727 = b43_phy_read(dev, 0x0727 + s);
			st[c].r73c = b43_phy_read(dev, 0x073c + s);
		}

		/* Quiet every chain: gains, bbmult and the TSSI radio path. */
		for_each_set_bit(c, &ac->coremask, ac->num_cores) {
			u16 s = (u16)(c * 0x200);
			u16 inner;

			b43_phy_write(dev, 0x0732 + s, 0x0000);
			b43_phy_write(dev, 0x0733 + s, 0x0000);
			b43_phy_write(dev, 0x0747 + s, 0x0000);
			b43_phy_maskset(dev, 0x0734 + s, (u16)~0x0038, 0);
			b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0001, 0x0001);
			b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0008, 0x0008);
			inner = b43_phy_ac_tbl_write_lock(dev);
			b43_actab_write_bulk(dev, 0x000c, (u16)(0x0063 + 4 * c), 16, 1, &zero);
			b43_actab_write_bulk(dev, 0x000c, (u16)(0x0073 + 4 * c), 16, 1, &zero);
			b43_phy_ac_tbl_write_unlock(dev, inner);
			st[c].r4e = b43_radio_read(dev, 0x004e + s);
			st[c].r166 = b43_radio_read(dev, 0x0166 + s);
			b43_actab_read_log(dev, 0x0007, 0x017e + 0x10 * c, 1);
			/*
			 * pdet_range: no pdetrange5g in the NVRAM of these
			 * boards (default 0), and the SPROM8 FEM offsets are
			 * 0xffff on SROM 11, so the bits are cleared.
			 */
			b43_radio_maskset(dev, 0x004e + s, (u16)~0x0e00, 0);
			b43_radio_maskset(dev, 0x0166 + s, (u16)~0x0002, 0x0002);
		}

		/* The bbmult cells as they stand mid-sequence, written back. */
		b43_phy_ac_tbl_write_unlock(dev, gate);
		udelay(100);
		for_each_set_bit(c, &ac->coremask, ac->num_cores) {
			b43_actab_read_bulk(dev, 0x000c, (u16)(0x0063 + 4 * c),
					    16, 1, &st[c].inner);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		}
		for_each_set_bit(c, &ac->coremask, ac->num_cores)
			b43_phy_ac_bbmult_write(dev, c, &st[c].inner);

		b43_phy_ac_cca_pulse(dev);
		b43_phy_ac_run_samples(dev, 0x0000, false);
		udelay(100);

		/* Gain override on every chain, then put back in reverse. */
		for_each_set_bit(c, &ac->coremask, ac->num_cores) {
			u16 s = (u16)(c * 0x200);

			st[c].r739 = b43_phy_read(dev, 0x0739 + s);
			b43_phy_write(dev, 0x0739 + s, st[c].r739 | 0x0080);
			st[c].r73a = b43_phy_read(dev, 0x073a + s);
			b43_phy_write(dev, 0x073a + s, st[c].r73a);
			st[c].r725 = b43_phy_read(dev, 0x0725 + s);
			b43_phy_write(dev, 0x0725 + s, st[c].r725 | 0x0004);
		}
		udelay(1);
		for (c = ac->num_cores; c-- > 0; ) {
			u16 s = (u16)(c * 0x200);

			if (!(ac->coremask & BIT(c)))
				continue;
			b43_phy_write(dev, 0x0725 + s, st[c].r725);
			b43_phy_write(dev, 0x073a + s, st[c].r73a);
			b43_phy_write(dev, 0x0739 + s, st[c].r739);
		}
		udelay(100);
		/*
		 * Sample the measurement and average it. Each pass arms the
		 * measurement and reads 0x0013 then 0x0012; a pass with a zero
		 * measurement field is dropped. The arming is per pass: the
		 * captures rewrite 0x0394 and 0x0393 before every pair of
		 * reads, which shows at 80 MHz where there are 256.
		 */
		{
			unsigned int passes = b43_phy_ac_idle_tssi_passes(dev);
			unsigned int i;
			u32 sum = 0;

			for (i = 0; i < passes; i++) {
				b43_phy_read_log(dev, 0x0393);
				b43_phy_write(dev, 0x0394, 0x0110 | core);
				b43_phy_write(dev, 0x0393, 0x8000);

				r013 = b43_phy_read(dev, 0x0013);
				r012 = b43_phy_read(dev, 0x0012);

				if (!(r012 & B43_PHY_AC_IDLE_TSSI_MEAS))
					continue;
				sum += (r012 & B43_PHY_AC_IDLE_TSSI_MEAS) >> 2;
			}

			idle_tssi = (u16)(B43_PHY_AC_IDLE_TSSI_BASE +
					  sum / passes);
		}
		r464 = b43_phy_read(dev, 0x0464);
		b43_phy_set(dev, B43_PHY_AC_SAMP_PLAY_CTL, B43_PHY_AC_SAMP_PLAY_STOP);
		b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);
		for_each_set_bit(c, &ac->coremask, ac->num_cores)
			b43_phy_ac_bbmult_write(dev, c, &zero);
		b43_phy_ac_cca_pulse(dev);

		/*
		 * Put every chain back as it was. 0x0722 is the one cleared
		 * rather than restored. 0x0734 is what it was read as: 0x0000
		 * on the d6220, 0x0029 on the tg789vac.
		 */
		for_each_set_bit(c, &ac->coremask, ac->num_cores) {
			u16 s = (u16)(c * 0x200);

			b43_phy_write(dev, 0x0732 + s, st[c].r732);
			b43_phy_write(dev, 0x0733 + s, st[c].r733);
			b43_phy_write(dev, 0x0747 + s, st[c].r747);
			b43_phy_write(dev, 0x0722 + s, 0x0000);
			b43_phy_write(dev, 0x0734 + s, st[c].r734);
			b43_phy_write(dev, 0x0727 + s, st[c].r727);
			b43_phy_write(dev, 0x073c + s, st[c].r73c);
			b43_phy_ac_bbmult_write(dev, c, &ac->bbmult_saved[c]);
			/*
			 * Restore what st[c].r4e saved at the start of the
			 * turn. The captures write back, position by position,
			 * what they read: a value derived from the width misses
			 * the third turn, where the register holds 0x80c0 at 20
			 * MHz and 0x0123 at 40 (and the per-core twin 0x8109 at
			 * 40). What leaves those bits there is not known.
			 */
			b43_radio_write(dev, 0x004e + s, st[c].r4e);
			b43_radio_write(dev, 0x0166 + s, st[c].r166);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_ac_meas_ctl_restore(dev, &ctl);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		/*
		 * The base index is
		 *
		 *   0x200 + sum(meas >> 2 over the non-zero passes) / passes
		 *
		 * where meas is 0x0012 masked to its measurement field, the
		 * division truncates, and the divisor counts every pass, not
		 * only those that carried a reading. Exact on all 52 sweep
		 * segments, every width and both cores (312 writes of 0x0645
		 * and 0x0845); dropping any of the three rules costs 11 or more
		 * of them, all at 80 MHz, or all three writes on ch140/20.
		 *
		 * The 0x200 is bit 11 of the readback surviving the shift: a
		 * pass reading 0x0800 has a zero measurement field. Core 1
		 * measures zero on every pass of every capture and writes 0x200
		 * flat.
		 *
		 * The field is the low ten bits of 0x?645. The stock driver's
		 * trace shows bits 10 to 15 set (0xfe02 under mask 0x03ff)
		 * because it holds the index as a sign-extended s16 and
		 * mod_phy_reg() writes only `val & mask`; b43_phy_maskset() ORs
		 * the set in whole, so the ten bits are masked here.
		 */
		base_index = idle_tssi & 0x03ff;
		b43_phy_maskset(dev, 0x0645 + p, (u16)~0x03ff, base_index);

		b43dbg(dev->wl,
		       "phy-ac: idle-tssi c%u meas: 0x013=0x%04x 0x012=0x%04x 0x464=0x%04x radio 0x4e=0x%04x 0x166=0x%04x prog=0x%04x\n",
		       core, r013, r012, r464,
		       st[core].r4e, st[core].r166, base_index);
	}

	/* Closing ops of iteration 1, emitted once. */
	b43_phy_write(dev, 0x0401, b43_phy_ac_rfseq_mode_all(dev));
	b43_phy_ac_rx_gate_with_adc_hold(dev, false);
}

/*
 * The stock driver's own locale, in quarter-dBm before the margin, per
 * primary channel and width. These are conducted limits: the d6220 (5.5 dB
 * antenna gain), the agcombo (5.5) and the tg789vac (4.25), with different
 * maxp5ga and mcsbw*po, write the same target wherever one of these binds,
 * and their own SROM value elsewhere. brcmsmac has the same kind of table
 * (locale_5g_* in channel.c); this one is measured, from PHY.MOD 0x0646
 * mask=0x00ff in every cold segment of the d6220 and the tg789vac-v2. It is
 * combined with cfg80211's limit in b43_phy_ac_reg_ceiling().
 *
 * Only the rows that bind on at least one board. The d6220's second
 * txpwrctrl pass on ch36/ch44 at 40 and 80 MHz (68) is not here; see
 * docs/retrace-todo.md.
 */
static const struct b43_phy_ac_locale_row b43_phy_ac_locale_20[] = {
	{  36,  48, 62 },
	{  64,  64, 82 },
	{ 100, 100, 82 },
	{ 104, 128, 90 },
	{ 132, 144, 86 },
};

static const struct b43_phy_ac_locale_row b43_phy_ac_locale_40[] = {
	{  36,  44, 74 },
	{  60,  60, 66 },
	{ 100, 100, 74 },
};

static const struct b43_phy_ac_locale_row b43_phy_ac_locale_80[] = {
	{  36,  36, 74 },
	{ 100, 100, 82 },
};

static u16 b43_phy_ac_locale_ceiling(struct b43_phy_ac *ac)
{
	const struct b43_phy_ac_locale_row *rows;
	unsigned int n, i;

	switch (ac->cal_width) {
	case NL80211_CHAN_WIDTH_80:
		rows = b43_phy_ac_locale_80;
		n = ARRAY_SIZE(b43_phy_ac_locale_80);
		break;
	case NL80211_CHAN_WIDTH_40:
		rows = b43_phy_ac_locale_40;
		n = ARRAY_SIZE(b43_phy_ac_locale_40);
		break;
	default:
		rows = b43_phy_ac_locale_20;
		n = ARRAY_SIZE(b43_phy_ac_locale_20);
		break;
	}
	for (i = 0; i < n; i++)
		if (ac->cal_channel >= rows[i].first &&
		    ac->cal_channel <= rows[i].last)
			return rows[i].limit;
	return 0;
}

/*
 * Regulatory ceiling for the configuration, in quarter-dBm, or 0 when none
 * applies.
 *
 * QDB(ch->max_power) - antgain, clamped at zero: an upper bound on the SROM
 * limit, as in brcmsmac. A bonded configuration is bounded by every
 * 20 MHz channel it occupies.
 *
 * The stock driver's ceilings do not bind on the hot sweeps; on a first
 * bring-up they do, under the locale it runs before userspace sets a
 * country, and they are board-independent: the d6220, the agcombo and the
 * tg789vac (antenna gains 5.5, 5.5 and 4.25 dB) write the same 56 on
 * ch36-48/20, 60 on ch60/40, 68 on ch100/40 and 76 on ch100 at 20 and 80.
 * So they are conducted limits, with no antenna gain taken off.
 *
 * cfg80211's max_power is EIRP, and taking the antenna gain off it keeps b43
 * within the regulatory domain. It carries one value per 20 MHz channel,
 * while the stock limits are per bandwidth, so the measured table of
 * b43_phy_ac_locale_ceiling() is applied too, and the tighter one wins.
 */
static u16 b43_phy_ac_reg_ceiling(struct b43_wldev *dev)
{
	const struct cfg80211_chan_def *chandef = &dev->wl->hw->conf.chandef;
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int span, i;
	int antgain, best = INT_MAX;

	if (!chandef->chan)
		return 0;

	switch (ac->cal_width) {
	case NL80211_CHAN_WIDTH_80:
		span = 4;
		break;
	case NL80211_CHAN_WIDTH_40:
		span = 2;
		break;
	default:
		span = 1;
		break;
	}

	antgain = sprom->antenna_gain_qdb[1];
	if (antgain < 0)
		antgain = 0;

	/*
	 * The block starts at the primary and runs upwards in 20 MHz steps;
	 * each channel is looked up on its own, since a domain may cap them
	 * differently.
	 */
	for (i = 0; i < span; i++) {
		struct ieee80211_channel *sub;
		int lim;

		sub = ieee80211_get_channel(dev->wl->hw->wiphy,
					   5000 + 5 * (ac->cal_channel + 4 * i));
		if (!sub || sub->max_power <= 0)
			continue;

		lim = B43_PHY_AC_QDB(sub->max_power) - antgain;
		if (lim < 0)
			lim = 0;
		if (lim < best)
			best = lim;
	}

	{
		u16 locale = b43_phy_ac_locale_ceiling(ac);

		if (locale && locale < best)
			best = locale;
	}
	return best == INT_MAX ? 0 : (u16)best;
}

/*
 * TX power target: the per-rate table and its maximum per core.
 *
 * As in brcmsmac and in b43_nphy_op_recalc_txpower(), with the rev 11 table: for every rate the
 * channel carries, min(SROM limit, regulatory limit) less the 6-unit margin,
 * floored at 1 dBm. The maximum over the rates is what the PHY closes its
 * power loop on, written to 0x0646[7:0] per core.
 *
 * The 6 + antenna gain that phy_n.c keeps under "#if 0" is what the captures
 * reproduce, with the antenna gain on the regulatory side only: the hot
 * sweep is exact from the SROM alone on all 26 configurations.
 *
 * The floor is not brcmsmac's 8 dBm. It binds only on the d6220's UNII-3,
 * where maxp5ga is 0: the stock driver writes 0x04 there on all eight
 * configurations (cold21-ch149-bw20). For the same reason there is no stage
 * switching off the rates below the TSSI-visible power: that threshold is
 * 17 quarters on that sub-band and 4 is programmed all the same.
 *
 * The SROM table is loaded from the minimum maxp5ga over the active cores,
 * as b43's loader does, and each core then adds back the difference to its
 * own maxp5ga. On the boards captured the cores are equal.
 *
 * Since the 40 and 80 MHz tables also carry their 20-in-40, 20-in-80 and
 * 40-in-80 rows, the maximum lands on the smallest mcsbw*po offset among the
 * widths the channel contains. Against the hot sweep this is exact at 20 and
 * 40 MHz and on ch36 and ch100 at 80. Missing is one term the stock driver
 * takes off at hot on ch36/40, ch52/40 and ch52/80 (two units): a per-rate
 * limit of the country in force, the user target or the TSSI-visible
 * threshold; the captures do not say which.
 *
 * Returns whether the target changed since the last computation.
 */
bool b43_phy_ac_txpwr_recalc(struct b43_wldev *dev)
{
	B43_AC_FN();
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	struct b43_phy_ac *ac = dev->phy.ac;
	struct b43_ppr_ac *ppr = &ac->txpwr_ppr;
	unsigned int sb = b43_ppr_ac_subband(ac->cal_channel, ac->cal_width);
	u16 ceiling = b43_phy_ac_reg_ceiling(dev);
	unsigned int core;
	u8 maxp, max;

	if (ac->txpwr_calc_chan == ac->cal_channel &&
	    ac->txpwr_calc_width == ac->cal_width &&
	    ac->txpwr_calc_ceiling == ceiling)
		return false;

	maxp = b43_ppr_ac_load_max_from_sprom(sprom, ac->coremask, ac->num_cores,
					      ppr, ac->cal_channel, ac->cal_width,
					      (u8)min_t(u16, ceiling, 0xff));
	b43_ppr_ac_load_spacing(sprom, &ac->txpwr_spacing, ac->cal_channel,
				ac->cal_width);
	if (!maxp)
		b43warn(dev->wl,
			"AC-PHY: maxp5ga is 0 for the sub-band of channel %u: "
			"the SROM declares no power, target at the floor\n",
			ac->cal_channel);
	b43_ppr_ac_add(ppr, -6);
	b43_ppr_ac_apply_min(ppr, B43_PHY_AC_QDB(1));
	max = b43_ppr_ac_get_max(ppr);

	ac->txpwr_maxp = maxp;

	if (b43_ppr_ac_sprom_has_subband_po(sprom) && !ac->txpwr_calc_chan)
		b43warn(dev->wl,
			"AC-PHY: the SROM has explicit sub-band row offsets "
			"(sb*/dot11agdup*/mcslr*), which are not applied: "
			"the 40/80 MHz power target may differ\n");

	for (core = 0; core < ac->num_cores; core++) {
		u8 own = sprom->core_pwr_info[core].maxp5ga[sb];
		u8 target = max;

		if (own > maxp)
			target = (u8)min_t(unsigned int, max + (own - maxp), 0x7f);
		ac->txpwr_max[core] = target;
	}

	ac->txpwr_calc_chan = ac->cal_channel;
	ac->txpwr_calc_width = ac->cal_width;
	ac->txpwr_calc_ceiling = ceiling;

	return true;
}

/*
 * The target registers, one per core, from the highest core down as the
 * stock driver does (0x0846 before 0x0646). Called from the channel setup at
 * its two sites and from adjust_txpower().
 */
static void b43_phy_ac_txpwr_target_write(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int cr;

	for (cr = ac->num_cores; cr-- > 0; ) {
		if (!((ac->coremask >> cr) & 1))
			continue;
		b43_phy_maskset(dev, 0x0646 + cr * 0x0200, (u16)~0x00ff,
				ac->txpwr_max[cr]);
	}
}

/*
 * est_pwr transfer function for one core, 128 entries.
 *
 * At step j: num = 512*b0 + 32*b1*j, den = 0x8000 + a1*j,
 * v = (den/2 + num)/den clamped to [-8, 0x7f]. The same formula as
 * b43_nphy_tx_power_ctl_setup() and brcmsmac, with 128 entries instead of
 * 64. The coefficients are the SPROM pa5ga[] triple of the current sub-band,
 * with a per-core default for boards whose triple is all zero.
 *
 * On 2.4 GHz the triple is pa2ga[], one for the band; with the archer-t5e's
 * SROM this matches its tables 0x40 and 0x60 128/128 on both chains. No board
 * here has an empty pa2ga, so the 5 GHz default is used for 2.4 GHz too.
 */
static void b43_phy_ac_est_pwr_lut(struct b43_wldev *dev, unsigned int core,
				   unsigned int grp, u16 *lut)
{
	static const struct { s16 a1, b0, b1; } pwrdet_def[3] = {
		{ (s16)0xff49, (s16)0x12d9, (s16)0xfd99 },
		{ (s16)0xff54, (s16)0x1212, (s16)0xfd89 },
		{ (s16)0xff53, (s16)0x11b7, (s16)0xfdc0 },
	};
	const struct ssb_sprom_core_pwr_info *pw =
		&dev->dev->bus_sprom->core_pwr_info[core];
	const u16 *pa = b43_current_band(dev->wl) == NL80211_BAND_2GHZ ?
		pw->pa2ga : &pw->pa5ga[grp * 3];
	s16 a1, b0, b1;
	s32 num, den;
	int j;

	if (pa[0] || pa[1] || pa[2]) {
		a1 = (s16)pa[0];
		b0 = (s16)pa[1];
		b1 = (s16)pa[2];
	} else {
		a1 = pwrdet_def[core].a1;
		b0 = pwrdet_def[core].b0;
		b1 = pwrdet_def[core].b1;
	}

	num = (s32)b0 << 9;
	den = 0x8000;
	for (j = 0; j < 128; j++) {
		s32 d = den ? den : 1;	/* guard: a real pa keeps den > 0 */
		s32 v = (d / 2 + num) / d;

		num += (s32)b1 * 0x20;
		if (v < -8)
			v = -8;
		if (v > 0x7f)
			v = 0x7f;
		lut[j] = (u16)(v & 0xff);
		den += a1;
	}
}

/*
 * The programming half of txpwrctrl_setup(), for sub-band @grp: current
 * index, target power, the est_pwr LUT of every active core and table 0x21.
 * Separate because b43_phy_ac_down() issues the same sequence again, on the
 * sub-band setup() cached.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   13072-13404, 13746-14078]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8766-9098, 9376-9708]
 */
static void b43_phy_ac_txpwrctrl_program(struct b43_wldev *dev,
					 unsigned int grp)
{
	B43_AC_FN();
	static const u16 est_pwr_tbl_id[3] = { 0x40, 0x60, 0x80 };
	u8 num_cores = dev->phy.ac->num_cores;
	u32 ppr[24] = { 0 };
	u8 core;

	/*
	 * Table 0x21, 24 u32s: the power-detector offsets by rate group, one
	 * byte per core in the low bytes of each word. Entries 1, 5 and 6 take
	 * the sub-band nibble of pdoffset40ma[core], entry 10 that of
	 * pdoffset80ma[core]; the rest is zero (rev 11 has no 20 MHz field, and
	 * pdoffsetcckma is zero on the one board that declares it).
	 *
	 * The d6220 and the agcombo have the same nibble on every core; the
	 * tg789vac-v2 has a different one per core and sub-band in both fields,
	 * and all 129 writes of its 43 cold segments follow this formula.
	 *
	 * On 2.4 GHz the MacBookAir6,1 and the archer-t5e write zeros. The 2.4
	 * GHz field, pdoffset2g40ma (SROM word 100), is not extracted; it is
	 * blank on the archer-t5e.
	 */
	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		const struct ssb_sprom *sp = dev->dev->bus_sprom;
		u32 o40, o80;

		if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ)
			break;

		o40 = (sp->pdoffset40ma[core] >> (4 * grp)) & 0xf;
		o80 = (sp->pdoffset80ma[core] >> (4 * grp)) & 0xf;
		ppr[1] |= o40 << (8 * core);
		ppr[5] |= o40 << (8 * core);
		ppr[6] |= o40 << (8 * core);
		ppr[10] |= o80 << (8 * core);
	}

	b43_phy_maskset(dev, 0x0072, (u16)~(0x0001), (0x0001));
	b43_phy_maskset(dev, 0x0070, (u16)~0x8000, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~(0x0100), (0x0100));
	b43_phy_maskset(dev, 0x0072, (u16)~0x4000, 0);
	b43_phy_maskset(dev, 0x0072, (u16)~(0x4000), (0x4000));
	b43_phy_maskset(dev, 0x0070, (u16)~0x8000, 0);

	/* Per-core current index: 0x14. */
	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		b43_phy_maskset(dev, 0x0644 + core * 0x0200, (u16)~0x007f, 0x0014);
	}

	/*
	 * The idle-TSSI base index is written by b43_phy_ac_idle_tssi_meas(),
	 * which op_switch_channel() calls before this.
	 */

	/* Target power and control bits: 0xc8. */
	b43_phy_maskset(dev, 0x0071, (u16)~0x00ff, 0x00c8);
	b43_phy_maskset(dev, 0x0071, (u16)~0x0700, 0x0400);
	b43_phy_maskset(dev, 0x0070, (u16)~0x0800, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~(0x0400), (0x0400));

	/*
	 * Per-core target power, high core to low as the stock driver does. The
	 * value is from b43_phy_ac_txpwr_recalc(): the 0x38 every board writes
	 * on attach is the regulatory ceiling binding.
	 */
	b43_phy_ac_txpwr_target_write(dev);

	/*
	 * Per-core est_pwr LUT, 128 u16s, then the per-rate ppr, 24 u32s, with
	 * plain bulk writes: the gate is held by the relock at the phase
	 * transition.
	 */
	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		u16 lut[128];


		b43_phy_ac_est_pwr_lut(dev, core, grp, lut);
		b43_actab_write_bulk(dev, est_pwr_tbl_id[core], 0, 16, 128, lut);
	}
	b43_actab_write_bulk(dev, 0x21, 0, 32, 24, ppr);
}

static void b43_phy_ac_txpwrctrl_setup(struct b43_wldev *dev, u16 freq)
{
	B43_AC_FN();
	unsigned int grp = b43_phy_ac_pa5g_group(dev, freq);

	dev->phy.ac->pa5g_grp = (u8)grp;

	/*
	 * The stock driver's state on entry: CLASSCTL released (0x0df6), clip
	 * detect enabled on every core, the CCA_RESET pulse finished, the MAC
	 * suspended. txpwrctrl is set up with the RX classifier live.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED |
			   B43_PHY_AC_STATE_RX_OFDM,
			   B43_PHY_AC_STATE_RX_CCK |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS |
			   B43_PHY_AC_STATE_CCA_RESET |
			   B43_PHY_AC_STATE_MAC_EN);

	b43_phy_ac_txpwrctrl_program(dev, grp);
}

/*
 * TX-gain table for 5 GHz, EPA path, radio 2069 rev 4.
 *
 * Byte-for-byte transcription of the vendor blob symbol
 * `acphy_txgain_epa_5g_2069rev4` (wlD6220.o .rodata @ 0x403af0, 768 bytes).
 * Each of the 128 entries is a triplet of big-endian u16 fields as stored
 * in the blob, re-expressed here as host-endian u16: one 48-bit entry per TX
 * power index. Bits 7:0 are the bbmult, the byte table 0x20 and the 0x0c
 * cells 0x63/0x73 + 4 * core take; bits 47:8 are the three gain code words
 * b43_phy_ac_txpwr_by_index() writes to table 0x07 at 0x100/0x103/0x106 +
 * core.
 *
 * Verified byte-for-byte across three independent blob branches carrying
 * the same symbol name: wlDSL-3580_EU.o_save (6.30), wlD6220.o_save
 * (7.14.89), and wl.ko extracted from AGSOT_1_0_8.img (Sercomm, unrelated
 * to Netgear/D-Link). D6220 and AGSOT match 128/128; the 6.30 DSL branch
 * diverges from index 31 onward (97/128 entries different). Two physically
 * independent boards carrying the same exact value rules out per-board
 * calibration -- this is the generic 7.x-branch table.
 *
 * The low byte of column [0] reproduces the vendor's table 0x20 load 128/128.
 * The 6.30 hybrid loads the whole entries there instead, and its 5 GHz
 * table differs from this one in 38 of 384 words.
 *
 * Which table, EPA or IPA, the blob picks on which board is not established;
 * every board captured uses the EPA ones.
 */
static const u16 b43_acphy_txgain_epa_5g_2069rev4[128][3] = {
	{ 0x0044, 0x7f00, 0xf3ff },
	{ 0x0040, 0x7f00, 0xf3ff },
	{ 0x0040, 0x7f00, 0xf3ef },
	{ 0x0040, 0x7f00, 0xf3df },
	{ 0x0041, 0x7f00, 0xf3cf },
	{ 0x003f, 0x7f00, 0xf3c7 },
	{ 0x0041, 0x7f00, 0xf3b7 },
	{ 0x0040, 0x7f00, 0xf3af },
	{ 0x003f, 0x7f00, 0xf3a7 },
	{ 0x0041, 0x7f00, 0xf397 },
	{ 0x0041, 0x7f00, 0xf38f },
	{ 0x0041, 0x7f00, 0xf387 },
	{ 0x0040, 0x7f00, 0xf37f },
	{ 0x0040, 0x7f00, 0xf377 },
	{ 0x0041, 0x7f00, 0xf36f },
	{ 0x0042, 0x7f00, 0xf367 },
	{ 0x003e, 0x7f00, 0xf367 },
	{ 0x003f, 0x7f00, 0xf35f },
	{ 0x0041, 0x7f00, 0xf357 },
	{ 0x003d, 0x7f00, 0xf357 },
	{ 0x0040, 0x7f00, 0xf34f },
	{ 0x0042, 0x7f00, 0xf347 },
	{ 0x003f, 0x7f00, 0xf347 },
	{ 0x0043, 0x7f00, 0xf33f },
	{ 0x003f, 0x7f00, 0xf33f },
	{ 0x0044, 0x7f00, 0xf337 },
	{ 0x0040, 0x7f00, 0xf337 },
	{ 0x003c, 0x7f00, 0xf337 },
	{ 0x0043, 0x7f00, 0xf32f },
	{ 0x003f, 0x7f00, 0xf32f },
	{ 0x003c, 0x7f00, 0xf32f },
	{ 0x003f, 0x6f00, 0xf32f },
	{ 0x0042, 0x6700, 0xf32f },
	{ 0x0041, 0x5f00, 0xf32f },
	{ 0x003e, 0x5f00, 0xf32f },
	{ 0x0042, 0x5700, 0xf32f },
	{ 0x003e, 0x5700, 0xf32f },
	{ 0x003e, 0x4f00, 0xf32f },
	{ 0x0042, 0x4700, 0xf32f },
	{ 0x003e, 0x4700, 0xf32f },
	{ 0x0042, 0x3f00, 0xf32f },
	{ 0x003f, 0x3f00, 0xf32f },
	{ 0x0045, 0x3700, 0xf32f },
	{ 0x0041, 0x3700, 0xf32f },
	{ 0x003d, 0x3700, 0xf32f },
	{ 0x0041, 0x2f00, 0xf32f },
	{ 0x003e, 0x2f00, 0xf32f },
	{ 0x003a, 0x2f00, 0xf32f },
	{ 0x0045, 0x2700, 0xf32f },
	{ 0x0041, 0x2700, 0xf32f },
	{ 0x003d, 0x2700, 0xf32f },
	{ 0x0046, 0x1f00, 0xf32f },
	{ 0x0042, 0x1f00, 0xf32f },
	{ 0x003e, 0x1f00, 0xf32f },
	{ 0x003b, 0x1f00, 0xf32f },
	{ 0x0037, 0x1f00, 0xf32f },
	{ 0x0047, 0x1700, 0xf32f },
	{ 0x0043, 0x1700, 0xf32f },
	{ 0x003f, 0x1700, 0xf32f },
	{ 0x003c, 0x1700, 0xf32f },
	{ 0x0039, 0x1700, 0xf32f },
	{ 0x0037, 0x1600, 0xf32f },
	{ 0x0037, 0x1500, 0xf32f },
	{ 0x0035, 0x1400, 0xf32f },
	{ 0x0035, 0x1300, 0xf32f },
	{ 0x0034, 0x1200, 0xf32f },
	{ 0x0034, 0x1100, 0xf32f },
	{ 0x0034, 0x1000, 0xf32f },
	{ 0x0036, 0x0f00, 0xf32f },
	{ 0x0036, 0x0e00, 0xf32f },
	{ 0x0034, 0x0d00, 0xf32f },
	{ 0x0035, 0x0c00, 0xf32f },
	{ 0x0038, 0x0b00, 0xf32f },
	{ 0x0039, 0x0a00, 0xf32f },
	{ 0x003d, 0x0900, 0xf32f },
	{ 0x0040, 0x0800, 0xf32f },
	{ 0x0044, 0x0700, 0xf32f },
	{ 0x0040, 0x0700, 0xf32f },
	{ 0x003c, 0x0700, 0xf32f },
	{ 0x0038, 0x0700, 0xf32f },
	{ 0x0034, 0x0700, 0xf32f },
	{ 0x0030, 0x0700, 0xf32f },
	{ 0x002c, 0x0700, 0xf32f },
	{ 0x0028, 0x0700, 0xf32f },
	{ 0x0024, 0x0700, 0xf32f },
	{ 0x0020, 0x0700, 0xf32f },
	{ 0x001c, 0x0700, 0xf32f },
	{ 0x0018, 0x0700, 0xf32f },
	{ 0x0014, 0x0700, 0xf32f },
	{ 0x0012, 0x0700, 0xf32f },
	{ 0x0011, 0x0700, 0xf32f },
	{ 0x0010, 0x0700, 0xf32f },
	{ 0x000f, 0x0700, 0xf32f },
	{ 0x000e, 0x0700, 0xf32f },
	{ 0x000d, 0x0700, 0xf32f },
	{ 0x000c, 0x0700, 0xf32f },
	{ 0x000c, 0x0700, 0xf32f },
	{ 0x000b, 0x0700, 0xf32f },
	{ 0x000a, 0x0700, 0xf32f },
	{ 0x000a, 0x0700, 0xf32f },
	{ 0x0009, 0x0700, 0xf32f },
	{ 0x0009, 0x0700, 0xf32f },
	{ 0x0008, 0x0700, 0xf32f },
	{ 0x0008, 0x0700, 0xf32f },
	{ 0x0007, 0x0700, 0xf32f },
	{ 0x0007, 0x0700, 0xf32f },
	{ 0x0007, 0x0700, 0xf32f },
	{ 0x0006, 0x0700, 0xf32f },
	{ 0x0006, 0x0700, 0xf32f },
	{ 0x0006, 0x0700, 0xf32f },
	{ 0x0005, 0x0700, 0xf32f },
	{ 0x0005, 0x0700, 0xf32f },
	{ 0x0005, 0x0700, 0xf32f },
	{ 0x0004, 0x0700, 0xf32f },
	{ 0x0004, 0x0700, 0xf32f },
	{ 0x0004, 0x0700, 0xf32f },
	{ 0x0004, 0x0700, 0xf32f },
	{ 0x0003, 0x0700, 0xf32f },
	{ 0x0003, 0x0700, 0xf32f },
	{ 0x0003, 0x0700, 0xf32f },
	{ 0x0003, 0x0700, 0xf32f },
	{ 0x0003, 0x0700, 0xf32f },
	{ 0x0003, 0x0700, 0xf32f },
	{ 0x0002, 0x0700, 0xf32f },
	{ 0x0002, 0x0700, 0xf32f },
	{ 0x0002, 0x0700, 0xf32f },
	{ 0x0002, 0x0700, 0xf32f },
	{ 0x0002, 0x0700, 0xf32f },
};

/*
 * The same table for 2.4 GHz, acphy_txgain_epa_2g_2069rev4 of the 6.30.102.7
 * blob (wlDSL-3580_EU.o_save of impl14_v07, the source of radio_2069.c's
 * channel table). The hybrid wl on the MacBookAir6,1 and the archer-t5e loads
 * it whole into table 0x20 on 2.4 GHz, 384 of 384 words on both. Not checked
 * against a 7.14 blob.
 */
static const u16 b43_acphy_txgain_epa_2g_2069rev4[128][3] = {
	{ 0x0044, 0xffff, 0xa7ff },
	{ 0x0040, 0xffff, 0xa7ff },
	{ 0x003f, 0xffff, 0xa7ef },
	{ 0x003f, 0xffff, 0xa7df },
	{ 0x003f, 0xffff, 0xa7cf },
	{ 0x0040, 0xffff, 0xa7bf },
	{ 0x0041, 0xffff, 0xa7af },
	{ 0x0040, 0xffff, 0xa7a7 },
	{ 0x003f, 0xffff, 0xa79f },
	{ 0x0041, 0xffff, 0xa78f },
	{ 0x0041, 0xffff, 0xa787 },
	{ 0x0040, 0xffff, 0xa77f },
	{ 0x0040, 0xffff, 0xa777 },
	{ 0x0040, 0xffff, 0xa76f },
	{ 0x0041, 0xffff, 0xa767 },
	{ 0x0042, 0xffff, 0xa75f },
	{ 0x003e, 0xffff, 0xa75f },
	{ 0x0040, 0xffff, 0xa757 },
	{ 0x0042, 0xffff, 0xa74f },
	{ 0x003e, 0xffff, 0xa74f },
	{ 0x0041, 0xffff, 0xa747 },
	{ 0x003d, 0xffff, 0xa747 },
	{ 0x0040, 0xffff, 0xa73f },
	{ 0x003d, 0xffff, 0xa73f },
	{ 0x0041, 0xffff, 0xa737 },
	{ 0x003d, 0xffff, 0xa737 },
	{ 0x0043, 0xffff, 0xa72f },
	{ 0x0040, 0xffff, 0xa72f },
	{ 0x003c, 0xffff, 0xa72f },
	{ 0x0043, 0xffff, 0xa727 },
	{ 0x0040, 0xffff, 0xa727 },
	{ 0x003c, 0xffff, 0xa727 },
	{ 0x0046, 0xffff, 0xa71f },
	{ 0x0042, 0xffff, 0xa71f },
	{ 0x003e, 0xffff, 0xa71f },
	{ 0x003b, 0xffff, 0xa71f },
	{ 0x004a, 0xffff, 0xa717 },
	{ 0x0046, 0xffff, 0xa717 },
	{ 0x0042, 0xffff, 0xa717 },
	{ 0x003e, 0xffff, 0xa717 },
	{ 0x003b, 0xffff, 0xa717 },
	{ 0x0037, 0xffff, 0xa717 },
	{ 0x004c, 0xffff, 0xa70f },
	{ 0x0048, 0xffff, 0xa70f },
	{ 0x0044, 0xffff, 0xa70f },
	{ 0x0040, 0xffff, 0xa70f },
	{ 0x003c, 0xffff, 0xa70f },
	{ 0x0039, 0xffff, 0xa70f },
	{ 0x0036, 0xffff, 0xa70f },
	{ 0x0033, 0xffff, 0xa70f },
	{ 0x002f, 0xffff, 0xa70f },
	{ 0x0055, 0xffff, 0xa707 },
	{ 0x0050, 0xffff, 0xa707 },
	{ 0x004b, 0xffff, 0xa707 },
	{ 0x0047, 0xffff, 0xa707 },
	{ 0x0043, 0xffff, 0xa707 },
	{ 0x003f, 0xffff, 0xa707 },
	{ 0x003c, 0xffff, 0xa707 },
	{ 0x0038, 0xffff, 0xa707 },
	{ 0x0035, 0xffff, 0xa707 },
	{ 0x0032, 0xffff, 0xa707 },
	{ 0x0030, 0xffff, 0xa707 },
	{ 0x002d, 0xffff, 0xa707 },
	{ 0x002a, 0xffff, 0xa707 },
	{ 0x003f, 0xcfff, 0xa707 },
	{ 0x0042, 0xc7ff, 0xa707 },
	{ 0x003f, 0xc7ff, 0xa707 },
	{ 0x0043, 0xbfff, 0xa707 },
	{ 0x003f, 0xbfff, 0xa707 },
	{ 0x0045, 0xb7ff, 0xa707 },
	{ 0x0041, 0xb7ff, 0xa707 },
	{ 0x003d, 0xb7ff, 0xa707 },
	{ 0x0044, 0xafff, 0xa707 },
	{ 0x0040, 0xafff, 0xa707 },
	{ 0x003d, 0xafff, 0xa707 },
	{ 0x0046, 0xa7ff, 0xa707 },
	{ 0x0041, 0xa7ff, 0xa707 },
	{ 0x003e, 0xa7ff, 0xa707 },
	{ 0x003b, 0xa7ff, 0xa707 },
	{ 0x0046, 0x9fff, 0xa707 },
	{ 0x0042, 0x9fff, 0xa707 },
	{ 0x003e, 0x9fff, 0xa707 },
	{ 0x003b, 0x9fff, 0xa707 },
	{ 0x0037, 0x9fff, 0xa707 },
	{ 0x0046, 0x97ff, 0xa707 },
	{ 0x0042, 0x97ff, 0xa707 },
	{ 0x003f, 0x97ff, 0xa707 },
	{ 0x003b, 0x97ff, 0xa707 },
	{ 0x0038, 0x97ff, 0xa707 },
	{ 0x0035, 0x97ff, 0xa707 },
	{ 0x004c, 0x8fff, 0xa707 },
	{ 0x0048, 0x8fff, 0xa707 },
	{ 0x0044, 0x8fff, 0xa707 },
	{ 0x0040, 0x8fff, 0xa707 },
	{ 0x003c, 0x8fff, 0xa707 },
	{ 0x0039, 0x8fff, 0xa707 },
	{ 0x0036, 0x8fff, 0xa707 },
	{ 0x0033, 0x8fff, 0xa707 },
	{ 0x0030, 0x8fff, 0xa707 },
	{ 0x002d, 0x8fff, 0xa707 },
	{ 0x0058, 0x87ff, 0xa707 },
	{ 0x0053, 0x87ff, 0xa707 },
	{ 0x004f, 0x87ff, 0xa707 },
	{ 0x004a, 0x87ff, 0xa707 },
	{ 0x0046, 0x87ff, 0xa707 },
	{ 0x0042, 0x87ff, 0xa707 },
	{ 0x003e, 0x87ff, 0xa707 },
	{ 0x003b, 0x87ff, 0xa707 },
	{ 0x0038, 0x87ff, 0xa707 },
	{ 0x0035, 0x87ff, 0xa707 },
	{ 0x0032, 0x87ff, 0xa707 },
	{ 0x002f, 0x87ff, 0xa707 },
	{ 0x002c, 0x87ff, 0xa707 },
	{ 0x002a, 0x87ff, 0xa707 },
	{ 0x0027, 0x87ff, 0xa707 },
	{ 0x0025, 0x87ff, 0xa707 },
	{ 0x0023, 0x87ff, 0xa707 },
	{ 0x0021, 0x87ff, 0xa707 },
	{ 0x001f, 0x87ff, 0xa707 },
	{ 0x001e, 0x87ff, 0xa707 },
	{ 0x001c, 0x87ff, 0xa707 },
	{ 0x001a, 0x87ff, 0xa707 },
	{ 0x0019, 0x87ff, 0xa707 },
	{ 0x0017, 0x87ff, 0xa707 },
	{ 0x0016, 0x87ff, 0xa707 },
	{ 0x0015, 0x87ff, 0xa707 },
	{ 0x0014, 0x87ff, 0xa707 },
	{ 0x0013, 0x87ff, 0xa707 },
};

static const u16 (*b43_phy_ac_txgain_table(struct b43_wldev *dev))[3]
{
	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ)
		return b43_acphy_txgain_epa_2g_2069rev4;
	return b43_acphy_txgain_epa_5g_2069rev4;
}

/**************************************************
 * Open-loop TX gain (fixed index)
 **************************************************/

/*
 * Program the per-chain TX gain from the open-loop txgain LUT at index @idx.
 * At index 0x40 it rewrites what adc_reset() already programs, so it leaves
 * no footprint of its own in the capture. It is the only knob on the
 * operating index and runs last: change the index here, not the adc_reset()
 * constants.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   22770-22841, 27560-27631]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   17898-17969, 22772-22843]
 */
void b43_phy_ac_txpwr_by_index(struct b43_wldev *dev, u8 idx)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;
	static const u16 bbmult_lo[3] = { 0x0063, 0x0067, 0x006b };
	static const u16 bbmult_hi[3] = { 0x0073, 0x0077, 0x007b };
	unsigned int core;
	u16 saved;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * The side effects some stock call sites wrap around this sequence
	 * (restoring 0x0070 bits 15:13, writing 0x1641) are the caller's.
	 */
	saved = b43_phy_ac_tbl_write_lock(dev);

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		const u16 *e = b43_phy_ac_txgain_table(dev)[idx];
		u16 g0, g1, g2, bbmult, inner;

		bbmult =  e[0]       & 0x00ff;
		g0     = (e[0] >> 8) | ((e[1] & 0x00ff) << 8);
		g1     = (e[1] >> 8) | ((e[2] & 0x00ff) << 8);
		g2     =  e[2] >> 8;

		/* Batch A: 3 fast WR TBL 0x0007 (gain code) */
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0100), 16, 1, &g0);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0103), 16, 1, &g1);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0106), 16, 1, &g2);

		/* Batch B: 2 fast WR TBL 0x000c (bbmult per-antenna), nested */
		inner = b43_phy_ac_tbl_write_lock(dev);
		b43_actab_write_bulk(dev, 0xc, bbmult_lo[core], 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0xc, bbmult_hi[core], 16, 1, &bbmult);
		b43_phy_ac_tbl_write_unlock(dev, inner);

		b43info(dev->wl,
		       "phy-ac: txpwr_by_index core %u idx %u gain %04x/%04x/%04x bbmult %02x\n",
		       core, idx, g0, g1, g2, bbmult);
	}

	b43_phy_ac_tbl_write_unlock(dev, saved);
}

/**************************************************
 * RF sequencing
 **************************************************/

/*
 * Run one RF sequencer command as the vendor does: save RFCTL1 and the
 * table-write gate, set bit 0 of the gate and bits 1:0 of RFCTL1, trigger
 * @rf_seq (a B43_PHY_AC_RF_SEQ_* bit), wait for it to clear, then write both
 * registers back.
 *
 * Returns true if the sequence completed, false on timeout.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   6919-6931, 6932-6942, 30615-30625]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2589-2601, 2602-2612, 25897-25907]
 */
bool b43_phy_ac_force_rf_sequence(struct b43_wldev *dev, u16 rf_seq)
{
	B43_AC_FN();
	u16 rfctl1 = b43_phy_read_log(dev, B43_PHY_AC_RFCTL1);
	u16 gate = b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	bool done;

	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_RF_SEQ_OVERRIDE_GATE,
			B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);
	b43_phy_set(dev, B43_PHY_AC_RFCTL1, 0x0003);
	b43_phy_set(dev, B43_PHY_AC_RF_SEQ_TRIG, rf_seq);

	done = b43_phy_ac_rfseq_wait_done(dev, rf_seq,
					  B43_PHY_AC_RF_SEQ_FORCE_TURNS);
	if (!done)
		b43err(dev->wl, "AC-PHY: RF sequence 0x%04x timeout\n", rf_seq);

	b43_phy_write(dev, B43_PHY_AC_RFCTL1, rfctl1);
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, gate);
	return done;
}

/*
 * Play @nsamp + 1 samples of the loaded tone, looping. RFCTL1 bit 0 is held
 * for the play and written back as it was found. With @iqmode the play is
 * started through 0x0382, for the RX-IQ estimator, otherwise through the
 * sample-play control.
 */
static void b43_phy_ac_run_samples(struct b43_wldev *dev, u16 nsamp,
				   bool iqmode)
{
	u16 rfctl1;

	b43_phy_mask(dev, 0x0471, (u16)~0x0001);
	b43_phy_write(dev, B43_PHY_AC_SAMP_PLAY_NSAMP, nsamp);
	b43_phy_write(dev, B43_PHY_AC_SAMP_PLAY_LOOPS, 0xffff);
	b43_phy_write(dev, B43_PHY_AC_SAMP_PLAY_WAIT, 0x003c);
	rfctl1 = b43_phy_read_log(dev, B43_PHY_AC_RFCTL1);
	b43_phy_set(dev, B43_PHY_AC_RFCTL1, 0x0001);
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL,
		     (u16)~B43_PHY_AC_SAMP_PLAY_START);
	b43_phy_mask(dev, 0x0382, (u16)~0xc000);
	if (iqmode)
		b43_phy_set(dev, 0x0382, 0x8000);
	else
		b43_phy_set(dev, B43_PHY_AC_SAMP_PLAY_CTL,
			    B43_PHY_AC_SAMP_PLAY_START);

	if (!b43_phy_ac_rfseq_wait_done(dev, 0x0001,
					B43_PHY_AC_RUN_SAMPLES_TURNS))
		b43err(dev->wl, "AC-PHY: sample play timeout\n");

	b43_phy_write(dev, B43_PHY_AC_RFCTL1, rfctl1);
}

/*
 * CCA reset strobe: pulse bit 0x4000 of BBCFG (0x0001) with the PHY clock
 * forced, and track the state. Always a maskset pair: every op on 0x0001 in
 * both cold sweeps is a MOD (696 on the d6220, 784 on the agcombo).
 *
 * All 38 CCA pulses of cold01 are the same four ops:
 *
 *   PHY.FGC val=0x0001
 *   PHY.MOD addr=0x0001 val=0x4000 mask=0x4000
 *   PHY.MOD addr=0x0001 val=0x0000 mask=0x4000
 *   PHY.FGC val=0x0000
 *
 * so there is one function; b43_phy_ac_reset_cca() is its exported name.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11398-11399, 11444-11445, 11709-11710, 11795-11796, 12038-12039,
 *   12124-12125, 14979-14980, 15244-15245, 15330-15331, 15573-15574,
 *   15659-15660, 15755-15756, 16020-16021, 16106-16107, 16349-16350,
 *   16435-16436, 16543-16544, 19695-19696, 22876-22877, 22945-22946,
 *   24485-24486, 27666-27667, 27708-27709, 28465-28466, 28577-28578,
 *   28837-28838, 28953-28954, 29213-29214, 29329-29330, 29538-29539,
 *   29654-29655, 29761-29762, 29956-29957, 30063-30064, 30380-30381]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7070-7071, 7132-7133, 7397-7398, 7483-7484, 7726-7727, 7812-7813,
 *   10393-10394, 10658-10659, 10744-10745, 10987-10988, 11073-11074,
 *   11169-11170, 11434-11435, 11520-11521, 11763-11764, 11849-11850,
 *   11957-11958, 14823-14824, 18004-18005, 18073-18074, 19697-19698,
 *   22878-22879, 22920-22921, 23677-23678, 23789-23790, 24049-24050,
 *   24165-24166, 24425-24426, 24541-24542, 24750-24751, 24866-24867,
 *   24973-24974, 25290-25291, 25397-25398, 25662-25663]
 */
static void b43_phy_ac_cca_pulse(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;

	b43_phy_ac_force_clock(dev, true);
	b43_phy_maskset(dev, B43_PHY_AC_BBCFG,
			(u16)~B43_PHY_AC_BBCFG_RSTCCA,
			B43_PHY_AC_BBCFG_RSTCCA);
	phy_ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	udelay(1);
	b43_phy_maskset(dev, B43_PHY_AC_BBCFG,
			(u16)~B43_PHY_AC_BBCFG_RSTCCA, 0);
	phy_ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_ac_force_clock(dev, false);
	udelay(2);
}

/*
 * Reset the CCA (Clear Channel Assessment) state machine; see
 * b43_phy_ac_cca_pulse().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5022-5023, 5146-5147, 11235-11236]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   692-693, 816-817, 6905-6906]
 */
void
b43_phy_ac_reset_cca(struct b43_wldev *dev)
{
	b43_phy_ac_cca_pulse(dev);
}

/**************************************************
 * Various PHY ops
 **************************************************/

/*
 * Clip detector enable/disable, per core: bit 0x4000 of 0x06d4 + core*0x200
 * on phy rev 1, cleared to enable, set to freeze it during a channel
 * reconfigure.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5018-5020, 11407-11409, 11440-11442, 12190-12192, 14975-14977,
 *   15725-15727, 15751-15753, 16501-16503, 16539-16541, 22900-22902,
 *   22941-22943, 27690-27692, 27704-27706, 30652-30654]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   688-690, 7079-7081, 7128-7130, 7878-7880, 10389-10391, 11139-11141,
 *   11165-11167, 11915-11917, 11953-11955, 18028-18030, 18069-18071,
 *   22902-22904, 22916-22918, 25934-25936]
 */
static void b43_phy_ac_clip_det(struct b43_wldev *dev, bool enable)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;
	unsigned int core;

	/*
	 * Every silicon core, including an antenna-less core 2: the stock
	 * driver freezes the clip bit on all of them, so gate on presence, not
	 * on the active-chain mask.
	 *
	 * `0x06d4 + core * 0x200` is written inline because the correlator
	 * recognises that idiom as a per-core stride.
	 */
	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 bit = (u16)(B43_PHY_AC_STATE_CLIP_C0_DIS << core);

		if (enable) {
			b43_phy_mask(dev, 0x06d4 + core * 0x200, (u16)~0x4000);
			phy_ac->status_mask &= ~bit;
		} else {
			b43_phy_set(dev, 0x06d4 + core * 0x200, 0x4000);
			phy_ac->status_mask |= bit;
		}
	}
}

/*
 * The 15 ops the stock driver emits at the start of every set_channel to
 * quiesce the PHY before reprogramming the radio; the steps are labelled A
 * onwards in the body.
 *
 * Bit 8 of 0x0003 is band-related but not confirmed; nominal 5 GHz is
 * bit 0. The clip mask here is 0x0010, distinct from the 0x0020 that
 * set_reg_on_reset() uses on the same registers.
 *
 * Over the 104 segments of the d6220 cold and hot and agcombo cold sweeps
 * every write to 0x0140 is one of 0x05f4/0x05f6/0x0df4/0x0df6: bits [10:0]
 * are fixed, and bit 11 is taken from the peek below.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5007-5023]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   677-693]
 */
static void b43_phy_ac_channel_switch_prep(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * A: gate override and bandctl, after a peek of 0x019e whose value is
	 * discarded.
	 */
	(void)b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_RF_SEQ_OVERRIDE_GATE,
			B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);
	b43_phy_maskset(dev, 0x0003, (u16)~0x0100, 0x0100);

	/*
	 * B: classifier setup, a peek then a plain write. Bits [10:0] are fixed
	 * (bit 2 is WAITEDEN, the others are not identified); bit 11 is the 20
	 * MHz flag coeff_bank_init() sets later in this setup, and still holds
	 * what the previous setup or the PHY reset left: 0x0df7 on all 52 cold
	 * attaches, 0x05f7 on the first hot segment. Carrying it over from the
	 * peek matches all of them.
	 */
	{
		u16 cur = b43_phy_read_log(dev, 0x0140);
		u16 next = (u16)((cur & 0x0800) | 0x05f4);

		b43info(dev->wl,
		       "phy-ac: channel_switch_prep 0x0140 cur=0x%04x -> 0x%04x\n",
		       cur, next);
		b43_phy_write(dev, 0x0140, next);
	}

	/* C: release the ADC hold (MOD 0x02?d val=0 mask=0x0010). */
	b43_phy_ac_adc_hold(dev, false);

	/* D: per-core clip detect disable; clip_det() updates status_mask. */
	b43_phy_ac_clip_det(dev, false);

	/* E: extra reset, meaning not confirmed; always 0 in the captures. */
	b43_phy_write(dev, 0x0339, 0x0000);

	/* F: CCA reset. */
	b43_phy_ac_reset_cca(dev);

	/*
	 * The classifier write set only WAITEDEN in bits [2:0]; mirror it into
	 * status_mask as classifier() would.
	 */
	dev->phy.ac->status_mask = (dev->phy.ac->status_mask & ~B43_PHY_AC_STATE_RX_ANY) |
				   B43_PHY_AC_STATE_RX_WAITED;
}


/*
 * Quiesce the silicon RX cores the board does not wire.
 *
 * PHY reg 0x0b reports the silicon core count (3 on the 4352/4360 die); the
 * board may wire fewer, per the SROM rxchain mask. Save 0x401/0x400, drive
 * the sequencer mode bits for the mask, fire rfseq cmd 0 (trigger 0x01) then
 * cmd 1 (0x02), restore.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   6907-6946]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2577-2616]
 */
static void b43_phy_ac_rxcore_setstate(struct b43_wldev *dev, u8 coremask)
{
	/* Core 0's RX-gain override, saved across the bracket. */
	u16 rxgain_ovr;
	B43_AC_FN();
	u16 saved_401, saved_400;

	saved_401 = b43_phy_read_log(dev, B43_PHY_AC_RF_SEQ_MODE);
	saved_400 = b43_phy_read_log(dev, B43_PHY_AC_RFCTL1);
	rxgain_ovr = b43_phy_read_log(dev, 0x06d8);

	/*
	 * RX-gain override companion of 0x06d8, bracketing the core-state
	 * change: forced wide open (0xffff) before, set back to what 0x06d8
	 * read after. The capture delays 5848 us after the wide-open write.
	 */
	b43_phy_write(dev, 0x16d8, 0xffff);
	udelay(5850);

	b43_phy_maskset(dev, 0x0160, (u16)~0x0007, coremask);
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x0070,
			(u16)(coremask << 4));
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x7000, 0x7000);
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x0007, 0x0000);
	b43_phy_maskset(dev, B43_PHY_AC_RFCTL1, (u16)~0x0001, 0x0001);

	b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RX2TX);
	b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_TX2RX);

	/*
	 * Restore, in the stock driver's order: MOD ~0x0007, MOD ~0x7000, then
	 * 0x0400. The low field goes back to the coremask, not to the saved
	 * value: the capture reads 0x7777 and writes 0x0003.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x0007, coremask);
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x7000,
			saved_401 & 0x7000);
	b43_phy_write(dev, B43_PHY_AC_RFCTL1, saved_400);

	b43_phy_write(dev, 0x16d8, rxgain_ovr);
}

/*
 * RF-sequencer command opcodes, as stored in the CMD rows of table 7. These
 * are distinct from the RF_SEQ_TRIG trigger bits in phy_ac.h.
 *
 * The low opcodes and the END sentinel are the N-PHY rev3 encoding
 * (brcmsmac phyreg_n.h, NPHY_REV3_RFSEQ_CMD_*): the rx2tx and tx2rx orders
 * match rev3's event lists one for one, and the sentinel is rev3's 0x1f
 * (rev < 3 used 0xf).
 */
enum b43_phy_ac_rfseq_cmd {
	B43_AC_RFSEQ_NOP		= 0x00,
	B43_AC_RFSEQ_RXG_FBW		= 0x01,
	B43_AC_RFSEQ_TR_SWITCH		= 0x02,
	B43_AC_RFSEQ_INT_PA_PU		= 0x03,
	B43_AC_RFSEQ_EXT_PA		= 0x04,
	B43_AC_RFSEQ_RXPD_TXPD		= 0x05,
	B43_AC_RFSEQ_TX_GAIN		= 0x06,
	B43_AC_RFSEQ_RX_GAIN		= 0x07,
	B43_AC_RFSEQ_CLR_HIQ_DIS	= 0x08,

	/*
	 * The HPC-set opcodes are taken from the same rev3 map by analogy, not
	 * pinned by an ordering. Only the LPF (even) ones occur; rev3's HPF
	 * (odd) siblings 0x09/0x0b/0x0d are unused here.
	 */
	B43_AC_RFSEQ_SET_LPF_H_HPC	= 0x0a,
	B43_AC_RFSEQ_SET_LPF_M_HPC	= 0x0c,
	B43_AC_RFSEQ_SET_LPF_L_HPC	= 0x0e,

	B43_AC_RFSEQ_CLR_RXRX_BIAS	= 0x0f,
	B43_AC_RFSEQ_END		= 0x1f,

	/*
	 * AC-only opcodes with no N-PHY counterpart and no known meaning.
	 * 0xb0-0xb2 carry a per-core second word and take 0x2b's slot in the 80
	 * MHz variant.
	 */
	B43_AC_RFSEQ_UNK_2A		= 0x2a,
	B43_AC_RFSEQ_UNK_2B		= 0x2b,
	B43_AC_RFSEQ_UNK_35		= 0x35,
	B43_AC_RFSEQ_UNK_36		= 0x36,
	B43_AC_RFSEQ_UNK_B0		= 0xb0,
	B43_AC_RFSEQ_UNK_B1		= 0xb1,
	B43_AC_RFSEQ_UNK_B2		= 0xb2,
};

static const u16 b43_acphy_rfseq_rx2tx_cmd[16] = {
	B43_AC_RFSEQ_NOP,	  B43_AC_RFSEQ_RXG_FBW,	    B43_AC_RFSEQ_TR_SWITCH,
	B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_RXPD_TXPD,   B43_AC_RFSEQ_NOP,
	B43_AC_RFSEQ_TX_GAIN,	  B43_AC_RFSEQ_INT_PA_PU,   B43_AC_RFSEQ_CLR_RXRX_BIAS,
	B43_AC_RFSEQ_EXT_PA,	  B43_AC_RFSEQ_NOP,	    B43_AC_RFSEQ_UNK_35,
	B43_AC_RFSEQ_CLR_RXRX_BIAS, B43_AC_RFSEQ_NOP,	    B43_AC_RFSEQ_UNK_36,
	B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_tx2rx_cmd[16] = {
	B43_AC_RFSEQ_EXT_PA,	  B43_AC_RFSEQ_INT_PA_PU,   B43_AC_RFSEQ_TX_GAIN,
	B43_AC_RFSEQ_RXPD_TXPD,	  B43_AC_RFSEQ_NOP,	    B43_AC_RFSEQ_TR_SWITCH,
	B43_AC_RFSEQ_RXG_FBW,	  B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_UNK_2A,
	B43_AC_RFSEQ_CLR_RXRX_BIAS, B43_AC_RFSEQ_NOP,	    B43_AC_RFSEQ_CLR_RXRX_BIAS,
	B43_AC_RFSEQ_UNK_2B,	  B43_AC_RFSEQ_END,	    B43_AC_RFSEQ_END,
	B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_reset2rx_cmd[16] = {
	B43_AC_RFSEQ_EXT_PA,	  B43_AC_RFSEQ_INT_PA_PU,   B43_AC_RFSEQ_TX_GAIN,
	B43_AC_RFSEQ_RXPD_TXPD,	  B43_AC_RFSEQ_TR_SWITCH,   B43_AC_RFSEQ_RXG_FBW,
	B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_UNK_2A,	    B43_AC_RFSEQ_UNK_2B,
	B43_AC_RFSEQ_CLR_RXRX_BIAS, B43_AC_RFSEQ_END,	    B43_AC_RFSEQ_END,
	B43_AC_RFSEQ_END,	  B43_AC_RFSEQ_END,	    B43_AC_RFSEQ_END,
	B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_reset2rx_dly[16] = { 0x000c, 0x0002, 0x0002, 0x0004, 0x0004, 0x0006, 0x0001, 0x0004, 0x0001, 0x0002, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001 };
static const u16 b43_acphy_rfseq_updl_lpf_hpc[2] = { 0x0aaa, 0x0aaa };
static const u16 b43_acphy_rfseq_updl_tia_hpc[2] = { 0x0222, 0x0222 };

/*
 * The per-core rfseq second setup in table 0x07: six table writes of length
 * 8, CMD and DLY for each of the three silicon cores, at a +0x10 stride.
 * Emitted for every silicon core, not filtered by the coremask.
 */
static const u16 b43_acphy_rfseq_2_cmd_c0[8] = {
	B43_AC_RFSEQ_UNK_2A, B43_AC_RFSEQ_RX_GAIN, B43_AC_RFSEQ_SET_LPF_H_HPC,
	B43_AC_RFSEQ_NOP, B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_UNK_2B,
	B43_AC_RFSEQ_END, B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_2_dly_c0[8] = {
	0x0001, 0x0002, 0x0002, 0x0002, 0x0010, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c1[8] = {
	B43_AC_RFSEQ_UNK_2A, B43_AC_RFSEQ_RX_GAIN, B43_AC_RFSEQ_CLR_HIQ_DIS,
	B43_AC_RFSEQ_SET_LPF_M_HPC, B43_AC_RFSEQ_SET_LPF_L_HPC, B43_AC_RFSEQ_UNK_2B,
	B43_AC_RFSEQ_END, B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_2_dly_c1[8] = {
	0x0001, 0x0006, 0x0012, 0x0008, 0x0010, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c2[8] = {
	B43_AC_RFSEQ_UNK_2A, B43_AC_RFSEQ_RX_GAIN, B43_AC_RFSEQ_CLR_HIQ_DIS,
	B43_AC_RFSEQ_SET_LPF_L_HPC, B43_AC_RFSEQ_UNK_2B, B43_AC_RFSEQ_END,
	B43_AC_RFSEQ_END, B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_2_dly_c2[8] = {
	0x0001, 0x0006, 0x001e, 0x001c, 0x0001, 0x0001, 0x0001, 0x0001,
};

/*
 * Same block at 80 MHz: the leading 0x2a of the narrow variant is gone,
 * 0xb0 with a per-core second word replaces 0x2b, and that delay slot drops
 * from 0x10 to 0xa. 20 and 40 MHz share the narrow variant.
 */
static const u16 b43_acphy_rfseq_2_cmd_c0_bw80[8] = {
	B43_AC_RFSEQ_RX_GAIN, B43_AC_RFSEQ_SET_LPF_H_HPC, B43_AC_RFSEQ_NOP,
	B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_UNK_B0, B43_AC_RFSEQ_UNK_B1,
	B43_AC_RFSEQ_END, B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_2_dly_c0_bw80[8] = {
	0x0002, 0x0002, 0x0002, 0x0001, 0x000a, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c1_bw80[8] = {
	B43_AC_RFSEQ_RX_GAIN, B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_SET_LPF_M_HPC,
	B43_AC_RFSEQ_SET_LPF_L_HPC, B43_AC_RFSEQ_UNK_B0, B43_AC_RFSEQ_UNK_B2,
	B43_AC_RFSEQ_END, B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_2_dly_c1_bw80[8] = {
	0x0006, 0x0012, 0x0008, 0x0001, 0x000a, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c2_bw80[8] = {
	B43_AC_RFSEQ_RX_GAIN, B43_AC_RFSEQ_CLR_HIQ_DIS, B43_AC_RFSEQ_SET_LPF_L_HPC,
	B43_AC_RFSEQ_UNK_B0, B43_AC_RFSEQ_UNK_B1, B43_AC_RFSEQ_END,
	B43_AC_RFSEQ_END, B43_AC_RFSEQ_END,
};
static const u16 b43_acphy_rfseq_2_dly_c2_bw80[8] = {
	0x0006, 0x001e, 0x001c, 0x000a, 0x0001, 0x0001, 0x0001, 0x0001,
};


static const u32 b43_acphy_txv_for_spexp[243] = {
	0x4009d1bb, 0x8013a376, 0x002746ec, 0x00000131, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00009700, 0xdb2540c0,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x0000b64a, 0xec3023ac, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00000069, 0x003400a5, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00004a4a, 0x1430ddac,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00006900, 0x2525c0c0, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00004ab6, 0x3014acdd, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00000097, 0x3400a500,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x0000b6b6, 0x30ecac23, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00009700, 0x25dbc040, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x0000b64a, 0x14d0dd54,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00000069, 0x00cc005b, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00004a4a, 0xecd02354, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00006900, 0xdbdb4040,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00004ab6, 0xd0ec5423, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00000097, 0xcc005b00, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x0000b6b6, 0xd01454dd,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00009700, 0xdb2540c0, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x0000b64a, 0xec3023ac, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00000069, 0x003400a5,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00004a4a, 0x1430ddac, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00006900, 0x2525c0c0, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00004ab6, 0x3014acdd,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00000097, 0x3400a500, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x0000b6b6, 0x30ecac23, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00009700, 0x25dbc040,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x0000b64a, 0x14d0dd54, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00000069, 0x00cc005b, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x0034005b, 0x00000000, 0x00004a4a, 0xecd02354,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0034005b, 0x00000000,
	0x00006900, 0xdbdb4040, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0034005b, 0x00000000, 0x00004ab6,
};

/*
 * FEM control table: table 0x0a, 32 bytes per core at 0x20 * core.
 *
 * The table is chosen per core by femctrl and femctrl_sub (the low three
 * bits of boardflags3), and written with the PHY tables on reset. Nothing
 * rewrites it on a channel or band change: the MacBookAir6,1 capture keeps
 * it through 302 chanspecs on both bands. The tables are the fectrl_<name>
 * .rodata objects of wlD6220.o (7.14.89).
 *
 * Only the pairs a capture shows are here, since a FEM driven with another
 * one's table gets the PA enable or the T/R switch wrong:
 *
 *   femctrl 6, sub 0  femctrl6 on every core: d6220, DSL-3580L, agcombo,
 *                     tg789vac.
 *   femctrl 2, sub 1  fem5516_fc1 on cores 0 and 2, x29c_c1_fc2_sub1 on
 *                     core 1, plus b43_phy_ac_fem2_sub1_setup():
 *                     MacBookAir6,1.
 *
 * TODO: femctrl 2 sub 2 (femctrl2_sub2_c0 on core 0, femctrl2_sub2_c12 on
 * cores 1 and 2) needs a capture of a board with it.
 */
#define B43_BFL3_FEMCTRL_SUB	0x00000007

static const u8 b43_phy_ac_fem_femctrl6[32] = {
	0x00, 0x00, 0x06, 0x02,  0x00, 0x00, 0x06, 0x02,
	0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x06, 0x02,  0x00, 0x00, 0x06, 0x02,
	0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

static const u8 b43_phy_ac_fem_fem5516_fc1[32] = {
	0x00, 0x00, 0x04, 0x00,  0x00, 0x00, 0x04, 0x00,
	0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x00,  0x00, 0x00, 0x04, 0x00,
	0x00, 0x02, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

static const u8 b43_phy_ac_fem_x29c_c1_fc2_sub1[32] = {
	0x00, 0x00, 0x30, 0x20,  0x00, 0x00, 0x30, 0x20,
	0x00, 0x80, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x40, 0x40, 0x46, 0x42,  0x40, 0x40, 0x46, 0x42,
	0x40, 0x41, 0x40, 0x40,  0x40, 0x40, 0x40, 0x40,
};

struct b43_phy_ac_fem {
	u8 femctrl, sub;
	const u8 *tbl[3];
	void (*setup)(struct b43_wldev *dev);
};

/*
 * What the MacBookAir6,1 writes between its FEM table and the release of the
 * table-write gate, where the bus shows a value change. It also reads and
 * writes back unchanged 0x0418, 0x040a and the three chipcommon GPIO
 * registers after 0x0414; with no change visible, those are left out
 * (docs/retrace-todo.md, "2.4 GHz").
 */
static void b43_phy_ac_fem2_sub1_setup(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_set(dev, 0x040a, 0x0100);
	b43_phy_write(dev, 0x0414, 0x0555);
	bcma_cc_set32(&dev->dev->bdev->bus->drv_cc, BCMA_CC_CHIPCTL, 0x00000008);
}

static const struct b43_phy_ac_fem b43_phy_ac_fems[] = {
	{ 6, 0, { b43_phy_ac_fem_femctrl6, b43_phy_ac_fem_femctrl6,
		  b43_phy_ac_fem_femctrl6 }, NULL },
	{ 2, 1, { b43_phy_ac_fem_fem5516_fc1, b43_phy_ac_fem_x29c_c1_fc2_sub1,
		  b43_phy_ac_fem_fem5516_fc1 }, b43_phy_ac_fem2_sub1_setup },
};

static const struct b43_phy_ac_fem *b43_phy_ac_fem_lookup(struct b43_wldev *dev)
{
	struct ssb_sprom *sprom = dev->dev->bus_sprom;
	u8 sub = sprom->boardflags3 & B43_BFL3_FEMCTRL_SUB;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_fems); i++)
		if (b43_phy_ac_fems[i].femctrl == sprom->femctrl &&
		    b43_phy_ac_fems[i].sub == sub)
			return &b43_phy_ac_fems[i];
	return NULL;
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5272-5386]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   942-1056]
 * [capture-ref: router-data/macbookair6-1/wl-firstload-20260926-190438.trace.xz;
 *   #7287-#7426]
 */
static void b43_phy_ac_set_regtbl_on_femctrl(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct ssb_sprom *sprom = dev->dev->bus_sprom;
	const struct b43_phy_ac_fem *fem = b43_phy_ac_fem_lookup(dev);
	u8 core;
	u16 saved;

	if (!fem) {
		b43warn(dev->wl,
			"AC-PHY: femctrl=%u femctrl_sub=%u not supported: no "
			"FEM control table verified for it, so none is "
			"loaded; another board's table could damage the "
			"front end\n",
			sprom->femctrl, sprom->boardflags3 & B43_BFL3_FEMCTRL_SUB);
		return;
	}

	/* Verified only on phy rev 1. */
	if (dev->phy.rev != 1)
		return;

	saved = b43_phy_ac_tbl_write_lock(dev);

	for (core = 0; core < min_t(u8, dev->phy.ac->num_cores,
				    ARRAY_SIZE(fem->tbl)); core++)
		b43_actab_write_bulk(dev, 0x0a, 0x20 * core, 8, 32,
				     fem->tbl[core]);

	if (fem->setup)
		fem->setup(dev);

	/*
	 * Bluetooth coexistence: the MacBookAir6,1, whose boardflags have it,
	 * sets bit 24 of chipcommon chipcontrol here; the routers do not.
	 */
	if (sprom->boardflags_lo & B43_BFL_BTCOEXIST)
		bcma_cc_set32(&dev->dev->bdev->bus->drv_cc, BCMA_CC_CHIPCTL,
			      0x01000000);

	b43_phy_ac_tbl_write_unlock(dev, saved);
}

/*
 * Analog TX-LPF setup.
 *
 * For each active core and each LPF stage selected by @stages (bit i =>
 * stage i, up to 9), read-modify-write the {lo,hi} table-7 pair holding the
 * 25-bit TX-LPF word: lo at {0x142,0x152,0x162} + stage, hi at
 * {0x362,0x372,0x382} + stage. A sub-field is rewritten only when its
 * argument is >= 0. @only_core restricts the work to one core, 0xffffffff
 * for all.
 *
 * The RMW keeps the per-stage base the table init loaded and rewrites the
 * cap field (f9/f17), which comes from rccal and is a per-unit measurement.
 * Bases, formula and verification: docs/txlpf-formula.md.
 *
 * TODO: the second reset path that drives this helper (stages 0x100,
 * f0=f6=<bw value>, only the bw fields) is not implemented.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5390-5857, 7270-7321]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   1060-1527, 2940-2991]
 */
static void b43_phy_ac_set_analog_tx_lpf_locked(struct b43_wldev *dev,
						u16 stages,
						int f0, int f6, int f3,
						int f9, int f17,
						u32 only_core)
{
	B43_AC_FN();
	static const u16 lo_off[3] = { 0x142, 0x152, 0x162 };
	static const u16 hi_off[3] = { 0x362, 0x372, 0x382 };
	u8 core, num_cores = dev->phy.ac->num_cores;

	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		u16 off_lo, off_hi;
		unsigned int stage;

		if (only_core != 0xffffffff && core != only_core)
			continue;

		off_lo = lo_off[core];
		off_hi = hi_off[core];

		for (stage = 0; stage < 9; stage++, off_lo++, off_hi++) {
			u16 lo, hi;
			u32 v;

			if (!(stages & (1 << stage)))
				continue;

			b43_actab_read_bulk(dev, 7, off_lo, 16, 1, &lo);
			b43_actab_read_bulk(dev, 7, off_hi, 16, 1, &hi);
			v = ((u32)hi << 16) | lo;

			if (f0  >= 0) v = (u32)f0 | (v & 0x1fffff8);
			if (f3  >= 0) v = (v & 0x1ffffc7) | ((u32)f3 << 3);
			if (f6  >= 0) v = (v & 0x1fffe3f) | ((u32)f6 << 6);
			if (f9  >= 0) v = (v & 0x1fe01ff) | ((u32)f9 << 9);
			if (f17 >= 0) v = (v & 0x1ffff)   | ((u32)f17 << 17);

			lo = (u16)v;
			hi = (u16)(v >> 16) & 0x1ff;

			b43_actab_write_bulk(dev, 7, off_lo, 16, 1, &lo);
			b43_actab_write_bulk(dev, 7, off_hi, 16, 1, &hi);
		}
	}
}

static void b43_phy_ac_set_analog_tx_lpf(struct b43_wldev *dev, u16 stages,
					 int f0, int f6, int f3, int f9,
					 int f17, u32 only_core)
{
	u16 saved = b43_phy_ac_tbl_write_lock(dev);

	b43_phy_ac_set_analog_tx_lpf_locked(dev, stages, f0, f6, f3, f9, f17,
					    only_core);

	b43_phy_ac_tbl_write_unlock(dev, saved);
}

/*
 * Wait for the RF sequencer to clear @busy in 0x0403: the bit of the command
 * kicked in 0x0402, bit 0 for the sample play. One read, then 10 us and
 * another while busy, for at most @turns reads.
 */
static bool b43_phy_ac_rfseq_wait_done(struct b43_wldev *dev, u16 busy,
				       unsigned int turns)
{
	while (turns--) {
		if (!(b43_phy_read_log(dev, B43_PHY_AC_RF_SEQ_STATUS) & busy))
			return true;
		udelay(10);
	}
	return false;
}

static void b43_phy_ac_chan_tables(struct b43_wldev *dev);

static void b43_phy_ac_rx_evm_shaping_override(struct b43_wldev *dev);

/*
 * The chanspec of a configuration, in the layout of B43_PHY_AC_CHANSPEC_*.
 *
 * The centre is the primary at 20 MHz and center_freq1 otherwise; the
 * sideband is the primary's distance from the lowest channel of the block,
 * in steps of four channels.
 */
static u16 b43_phy_ac_chanspec(const struct cfg80211_chan_def *chandef)
{
	const struct ieee80211_channel *chan = chandef->chan;
	int prim = chan->hw_value;
	int centre = prim;
	int half = 0;
	u16 spec;

	switch (chandef->width) {
	case NL80211_CHAN_WIDTH_80:
		spec = B43_PHY_AC_CHANSPEC_BW80;
		half = 6;
		break;
	case NL80211_CHAN_WIDTH_40:
		spec = B43_PHY_AC_CHANSPEC_BW40;
		half = 2;
		break;
	default:
		spec = B43_PHY_AC_CHANSPEC_BW20;
		break;
	}
	if (half)
		centre += ((int)chandef->center_freq1 - (int)chan->center_freq) / 5;

	spec |= centre;
	spec |= ((prim - (centre - half)) / 4) << B43_PHY_AC_CHANSPEC_SB_SHIFT;
	if (chan->band == NL80211_BAND_5GHZ)
		spec |= B43_PHY_AC_CHANSPEC_BAND_5G;
	return spec;
}

/*
 * Write the chanspec the ucode reads out of shared memory.
 *
 * On the AC-PHY the core leaves B43_SHM_SH_CHAN to the PHY, which writes it
 * at the head of the RF bring-up and, when it changes, at the head of a
 * channel switch, as the stock driver does.
 *
 * The values come from phy.chandef, not from the cal_* fields: at the head
 * of the RF bring-up no channel setup has run yet, so cal_* are stale or
 * zero. b43_phy_init() points phy.chandef at the hardware config before
 * switch_analog() and b43_software_rfkill() (which calls this), and
 * b43_op_config() repoints it on every channel change.
 *
 * Right after the chanspec the stock driver tells the MAC the width when it
 * changed (MAC.BW in the wl-diag captures); b43_mac_bw_set() does the same.
 */
void b43_phy_ac_write_chanspec(struct b43_wldev *dev)
{
	const struct cfg80211_chan_def *chandef = dev->phy.chandef;
	struct b43_phy_ac *ac = dev->phy.ac;
	u16 spec = b43_phy_ac_chanspec(chandef);

	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_CHAN, spec);
	ac->chanspec = spec;

	if (ac->mac_width != chandef->width) {
		b43_mac_bw_set(dev, spec & B43_PHY_AC_CHANSPEC_BW_MASK);
		ac->mac_width = chandef->width;
	}
}

static void b43_phy_ac_chanspec_tail(struct b43_wldev *dev);

/*
 * The block that follows rfseq_tbl_init: quiesce the unwired RX cores, lock
 * the table-write gate and write 0x01ec = 0x9c40, whose meaning is not
 * known. Returns the lock's saved value; the caller unlocks.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   6905-6950]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2575-2620]
 */
static u16 b43_phy_ac_post_rfseq_misc_setup(struct b43_wldev *dev)
{
	B43_AC_FN();
	u16 gate;

	/* Diagnostic peek. */
	b43_phy_read_log(dev, 0x000b);

	/*
	 * num_cores is 0x000b & 0x07, 3 on both the 4352 and the 4360; the
	 * coremask comes from the SROM rxchain. Only when they disagree: the
	 * agcombo (rxchain 7) has no write of 0x16d8 at all, the d6220 (rxchain
	 * 3) has the sequence.
	 */
	if (hweight8(dev->phy.ac->coremask) != dev->phy.ac->num_cores)
		b43_phy_ac_rxcore_setstate(dev, dev->phy.ac->coremask);

	gate = b43_phy_ac_tbl_write_lock(dev);
	b43_phy_write(dev, 0x01ec, 0x9c40);
	return gate;
}

/*
 * Per-core radio setup, step 1, the same on the d6220 and the agcombo:
 *   1. once, program 0x0548-0x054c (probably an LO calibration output
 *      buffer) and clear bit 0 of 0x040b;
 *   2. per core, seven masksets: 0x001a three times, the shared 0x054b/c,
 *      then 0x0017, 0x001f and 0x0170, at a +0x200 stride.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7090-7157]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2760-2827]
 */
static void b43_phy_ac_radio_percore_setup_1(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int core;
	unsigned int num_cores = dev->phy.ac->num_cores;

	b43_radio_maskset(dev, 0x0548, (u16)~0x0001, 0x0001);
	b43_radio_write(dev, 0x0549, 0x0000);
	b43_radio_write(dev, 0x054a, 0x0000);
	b43_radio_write(dev, 0x054b, 0x0000);
	b43_radio_write(dev, 0x054c, 0x0000);
	b43_radio_maskset(dev, 0x040b, (u16)~0x0001, 0x0000);

	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		u16 stride = (u16)(core * 0x200);
		/*
		 * 0x054b and 0x054c hold one byte per core: core 0 is 0x054b
		 * high, core 1 0x054b low, core 2 0x054c high (seen on the 3x3
		 * agcombo).
		 */
		u16 sh_reg  = (u16)(0x054b + core / 2);
		u16 sh_mask = (core & 1) ? 0x00ff : 0xff00;
		u16 sh_val  = (core & 1) ? 0x0001 : 0x0100;

		b43_radio_maskset(dev, 0x001a + stride,
				  (u16)~0x00f0, 0x0010);
		b43_radio_maskset(dev, 0x001a + stride,
				  (u16)~0x0004, 0x0004);
		b43_radio_maskset(dev, sh_reg, (u16)~sh_mask, sh_val);
		b43_radio_maskset(dev, 0x001a + stride,
				  (u16)~0x0300, 0x0000);
		b43_radio_maskset(dev, 0x0017 + stride,
				  (u16)~0x0002, 0x0000);
		b43_radio_maskset(dev, 0x001f + stride,
				  (u16)~0x0004, 0x0000);
		b43_radio_maskset(dev, 0x0170 + stride,
				  (u16)~0x0100, 0x0100);
	}
}

/*
 * Bandwidth in MHz, used as a scale factor: a number of PHY parameters are
 * sample counts or durations and double as the bandwidth doubles.
 */
static unsigned int b43_phy_ac_bw_mhz(struct b43_wldev *dev)
{
	switch (dev->wl->hw->conf.chandef.width) {
	case NL80211_CHAN_WIDTH_80:
		return 80;
	case NL80211_CHAN_WIDTH_40:
		return 40;
	default:
		return 20;
	}
}

/*
 * Coefficient bank init, 48 ops. It is the same across channels within
 * 5 GHz at a given width (d6220 ch36/ch44) and across chips (agcombo and
 * d6220 ch36), and differs per width: one LUT per width, each constant over
 * every cold segment of that width. 2 GHz has none: no capture, and
 * switch_channel does not reach this on that band.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7220-7269]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2890-2939]
 */
static void b43_phy_ac_coeff_bank_init(struct b43_wldev *dev)
{
	B43_AC_FN();
	const unsigned int bw = b43_phy_ac_bw_mhz(dev);
	static const u16 lut_bw20[21] = {
		/* 0x0180 */ 0x0015,  /* mask=0x001f, the rest 0x07ff */
		/* 0x0181 */ 0x0146,
		/* 0x0182 */ 0x0088,
		/* 0x0183 */ 0x0146,
		/* 0x0184 */ 0x076e,
		/* 0x0185 */ 0x01a8,
		/* 0x0186 */ 0x00a3,
		/* 0x0187 */ 0x00f4,
		/* 0x0188 */ 0x00a3,
		/* 0x0189 */ 0x0684,
		/* 0x018a */ 0x00ad,
		/* 0x018b */ 0x00e5,
		/* 0x018c */ 0x0068,
		/* 0x018d */ 0x00e5,
		/* 0x018e */ 0x06be,
		/* 0x018f */ 0x019e,
		/* 0x0190 */ 0x0073,
		/* 0x0191 */ 0x00b2,
		/* 0x0192 */ 0x0073,
		/* 0x0193 */ 0x05fe,
		/* 0x0194 */ 0x00cc,
	};
	static const u16 lut_bw40[21] = {
		0x000b, 0x0181, 0x005a, 0x0181, 0x0793, 0x01b7, 0x00c1,
		0x0102, 0x00c1, 0x06c0, 0x00a9, 0x0162, 0x0042, 0x0162,
		0x075c, 0x01b3, 0x00b1, 0x00ed, 0x00b1, 0x0692, 0x00af,
	};
	static const u16 lut_bw80[21] = {
		0x0005, 0x017a, 0x009e, 0x017a, 0x07ca, 0x01b2, 0x00bd,
		0x0114, 0x00bd, 0x06d6, 0x00a2, 0x016c, 0x006f, 0x016c,
		0x0793, 0x01b2, 0x00b6, 0x00ff, 0x00b6, 0x06b4, 0x00a8,
	};
	const u16 *lut = (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80)
		? lut_bw80
		: (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40) ? lut_bw40
								    : lut_bw20;
	unsigned int i;
	unsigned int core;
	unsigned int num_cores = dev->phy.ac->num_cores;

	/*
	 * Called from channel_setup(): channel_switch_prep() has frozen RX
	 * (RX_WAITED plus CLIP_ALL_DIS) and the MAC is still enabled.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET);

	/*
	 * 1. Bandwidth selector: the field in 0x0076 is the width index (1 at
	 * 20 MHz, 2 at 40, 3 at 80), and bit 11 of 0x0140 and bit 4 of 0x0164
	 * are set at 20 MHz and clear on a bonded channel. All 26 cold segments
	 * agree.
	 */
	{
		u16 idx = (u16)(b43_phy_ac_bw_step(dev) + 1);
		u16 narrow = (idx == 1) ? ~0 : 0;

		b43_phy_maskset(dev, 0x0076, (u16)~0x0007, idx);
		b43_phy_maskset(dev, 0x0140, (u16)~0x0800, 0x0800 & narrow);
		b43_phy_maskset(dev, 0x0164, (u16)~0x0010, 0x0010 & narrow);
	}

	/* 2. LUT 0x0180-0x0194: mask 0x001f on 0x0180, 0x07ff on the rest. */
	b43_phy_maskset(dev, 0x0180, (u16)~0x001f, lut[0]);
	for (i = 1; i < 21; i++)
		b43_phy_maskset(dev, (u16)(0x0180 + i),
				(u16)~0x07ff, lut[i]);

	/*
	 * 3. Extra register setup. Three depend on the bandwidth (D6220 sweep,
	 * 26 configurations over 52 segments):
	 *
	 *              20 MHz   40 MHz   80 MHz
	 *   0x0250        25       50       50
	 *   0x0262       200      400      400
	 *   0x0263        25       50       50
	 *
	 * They saturate at 40 MHz. The DSL captures show a pure doubling to
	 * 25/50/100 instead: the structure carries across boards, the values
	 * are the d6220's.
	 *
	 * 0x0261 follows the same pattern, 0x14 at 20 MHz and 0x28 from 40 on;
	 * 0x01b5 is 0x8b at 40 MHz and 0x97 at 20 and 80. 0x025b is written
	 * only by the DSL, not by the d6220.
	 */
	b43_phy_maskset(dev, 0x01b5, (u16)~0x00ff, bw == 40 ? 0x008b : 0x0097);
	b43_phy_maskset(dev, 0x0250, (u16)~0x00ff, bw == 20 ? 25 : 50);
	b43_phy_maskset(dev, 0x0261, (u16)~0x0fff, bw == 20 ? 0x0014 : 0x0028);
	b43_phy_maskset(dev, 0x0262, (u16)~0x0fff, bw == 20 ? 200 : 400);
	b43_phy_maskset(dev, 0x0263, (u16)~0x0fff, bw == 20 ? 25 : 50);
	/*
	 * These two drop at 80 MHz only: 0x13 at 20 and 40 MHz, 0x09 at 80, in
	 * the low byte of 0x0312 and the high byte of 0x0313.
	 */
	b43_phy_maskset(dev, 0x0312, (u16)~0x00ff, bw == 80 ? 0x0009 : 0x0013);
	b43_phy_maskset(dev, 0x0313, (u16)~0xff00, bw == 80 ? 0x0900 : 0x1300);

	/* 4. Per-core LUT 0x06ed/0x06ef, every silicon core. */
	for (core = 0; core < num_cores; core++) {
		u16 stride = (u16)(core * 0x200);

		/* 0x0a at 20 MHz, 0x14 from 40 on. */
		b43_phy_maskset(dev, 0x06ed + stride,
				(u16)~0x00ff, bw == 20 ? 0x000a : 0x0014);
		/*
		 * In the D6220 sweep 0x06ef[7:0] takes two values per
		 * bandwidth, so there are two writes:
		 *
		 *              20 MHz   40 MHz   80 MHz
		 *   this one     0x17     0x2a     0x54     (23 / 42 / 84)
		 *   step 6       0x0f     0x1e     0x3c     (15 / 30 / 60)
		 *
		 * Only the second doubles.
		 */
		b43_phy_maskset(dev, 0x06ef + stride, (u16)~0x00ff,
				bw == 20 ? 0x0017 : (bw == 40 ? 0x002a : 0x0054));
		/* The high byte: 0x0e, 0x16, 0x2c. */
		b43_phy_maskset(dev, 0x06ef + stride, (u16)~0xff00,
				bw == 20 ? 0x0e00
					 : (bw == 40 ? 0x1600 : 0x2c00));
	}

	/* 5. Per-core 0x06ef[7:0], second write: 15 / 30 / 60. */
	for (core = 0; core < num_cores; core++) {
		u16 stride = (u16)(core * 0x200);

		b43_phy_maskset(dev, 0x06ef + stride, (u16)~0x00ff,
				(u16)(15 * (bw / 20)));
	}
}

/*
 * Power-detector setup: 14 one-shot writes. @full selects the long variant,
 * which includes 0x0358-0x035a.
 *
 * Not per-core: no capture shows 0x0750/0x0950/0x0b50. The values are
 * transcribed; the identical pairs (1000/1000, 500/500) look like settle
 * windows rather than gain codes. 0x0554 and 0x0555 are adjusted later by
 * the periodic watchdog; see b43_phy_ac_op_pwork_60sec().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   1202-1214, 5253-5268]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   568-580, 923-938]
 */
static void b43_phy_ac_set_pdet_on_reset(struct b43_wldev *dev, bool full)
{
	B43_AC_FN();
	/*
	 * The stock driver reads 0x0550 before overwriting it, at both call
	 * sites (op_init and set_channel); where the value goes is not known.
	 */
	b43_phy_read_log(dev, 0x0550);
	b43_phy_write(dev, 0x0550, 0x0ffd);
	b43_phy_maskset(dev, 0x0551, (u16)~0x000f, 0x000f);
	b43_phy_maskset(dev, 0x0551, (u16)~0x00f0, 0x00f0);
	b43_phy_write(dev, 0x0552, 0x0001);
	b43_phy_write(dev, 0x0553, 0x0001);
	b43_phy_write(dev, 0x0554, 0x03e8);
	b43_phy_write(dev, 0x0555, 0x03e8);
	b43_phy_write(dev, 0x0556, 0x01f4);
	b43_phy_write(dev, 0x0557, 0x01f4);
	b43_phy_write(dev, 0x0558, 0xb8d8);
	b43_phy_write(dev, 0x0559, 0x0005);
	if (!full)
		return;
	b43_phy_write(dev, 0x0358, 0xc07f);
	b43_phy_write(dev, 0x0359, 0x0064);
	b43_phy_write(dev, 0x035a, 0x0064);
}

/*
 * The low word of the RX-LPF cell in table 7, by operating width (20, 40,
 * 80 MHz) and core.
 */
static const u16 b43_phy_ac_rxlpf_lo_cell[3][3] = {
	{ 0x140, 0x150, 0x160 },
	{ 0x141, 0x151, 0x161 },
	{ 0x441, 0x443, 0x445 },
};

/*
 * Analog reset-time sub-setups, run under the caller's table-write lock:
 * FEM control, TX-LPF, TX AFE dacbuf cap, RX-LPF, in the stock driver's
 * order. Verified on phy rev 1 only.
 *
 * The LPF and dacbuf caps are those b43_radio_2069_rccal() measured, from
 * op_software_rfkill, before the first channel setup that reaches here.
 *
 * The per-core loops use the active-core mask (SROM rxchain), which on the
 * sampled boards equals the PHY TX/RX core mask; a board with
 * phytxchain != rxchain needs this revisited.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5253-6264]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   923-1934]
 */
static void b43_phy_ac_analog_on_reset(struct b43_wldev *dev, u16 *saved_outer_out)
{
	B43_AC_FN();
	struct b43_phy_ac *aphy = dev->phy.ac;
	u8 core, num_cores = dev->phy.ac->num_cores;
	u16 saved;

	b43_phy_ac_set_pdet_on_reset(dev, true);

	/*
	 * Outer table-write gate: taken here, released by the caller after
	 * rfseq_tbl_init(), with no intermediate unlock in the stock sequence.
	 * The nested sub-locks emit idempotent masksets, as in the capture.
	 */
	*saved_outer_out = b43_phy_ac_tbl_write_lock(dev);

	b43_phy_ac_set_regtbl_on_femctrl(dev);

	/*
	 * TX-LPF stage entries in table 7, right after the idempotent close of
	 * set_regtbl_on_femctrl().
	 */
	b43_phy_ac_set_analog_tx_lpf(dev, 0x1ff, -1, -1, -1,
				     aphy->lpf_cap0, aphy->lpf_cap1, 0xffffffff);


	/*
	 * TX AFE dacbuf cap: dacbuf_cap into the 6-bit cap field (bits 0..5 or
	 * 6..11 by stage) of all 9 stages on every active core, keeping the
	 * base the cell carries (0x0b20 for stages 0-7, 0x0020 for stage 8).
	 * dacbuf_cap is (RCCAL_G & 0x03e0) >> 5: DSL RCCAL_G=0x0186 gives
	 * 0x0b2c, agcombo 0x1a8 gives 0x0b2d, the d6220 writes 0x0b2e.
	 */
	{
		static const u16 base[3]  = { 0x3f0, 0x60, 0xd0 };
		static const u8  add[9]   = { 0xb, 0xb, 0xc, 0xc, 0xe, 0xe, 0xf, 0xf, 0xa };
		static const u8  shift[9] = { 0, 6, 0, 6, 0, 6, 0, 6, 0 };
		unsigned int stage;

		saved = b43_phy_ac_tbl_write_lock(dev);
		for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
			for (stage = 0; stage < 9; stage++) {
				u16 off = base[core] + add[stage];
				u16 cur, out, field;

				b43_actab_read_bulk(dev, 7, off, 16, 1, &cur);
				field = ((cur >> shift[stage]) & 0x20) | aphy->dacbuf_cap;
				if (shift[stage] == 0)
					out = field | (cur & 0xfc0);
				else
					out = (u16)(field << 6) | (cur & 0x3f);

				b43_actab_write_bulk(dev, 7, off, 16, 1, &out);
			}
		}
		b43_phy_ac_tbl_write_unlock(dev, saved);
	}

	/*
	 * RX-LPF: the table-7 {lo,hi} cells carry a per-row base (lo bits 0-5 =
	 * 0x00/0x09/0x12, hi 0) that the RMW keeps, rewriting the cap fields f6
	 * (bits 6..13) and f17 (bits 17..): f17 = lpf_cap1, f6 = (lpf_cap0 *
	 * rx_k[row]) >> 8 with rx_k = {221, 215, 215}. Verified on the d6220
	 * (0xa8 -> 0x91/0x8d/0x8d) and the agcombo (0xae -> 0x96/0x92/0x92);
	 * the DSL's wl 6.30 does not scale (rx_k = 256).
	 *
	 * The rows are the three operating widths;
	 * b43_phy_ac_rxgain_config_apply() reads back the current width's.
	 *
	 * TODO: the coefficients are fitted on two cap samples (221/222 and
	 * 215/216 both match); a third lpf_cap0 would pin them.
	 */
	{
		const u16 (*lo_off)[3] = b43_phy_ac_rxlpf_lo_cell;
		static const u16 hi_off[3][3] = {
			{ 0x360, 0x370, 0x380 },
			{ 0x361, 0x371, 0x381 },
			{ 0x440, 0x442, 0x444 },
		};
		static const u16 rx_k[3] = { 221, 215, 215 };
		unsigned int stage;

		for (stage = 0; stage < 3; stage++) {
			saved = b43_phy_ac_tbl_write_lock(dev);
			for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
				u16 off_lo, off_hi;
				u16 lo, hi;
				u32 v;


				off_lo = lo_off[stage][core];
				off_hi = hi_off[stage][core];

				b43_actab_read_bulk(dev, 7, off_lo, 16, 1, &lo);
				b43_actab_read_bulk(dev, 7, off_hi, 16, 1, &hi);
				v = ((u32)hi << 16) | lo;
				v = (v & 0x1ffc03f) |
				    ((u32)(((u32)aphy->lpf_cap0 * rx_k[stage]) >> 8) << 6);
				v = (v & 0x1ffff)   | ((u32)aphy->lpf_cap1 << 17);
				lo = (u16)v;
				hi = (u16)(v >> 16) & 0x1ff;

				b43_actab_write_bulk(dev, 7, off_lo, 16, 1, &lo);
				b43_actab_write_bulk(dev, 7, off_hi, 16, 1, &hi);
			}
			b43_phy_ac_tbl_write_unlock(dev, saved);
		}
	}
}

/*
 * RF sequencer command tables (table 7) and the spexp TXV (table 0x10):
 * the table-load half of the reset-time setup, under the outer gate
 * analog_on_reset() took. The band-gated 0x80 write is skipped on these
 * boards (boardflags2 = 0x2) and omitted.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   6265-6903]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   1935-2573]
 */
static void b43_phy_ac_rfseq_tbl_init(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const u16 spexp_pad = 0x0020;
	static const u16 spexp_off[] = { 0x3c6, 0x3c7, 0x3d6, 0x3d7, 0x3e6, 0x3e7 };
	size_t i;

	b43_actab_write_bulk(dev, 7, 0x020, 16, 16, b43_acphy_rfseq_reset2rx_cmd);
	b43_actab_write_bulk(dev, 7, 0x090, 16, 16, b43_acphy_rfseq_reset2rx_dly);
	b43_actab_write_bulk(dev, 7, 0x121, 16,  2, b43_acphy_rfseq_updl_lpf_hpc);
	b43_actab_write_bulk(dev, 7, 0x131, 16,  2, b43_acphy_rfseq_updl_lpf_hpc);
	b43_actab_write_bulk(dev, 7, 0x124, 16,  2, b43_acphy_rfseq_updl_tia_hpc);
	b43_actab_write_bulk(dev, 7, 0x137, 16,  2, b43_acphy_rfseq_updl_tia_hpc);
	b43_actab_write_bulk(dev, 7, 0x000, 16, 16, b43_acphy_rfseq_rx2tx_cmd);
	b43_actab_write_bulk(dev, 7, 0x010, 16, 16, b43_acphy_rfseq_tx2rx_cmd);

	for (i = 0; i < ARRAY_SIZE(spexp_off); i++)
		b43_actab_write_bulk(dev, 7, spexp_off[i], 16, 1, &spexp_pad);

	b43_actab_write_bulk(dev, 0x10, 0x4c4, 32, 243, b43_acphy_txv_for_spexp);
}

/*
 * Reset-time PHY register block (phy rev 1, the only verified rev). The
 * backplane MAC-PHY clock enable is b43_mac_phy_clock_set(); the CCA reset
 * mid-sequence is b43_phy_ac_reset_cca().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5128-5158]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   798-828]
 */
static void b43_phy_ac_set_reg_on_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;

	/* Clear the RF-seq override gate before the reset block. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_RF_SEQ_OVERRIDE_GATE, 0);
	b43_phy_set(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x01c0);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~0x0200, 0x0200);
	if (dev->phy.rev == 1)
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x003c, 0x0010);
	b43_phy_write(dev, 0x01f2, 0x00c8);
	if (dev->phy.rev == 1)
		b43_phy_write(dev, 0x0026, 0x0092);
	b43_phy_write(dev, 0x0025, 0x0030);

	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);

	/*
	 * Clip mask: all the bit-5 clears first, then all the low-byte 0x55
	 * writes, in the stock driver's order.
	 */
	b43_phy_maskset(dev, 0x02f1, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x02ed, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x02f9, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x02f5, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x02ef, (u16)~0x00ff, 0x0055);
	b43_phy_maskset(dev, 0x02eb, (u16)~0x00ff, 0x0055);
	b43_phy_maskset(dev, 0x02f7, (u16)~0x00ff, 0x0055);
	b43_phy_maskset(dev, 0x02f3, (u16)~0x00ff, 0x0055);

	b43_phy_write(dev, 0x0400, 0x0000);
	if (dev->phy.rev == 1)
		b43_phy_maskset(dev, 0x01ca, (u16)~0x1000, 0);

	b43_phy_ac_reset_cca(dev);

	b43_phy_maskset(dev, 0x0072, (u16)~0x0004, 0x0004);
	b43_phy_maskset(dev, 0x01b0, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x01b1, (u16)~0x1000, 0x1000);
	b43_phy_maskset(dev, 0x01b6, (u16)~0x8000, 0);

	for (c = 0; c < num_cores; c++) {
		b43_phy_maskset(dev, 0x0690 + c * 0x0200, (u16)~0x0200, 0x0200);
		b43_phy_maskset(dev, 0x0690 + c * 0x0200, (u16)~0x0400, 0x0400);
	}

	b43_phy_write(dev, 0x01e6, 0x0030);
}

/*
 * AvVmid: gain and mid-point voltage of each chain's power-detector input,
 * one pair per chain and band, written into RFSEQ 0x03cd + 0x10 * core as
 * (Vmid << 3) | Av.
 *
 * The board may give it in NVRAM as AvVmid_c0..2 (Av,Vmid for 2g, 5gl,
 * 5gml, 5gmu, 5gh), which the stock driver uses only when boardflags3 has
 * BFL3_AvVim ("load AvVim from nvram"). Otherwise the pair is the default
 * set indexed by pdgain5g. All four boards here have boardflags3=0 and no
 * AvVmid in NVRAM.
 *
 * The default sets are the stock driver's avvmid_set[] (wlD6220.o, 7.14.89,
 * 720 bytes of .rodata): 24 sets, five bands each, the three chains' Av
 * then their Vmid. Set 10 is what the d6220, the DSL-3580L and the agcombo
 * write (pdgain5g=10), set 19 what the tg789vac writes (pdgain5g=19) on all
 * 43 cold segments. Band 0 is 2.4 GHz, bands 1-4 follow
 * b43_phy_ac_pa5g_group().
 */
#define B43_BFL3_AVVIM	0x40000000

static const u8 b43_phy_ac_avvmid_set[24][5][2][3] = {
	/*  0 */ {
		{ { 2, 1, 2 }, { 107, 150, 110 } },
		{ { 2, 2, 1 }, { 157, 153, 160 } },
		{ { 2, 2, 1 }, { 157, 153, 161 } },
		{ { 2, 2, 0 }, { 157, 153, 186 } },
		{ { 2, 2, 0 }, { 157, 153, 187 } },
	},
	/*  1 */ {
		{ { 1, 0, 1 }, { 159, 174, 161 } },
		{ { 1, 0, 1 }, { 160, 185, 156 } },
		{ { 1, 0, 1 }, { 163, 185, 162 } },
		{ { 1, 0, 1 }, { 169, 187, 167 } },
		{ { 1, 0, 1 }, { 152, 188, 160 } },
	},
	/*  2 */ {
		{ { 1, 1, 1 }, { 159, 166, 166 } },
		{ { 2, 2, 4 }, { 140, 151, 100 } },
		{ { 2, 2, 3 }, { 143, 153, 116 } },
		{ { 2, 2, 2 }, { 143, 153, 140 } },
		{ { 2, 2, 2 }, { 145, 160, 154 } },
	},
	/*  3 */ {
		{ { 1, 1, 2 }, { 130, 131, 106 } },
		{ { 1, 1, 2 }, { 130, 131, 106 } },
		{ { 1, 1, 2 }, { 128, 127,  97 } },
		{ { 0, 1, 3 }, { 159, 137,  75 } },
		{ { 0, 0, 3 }, { 164, 162,  76 } },
	},
	/*  4 */ {
		{ { 1, 1, 1 }, { 156, 160, 158 } },
		{ { 1, 1, 1 }, { 156, 160, 158 } },
		{ { 1, 1, 1 }, { 156, 160, 158 } },
		{ { 1, 1, 1 }, { 156, 160, 158 } },
		{ { 1, 1, 1 }, { 156, 160, 158 } },
	},
	/*  5 */ {
		{ { 2, 2, 2 }, { 104, 108, 106 } },
		{ { 2, 2, 2 }, { 104, 108, 106 } },
		{ { 2, 2, 2 }, { 104, 108, 106 } },
		{ { 2, 2, 2 }, { 104, 108, 106 } },
		{ { 2, 2, 2 }, { 104, 108, 106 } },
	},
	/*  6 */ {
		{ { 2, 0, 2 }, { 102, 170, 104 } },
		{ { 3, 4, 3 }, {  82, 102,  82 } },
		{ { 1, 3, 1 }, { 134, 122, 136 } },
		{ { 1, 3, 1 }, { 134, 124, 136 } },
		{ { 2, 3, 2 }, { 104, 122, 108 } },
	},
	/*  7 */ {
		{ { 0, 0, 0 }, { 180, 180, 180 } },
		{ { 0, 0, 0 }, { 180, 180, 180 } },
		{ { 0, 0, 0 }, { 180, 180, 180 } },
		{ { 0, 0, 0 }, { 180, 180, 180 } },
		{ { 0, 0, 0 }, { 180, 180, 180 } },
	},
	/*  8 */ {
		{ { 2, 1, 2 }, { 102, 138, 104 } },
		{ { 3, 5, 3 }, {  82, 100,  82 } },
		{ { 1, 4, 1 }, { 134, 116, 136 } },
		{ { 1, 3, 1 }, { 134, 136, 136 } },
		{ { 2, 3, 2 }, { 104, 136, 108 } },
	},
	/*  9 */ {
		{ { 3, 2, 3 }, {  90, 106,  86 } },
		{ { 3, 1, 3 }, {  90, 158,  90 } },
		{ { 2, 1, 2 }, { 114, 158, 112 } },
		{ { 2, 1, 1 }, { 116, 158, 142 } },
		{ { 2, 1, 1 }, { 116, 158, 142 } },
	},
	/* 10 */ {
		{ { 2, 2, 2 }, { 152, 156, 156 } },
		{ { 2, 2, 2 }, { 152, 156, 156 } },
		{ { 2, 2, 2 }, { 152, 156, 156 } },
		{ { 2, 2, 2 }, { 152, 156, 156 } },
		{ { 2, 2, 2 }, { 152, 156, 156 } },
	},
	/* 11 */ {
		{ { 1, 1, 1 }, { 134, 134, 134 } },
		{ { 1, 1, 1 }, { 136, 136, 136 } },
		{ { 1, 1, 1 }, { 136, 136, 136 } },
		{ { 1, 1, 1 }, { 136, 136, 136 } },
		{ { 1, 1, 1 }, { 136, 136, 136 } },
	},
	/* 12 */ {
		{ { 3, 3, 3 }, {  90,  92,  86 } },
		{ { 3, 3, 3 }, {  90,  86,  90 } },
		{ { 2, 3, 2 }, { 114,  86, 112 } },
		{ { 2, 2, 1 }, { 116, 109, 142 } },
		{ { 2, 2, 1 }, { 116, 110, 142 } },
	},
	/* 13 */ {
		{ { 2, 2, 2 }, { 112, 114, 112 } },
		{ { 2, 2, 2 }, { 114, 114, 114 } },
		{ { 2, 2, 2 }, { 114, 114, 114 } },
		{ { 2, 2, 2 }, { 113, 114, 112 } },
		{ { 2, 2, 2 }, { 113, 114, 112 } },
	},
	/* 14 */ {
		{ { 1, 1, 1 }, { 134, 134, 134 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
	},
	/* 15 */ {
		{ { 0, 0, 0 }, { 172, 172, 172 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
	},
	/* 16 */ {
		{ { 3, 2, 3 }, {  90, 106,  86 } },
		{ { 3, 0, 3 }, {  90, 186,  90 } },
		{ { 2, 0, 2 }, { 114, 186, 112 } },
		{ { 2, 0, 1 }, { 116, 186, 142 } },
		{ { 2, 0, 1 }, { 116, 186, 142 } },
	},
	/* 17 */ {
		{ { 4, 4, 4 }, {  50,  45,  50 } },
		{ { 3, 3, 3 }, {  82,  82,  82 } },
		{ { 3, 3, 3 }, {  82,  82,  82 } },
		{ { 3, 3, 3 }, {  82,  82,  82 } },
		{ { 3, 3, 3 }, {  82,  82,  82 } },
	},
	/* 18 */ {
		{ { 5, 5, 5 }, {  61,  61,  61 } },
		{ { 2, 2, 2 }, { 122, 122, 122 } },
		{ { 2, 2, 2 }, { 122, 122, 122 } },
		{ { 2, 2, 2 }, { 122, 122, 122 } },
		{ { 2, 2, 2 }, { 122, 122, 122 } },
	},
	/* 19 */ {
		{ { 2, 2, 2 }, { 152, 156, 156 } },
		{ { 1, 1, 1 }, { 165, 165, 165 } },
		{ { 1, 1, 1 }, { 160, 160, 160 } },
		{ { 1, 1, 1 }, { 152, 150, 160 } },
		{ { 1, 1, 1 }, { 152, 150, 160 } },
	},
	/* 20 */ {
		{ { 3, 3, 3 }, { 108, 108, 108 } },
		{ { 1, 1, 1 }, { 160, 160, 160 } },
		{ { 1, 1, 1 }, { 160, 160, 160 } },
		{ { 1, 1, 1 }, { 160, 160, 160 } },
		{ { 1, 1, 1 }, { 160, 160, 160 } },
	},
	/* 21 */ {
		{ { 2, 2, 2 }, { 110, 110, 110 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
		{ { 0, 0, 0 }, { 168, 168, 168 } },
	},
	/* 22 */ {
		{ { 6, 6, 6 }, {  40,  40,  40 } },
		{ { 2, 2, 1 }, { 115, 115, 142 } },
		{ { 1, 2, 1 }, { 142, 115, 142 } },
		{ { 1, 1, 1 }, { 142, 142, 142 } },
		{ { 1, 1, 1 }, { 142, 142, 142 } },
	},
	/* 23 */ {
		{ { 1, 1, 1 }, { 156, 160, 158 } },
		{ { 6, 6, 6 }, {  47,  45,  48 } },
		{ { 1, 1, 1 }, { 147, 146, 148 } },
		{ { 1, 1, 1 }, { 146, 146, 152 } },
		{ { 1, 1, 1 }, { 146, 146, 152 } },
	},
};

static u16 b43_phy_ac_avvmid_cell(struct b43_wldev *dev, unsigned int core,
				  unsigned int grp)
{
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	unsigned int band = 1 + grp;
	u8 av, vmid;

	if ((sprom->boardflags3 & B43_BFL3_AVVIM) &&
	    (sprom->avvmid_valid & BIT(core))) {
		av = sprom->avvmid[core][band][0];
		vmid = sprom->avvmid[core][band][1];
	} else if (sprom->pdgain5g < ARRAY_SIZE(b43_phy_ac_avvmid_set)) {
		av = b43_phy_ac_avvmid_set[sprom->pdgain5g][band][0][core];
		vmid = b43_phy_ac_avvmid_set[sprom->pdgain5g][band][1][core];
	} else {
		b43warn(dev->wl,
			"AC-PHY: pdgain5g=%u has no AvVmid default set and the board gives none; chain %u left at reset\n",
			sprom->pdgain5g, core);
		return 0x0c02;
	}
	return (u16)((vmid << 3) | (av & 0x7));
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5128-7533]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   798-3203]
 */
static void b43_phy_ac_channel_setup(struct b43_wldev *dev,
				     const struct b43_phy_ac_channeltab_e_radio2069 *e,
				     struct ieee80211_channel *new_channel)
{
	B43_AC_FN();
	unsigned int i;
	u16 gate, inner;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_AFE_OFF);

	if (!e) {
		b43err(dev->wl, "AC-PHY: no channel table entry, skipping setup\n");
		return;
	}

	/* Reset-time register block (includes the CCA reset). */
	b43_phy_ac_set_reg_on_reset(dev);

	/*
	 * RXIQ coefficient seed (per-core, 2 bytes each), emitted before the
	 * second AFE/LPF stage call.
	 */
	b43_phy_maskset(dev, 0x02d1, (u16)~0x00f0, 0x0040);
	b43_phy_maskset(dev, 0x02d1, (u16)~0x0f00, 0x0400);
	b43_phy_maskset(dev, 0x02d2, (u16)~0x00f0, 0x0040);
	b43_phy_maskset(dev, 0x02d2, (u16)~0x0f00, 0x0400);

	/* Per-chain AFE/LPF stage, second call. */
	b43_radio_2069_afe_lpf_stage(dev, 0x0000);

	/*
	 * analog_on_reset() opens the outer gate and it covers rfseq_tbl_init()
	 * too: the stock driver emits no unlock or relock between the two
	 * blocks, the last RX-LPF inner unlock being immediately followed by
	 * the first table write of the second.
	 */
	{
		u16 saved_outer;

		b43_phy_ac_analog_on_reset(dev, &saved_outer);
		b43_phy_ac_rfseq_tbl_init(dev);

		b43_phy_ac_tbl_write_unlock(dev, saved_outer);
	}

	gate = b43_phy_ac_post_rfseq_misc_setup(dev);

	/* Table 0x20: the band's TX gain table, 128 cells of 48 bits. */
	b43_actab_write_bulk(dev, 0x20, 0x0000, 48, 128,
			     b43_phy_ac_txgain_table(dev)[0]);

	/*
	 * Per-chain PHY setup after table 0x20, the same on the d6220 and the
	 * agcombo, filtered by the coremask.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;

		for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
			u16 stride = (u16)(core * 0x200);

			b43_phy_maskset(dev, 0x0072, (u16)~0x0004, 0x0004);
			b43_phy_maskset(dev, 0x0727 + stride,
					(u16)~0x0004, 0x0004);
			b43_phy_maskset(dev, 0x073c + stride,
					(u16)~0x0010, 0x0000);
		}
	}

	b43_phy_ac_radio_percore_setup_1(dev);

	/*
	 * Not filtered by the coremask: the stock driver emits this for the
	 * d6220's unwired core 2 as well.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;

		for (core = 0; core < num_cores; core++) {
			u16 stride = (u16)(core * 0x200);

			b43_phy_maskset(dev, 0x0728 + stride,
					(u16)~0x3800, 0x0800);
			b43_phy_maskset(dev, 0x0721 + stride,
					(u16)~0x4000, 0x4000);
		}
	}

	/*
	 * Not filtered by the coremask either. Bit 12 of 0x?21 adds to the bit
	 * 14 the previous loop set.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;

		for (core = 0; core < num_cores; core++) {
			u16 phy_stride = (u16)(core * 0x200);
			u16 rad_stride = (u16)(core * 0x200);

			b43_phy_maskset(dev, 0x0729 + phy_stride,
					(u16)~0x1000, 0x1000);
			b43_phy_maskset(dev, 0x0721 + phy_stride,
					(u16)~0x1000, 0x1000);
			b43_radio_maskset(dev, 0x0033 + rad_stride,
					  (u16)~0xf000, 0x4000);
		}
	}

	/*
	 * Per-chain clear, filtered by the coremask: both captures skip core 2.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;

		for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
			u16 tbl_off = (u16)(0x0060 + core * 4);
			u16 rad_stride = (u16)(core * 0x200);
			u16 phy_stride = (u16)(core * 0x200);


			b43_actab_zerofill(dev, 0x0c, tbl_off, 16, 2);
			b43_actab_zerofill(dev, 0x0c, (u16)(tbl_off + 2), 16, 1);
			b43_radio_write(dev, 0x0002 + rad_stride, 0);
			b43_radio_write(dev, 0x0003 + rad_stride, 0);
			b43_radio_write(dev, 0x0004 + rad_stride, 0);
			b43_radio_write(dev, 0x0005 + rad_stride, 0);
			b43_phy_write(dev, 0x06a0 + phy_stride, 0);
			b43_phy_write(dev, 0x06a1 + phy_stride, 0);
		}
	}

	b43_phy_ac_tbl_write_unlock(dev, gate);
	gate = b43_phy_ac_tbl_write_lock(dev);
	b43_phy_ac_coeff_bank_init(dev);

	/*
	 * Second pass of set_analog_tx_lpf, on stage 8 only, under a lock
	 * nested in coeff_bank_init()'s, hence the _locked variant.
	 *
	 * This is where the bandwidth shows: the first pass lays down the 20
	 * MHz base on every width, and this one replaces stage 8 with the base
	 * the width selects, 0x0db at 20 MHz, 0x123 at 40, 0x16b at 80. Stages
	 * 0 to 7 are the same on every width.
	 *
	 * These are the driver's defaults: no board defines ofdmanalogfiltbw5g,
	 * and the 4360 and the 4352 write the same bases, differing only in the
	 * rccal cap in the high byte.
	 */
	inner = b43_phy_ac_tbl_write_lock(dev);
	{
		static const u16 stage8_base[3] = { 0x00db, 0x0123, 0x016b };
		u16 base = stage8_base[b43_phy_ac_bw_step(dev)];

		b43_phy_ac_set_analog_tx_lpf_locked(dev, 0x100,
						    base & 7,
						    (base >> 6) & 7,
						    (base >> 3) & 7,
						    dev->phy.ac->lpf_cap0,
						    dev->phy.ac->lpf_cap0,
						    0xffffffff);
	}
	b43_phy_ac_tbl_write_unlock(dev, inner);

	b43_phy_ac_rx_evm_shaping_override(dev);

	/*
	 * Per-core masksets before the second rfseq setup, filtered by the
	 * coremask.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;

		for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
			u16 stride = (u16)(core * 0x200);


			b43_phy_maskset(dev, 0x073a + stride,
					(u16)~0x0080, 0x0080);
			b43_phy_maskset(dev, 0x0725 + stride,
					(u16)~0x0200, 0x0200);
		}
	}

	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;
		static const u16 *cmd_narrow[3] = {
			b43_acphy_rfseq_2_cmd_c0,
			b43_acphy_rfseq_2_cmd_c1,
			b43_acphy_rfseq_2_cmd_c2,
		};
		static const u16 *dly_narrow[3] = {
			b43_acphy_rfseq_2_dly_c0,
			b43_acphy_rfseq_2_dly_c1,
			b43_acphy_rfseq_2_dly_c2,
		};
		static const u16 *cmd_bw80[3] = {
			b43_acphy_rfseq_2_cmd_c0_bw80,
			b43_acphy_rfseq_2_cmd_c1_bw80,
			b43_acphy_rfseq_2_cmd_c2_bw80,
		};
		static const u16 *dly_bw80[3] = {
			b43_acphy_rfseq_2_dly_c0_bw80,
			b43_acphy_rfseq_2_dly_c1_bw80,
			b43_acphy_rfseq_2_dly_c2_bw80,
		};
		bool wide = dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80;
		const u16 **cmd_tbl = wide ? cmd_bw80 : cmd_narrow;
		const u16 **dly_tbl = wide ? dly_bw80 : dly_narrow;

		/*
		 * At 80 MHz the sequence opens with three 48-bit cells of
		 * table 0x14, one write each; absent at 20 and 40 MHz.
		 */
		if (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80) {
			static const u16 t14_bw80[3][3] = {
				{ 0x0fd2, 0x0096, 0x0000 },
				{ 0x0fc2, 0x0086, 0x0000 },
				{ 0x0fd2, 0x0086, 0x0000 },
			};
			unsigned int k;

			for (k = 0; k < ARRAY_SIZE(t14_bw80); k++)
				b43_actab_write_bulk(dev, 0x0014, 0x0030 + k,
						     48, 1, t14_bw80[k]);
		}

		for (core = 0; core < num_cores && core < 3; core++) {
			u16 cmd_off = (u16)(0x0030 + core * 0x10);
			u16 dly_off = (u16)(0x00a0 + core * 0x10);

			b43_actab_write_bulk(dev, 7, cmd_off, 16, 8,
					     cmd_tbl[core]);
			b43_actab_write_bulk(dev, 7, dly_off, 16, 8,
					     dly_tbl[core]);
		}
	}

	/*
	 * 0x0197 and 0x0198, then unlock coeff_bank_init()'s gate so that the
	 * raw PHY writes that follow get through. Both step once from 20 to 40 MHz and
	 * hold: 0x14 and 0x10 at 20 MHz, 0x1e and 0x14 from 40 on, on every
	 * channel.
	 */
	if (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_20) {
		b43_phy_write(dev, 0x0197, 0x0014);
		b43_phy_write(dev, 0x0198, 0x0010);
	} else {
		b43_phy_write(dev, 0x0197, 0x001e);
		b43_phy_write(dev, 0x0198, 0x0014);
	}
	b43_phy_ac_tbl_write_unlock(dev, gate);

	/*
	 * Clear bits of 0x0410 and of the per-core 0x0?3a/0x0?25, not filtered
	 * by the coremask.
	 */
	b43_phy_maskset(dev, 0x0410, (u16)~0x0008, 0x0000);
	b43_phy_maskset(dev, 0x0410, (u16)~0x0380, 0x0000);
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;

		for (core = 0; core < num_cores; core++) {
			u16 stride = (u16)(core * 0x200);

			b43_phy_maskset(dev, 0x073a + stride,
					(u16)~0x0008, 0x0000);
			b43_phy_maskset(dev, 0x0725 + stride,
					(u16)~0x0040, 0x0000);
			b43_phy_maskset(dev, 0x073a + stride,
					(u16)~0x0010, 0x0000);
			b43_phy_maskset(dev, 0x0725 + stride,
					(u16)~0x0080, 0x0000);
			b43_phy_maskset(dev, 0x073a + stride,
					(u16)~0x0007, 0x0000);
			b43_phy_maskset(dev, 0x0725 + stride,
					(u16)~0x0020, 0x0000);
		}
	}

	b43_phy_ac_farrow_setup(dev, new_channel);

	/* The gate for the RFSEQ 0x03cd/0x03dd accesses that follow. */
	gate = b43_phy_ac_tbl_write_lock(dev);

	/*
	 * The AvVmid of each wired chain, into RFSEQ 0x03cd + 0x10 * core: read,
	 * then write. The read returns 0x0c02 everywhere. See
	 * b43_phy_ac_avvmid_cell().
	 */
	{
		struct b43_phy_ac *ac = dev->phy.ac;
		unsigned int grp = b43_phy_ac_pa5g_group(dev, new_channel->center_freq);
		unsigned int c;

		for_each_set_bit(c, &ac->coremask, ac->num_cores) {
			u16 off = (u16)(0x03cd + 0x10 * c);
			u16 val = b43_phy_ac_avvmid_cell(dev, c, grp);

			b43_actab_read_log(dev, 7, off, 1);
			b43_actab_write_bulk(dev, 7, off, 16, 1, &val);
		}
	}

	b43_phy_ac_tbl_write_unlock(dev, gate);

	/*
	 * Per-channel PHY 0x371-0x376, the 2069 path of
	 * set_regtbl_on_chan_change: phy_reg_write(0x371 + i, ci[0x68 + 2*i]),
	 * entries u16[52..57] of the channel table. Checked against the stock
	 * sweep on all 16 20 MHz channels by reverse-tools/check_channeltab.py.
	 */
	for (i = 0; i < 6; i++)
		b43_phy_write(dev, B43_PHY_AC_BW1A + i, e->phy_bw[i]);

	b43_phy_ac_chanspec_tail(dev);
}

/*
 * Table id 0x11, 464 cells of 48 bits, written one cell at a time.
 *
 * The twelve cells at the head and the four at the tail are constants. The
 * 448 between them repeat one value per sub-band, built
 * from the SROM's rpcal word for that sub-band (rpcal2g on 2.4 GHz,
 * rpcal5gb0..3 on the four pa5g sub-bands): each of its two bytes gives a
 * 24-bit coefficient, the low byte in bits 23:0 of the cell and the high byte
 * in bits 47:24.
 */
static const u16 b43_acphy_tbl11_head[12][3] = {
	{ 0x005b, 0x0000, 0x0000 }, { 0x8250, 0x0000, 0x0000 },
	{ 0xc338, 0x0000, 0x0000 }, { 0x4527, 0x0001, 0x0000 },
	{ 0xa6a1, 0x0001, 0x0000 }, { 0x081b, 0x0002, 0x0000 },
	{ 0x8a18, 0x0002, 0x0000 }, { 0x2c96, 0x0003, 0x0000 },
	{ 0x8e17, 0x0003, 0x0000 }, { 0x101b, 0x0004, 0x0000 },
	{ 0x0020, 0x0000, 0x0000 }, { 0x0020, 0x0000, 0x0000 },
};

#define B43_PHY_AC_TBL11_FILL_OFF	12
#define B43_PHY_AC_TBL11_FILL_LEN	448
#define B43_PHY_AC_TBL11_TAIL_LEN	4

/* round(512 * sin(k * 2 pi / 256)) for k = 0..63. */
static const u16 b43_phy_ac_sin512_q[64] = {
	  0,  13,  25,  38,  50,  63,  75,  88, 100, 112, 124, 137, 149,
	161, 172, 184, 196, 207, 219, 230, 241, 252, 263, 274, 284, 295,
	305, 315, 325, 334, 344, 353, 362, 371, 379, 388, 396, 404, 411,
	419, 426, 433, 439, 445, 452, 457, 463, 468, 473, 478, 482, 486,
	490, 493, 497, 500, 502, 504, 506, 508, 510, 511, 511, 512,
};

static u16 b43_phy_ac_rpcal(struct b43_wldev *dev)
{
	const struct ssb_sprom *sp = dev->dev->bus_sprom;

	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ)
		return sp->rpcal2g;
	return sp->rpcal5gb[b43_phy_ac_pa5g_group(dev, dev->phy.ac->cal_freq)];
}

/*
 * One rpcal byte as a 24-bit coefficient: bits 5:0 index the
 * quarter-wave table, bits 7:6 pick the quadrant, I goes in bits 10:0 and Q
 * in bits 21:11, both as 11-bit two's complement, and bit 22 is set.
 */
static u32 b43_phy_ac_rpcal_coeff(u8 v)
{
	unsigned int k = v & 0x3f;
	s16 a = b43_phy_ac_sin512_q[63 - k];
	s16 b = b43_phy_ac_sin512_q[k];
	s16 i, q;

	switch (v >> 6) {
	case 0:
		i = a;
		q = -b;
		break;
	case 1:
		i = -b;
		q = -a;
		break;
	case 2:
		i = -a;
		q = b;
		break;
	default:
		i = b;
		q = a;
		break;
	}
	return BIT(22) | (u32)(q & 0x7ff) << 11 | (u32)(i & 0x7ff);
}

static void b43_phy_ac_tbl11_fill(struct b43_wldev *dev, u16 cell[3])
{
	u16 rpcal = b43_phy_ac_rpcal(dev);
	u64 v = b43_phy_ac_rpcal_coeff(rpcal & 0xff) |
		(u64)b43_phy_ac_rpcal_coeff(rpcal >> 8) << 24;

	cell[0] = (u16)v;
	cell[1] = (u16)(v >> 16);
	cell[2] = (u16)(v >> 32);
}

/*
 * Per-channel table loads, radio rev 4 on 5 GHz: the twin coefficients at
 * 0x00ec-0x00f5, table 0x11 at 464 words, tables 0x0b and 0x15, and the
 * per-core 0x44/0x45 pair broadcast to num_cores.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7534-10317]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   3204-5987]
 */
static void b43_phy_ac_chan_tables(struct b43_wldev *dev)
{
	B43_AC_FN();
	u16 saved, fill[3];
	unsigned int k;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Table 0x11, one 48-bit cell per table write, so id and offset are
	 * reselected for every cell. The load takes the 0x019e gate and gives
	 * it back; the noise-shaping tables (0x0b, 0x15, the per-core
	 * 0x44/0x45) are emitted by op_switch_channel().
	 *
	 * Loaded only where the SROM carries rpcal: on the d6220 and the
	 * agcombo, not on the tg789vac or the DSL-3580L, whose five rpcal words
	 * are zero. There the gate cycle around it is absent too. The 6.30
	 * hybrid loads it regardless.
	 */
	{
		const struct ssb_sprom *sp = dev->dev->bus_sprom;

		if (!sp->rpcal2g && !sp->rpcal5gb[0] && !sp->rpcal5gb[1] &&
		    !sp->rpcal5gb[2] && !sp->rpcal5gb[3])
			return;
	}

	b43_phy_ac_tbl11_fill(dev, fill);

	saved = b43_phy_ac_tbl_write_lock(dev);
	for (k = 0; k < ARRAY_SIZE(b43_acphy_tbl11_head); k++)
		b43_actab_write_bulk(dev, 0x11, k, 48, 1, b43_acphy_tbl11_head[k]);
	for (k = 0; k < B43_PHY_AC_TBL11_FILL_LEN; k++)
		b43_actab_write_bulk(dev, 0x11, B43_PHY_AC_TBL11_FILL_OFF + k,
				     48, 1, fill);
	for (k = 0; k < B43_PHY_AC_TBL11_TAIL_LEN; k++)
		b43_actab_zerofill(dev, 0x11, B43_PHY_AC_TBL11_FILL_OFF +
				   B43_PHY_AC_TBL11_FILL_LEN + k, 48, 1);
	b43_phy_ac_tbl_write_unlock(dev, saved);
}

/*
 * rx_evm_shaping override in table 0x04: two runs of three words, at 0x0001
 * and 0x003d, under the gate channel_setup() holds.
 *
 * {8, 6, 4} and {4, 6, 8} at 20 MHz; zeroes, i.e. disabled, on a bonded
 * channel, on every channel of each width.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7323-7338]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2993-3008]
 */
static void b43_phy_ac_rx_evm_shaping_override(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const u16 narrow_lo[3] = { 0x0008, 0x0006, 0x0004 };
	static const u16 narrow_hi[3] = { 0x0004, 0x0006, 0x0008 };
	static const u16 wide[3]      = { 0x0000, 0x0000, 0x0000 };
	bool narrow = dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_20;
	const u16 *blk_lo = narrow ? narrow_lo : wide;
	const u16 *blk_hi = narrow ? narrow_hi : wide;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET);

	b43_actab_write_bulk(dev, 0x04, 0x0001, 16, 3, blk_lo);
	b43_actab_write_bulk(dev, 0x04, 0x003d, 16, 3, blk_hi);
}

/*
 * Per-core body of the post-noise-shaping block, at a 0x200 stride: peek
 * and write 0x06dc/0x06dd, write table 0x07 at 0xf9 + core = 0xc0b5, unlock
 * the outer gate, five groups of (clear bit 1 of 0x06e3, two writes), eight
 * peeks, and a MOD of 0x06ee.
 *
 * 0x06dc-0x06e5 and 0x06ee have no known name; the per-core shape suggests
 * RX gain or RSSI programming.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11060-11101, 11113-11154, 11166-11207]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6730-6771, 6783-6824, 6836-6877]
 */
static void
b43_phy_ac_post_noise_shaping_rx_regprog_core(struct b43_wldev *dev,
					      unsigned int core)
{
	u16 cur;
	B43_AC_FN();
	u16 stride = (u16)(core * 0x200);
	u16 tbl_off = (u16)(0x00f9 + core);
	static const u16 tbl_val = 0xc0b5;
	/*
	 * 0x06de and 0x06e0 are 0x014a on 2.4 GHz on both cores of the
	 * MacBookAir6,1 and the archer-t5e, 0x015a and 0x016a on 5 GHz. The
	 * rest of the block differs between those boards on 2.4 GHz, and 0x06e1
	 * on 5 GHz too: board data, transcribed from the d6220.
	 */
	bool band_2g = b43_current_band(dev->wl) == NL80211_BAND_2GHZ;

	/* 0x06dc written back as read, 0x06dd programmed */
	cur = b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_write(dev, 0x06dc + stride, cur);
	b43_phy_write(dev, 0x06dd + stride, 0x0604);

	/* The gate is unlocked here, hence _reopen. */
	b43_actab_write_bulk_reopen(dev, 0x07, tbl_off, 16, 1, &tbl_val);

	/* Unlock the outer gate after the table write. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);

	/* Five groups: clear bit 1 of 0x06e3, then two writes. */
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06de + stride, band_2g ? 0x014a : 0x015a);
	b43_phy_write(dev, 0x06df + stride, 0x0004);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06e0 + stride, band_2g ? 0x014a : 0x016a);
	b43_phy_write(dev, 0x06e1 + stride, 0x0018);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06e4 + stride, 0x013a);
	b43_phy_write(dev, 0x06e5 + stride, 0x0008);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06e2 + stride, 0x013a);
	b43_phy_write(dev, 0x06e3 + stride, 0x0008);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);

	/* Five peeks of 0x06dc, three of 0x06dd. */
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_read_log(dev, 0x06dd + stride);
	b43_phy_read_log(dev, 0x06dd + stride);
	b43_phy_read_log(dev, 0x06dd + stride);

	/* 1 at 20 MHz, 2 from 40 on. */
	b43_phy_maskset(dev, 0x06ee + stride, (u16)~0x0003,
			dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_20
				? 0x0001 : 0x0002);
}

/*
 * Core transition, after every core's body including the last: radio
 * 0x0045 and 0x0033, PHY 0x06dc and 0x06ee. radio_maskset() expands to
 * MOD + RD + WR with the mirror value radio_2069_channel_setup() left: an
 * active core gives 0x73bf/0x4181, an inactive one 0x7380/0x4180.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11102-11112, 11155-11165, 11208-11218]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6772-6782, 6825-6835, 6878-6888]
 */
static void
b43_phy_ac_post_noise_shaping_core_transition(struct b43_wldev *dev,
					      unsigned int core)
{
	B43_AC_FN();
	u16 stride = (u16)(core * 0x200);

	/*
	 * At 20 MHz radio 0x0045 gets bits 9:8 set, on a bonded channel bit 6
	 * cleared instead (a different mask, not just a different value); radio
	 * 0x0033's nibble goes from 0x80 to 0xc0. The 0x06ee field is always
	 * 0x8.
	 */
	if (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_20)
		b43_radio_maskset(dev, 0x0045 + stride, (u16)~0x0300, 0x0300);
	else
		b43_radio_maskset(dev, 0x0045 + stride, (u16)~0x0040, 0x0000);
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_maskset(dev, 0x06ee + stride, (u16)~0x000c, 0x0008);
	b43_radio_maskset(dev, 0x0033 + stride, (u16)~0x00f0,
			  dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_20
				? 0x0080 : 0x00c0);
}

/*
 * Post-noise-shaping block: for every silicon core (num_cores, not the
 * coremask) the body and the transition. The registers are not identified; RX gain or RSSI
 * programming is the likely purpose.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11058-11218]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6728-6888]
 */
static void b43_phy_ac_post_noise_shaping_rx_regprog(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int core;
	u8 num_cores = dev->phy.ac->num_cores;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET);

	for (core = 0; core < num_cores; core++) {
		b43_phy_ac_post_noise_shaping_rx_regprog_core(dev, core);
		b43_phy_ac_post_noise_shaping_core_transition(dev, core);
	}
}

/*
 * RX gain-control registers a core saves before a measurement and restores
 * after it, in the stock driver's order: rx_gain_regs_program() saves them
 * into rxgain_saved[], b43_phy_ac_tempsense() restores them.
 */
static const u16 b43_phy_ac_rxgain_regs[14] = {
	0x073e, 0x0727, 0x073c, 0x0721, 0x0729, 0x0720, 0x0728,
	0x0724, 0x0736, 0x0725, 0x0739, 0x073a, 0x0722, 0x0734,
};

/*
 * The four fields of the RX gain block that depend on the width, and only
 * on it: each value is the same on every channel of its width in the sweep
 * (36 to 140, DFS included). The other 22 fields do not move.
 *
 * Three measured values, not a law: 0x0739[6:1] holds between 20 and 40 and
 * changes at 80, 0x073a[6:5] does the opposite. There is no 160 MHz row.
 */
struct b43_phy_ac_rxgain_bw {
	u16 f73a_07, f739_7e, f73a_08, f73a_60;
};

static const struct b43_phy_ac_rxgain_bw b43_phy_ac_rxgain_bw_tab[3] = {
	{ 0x0003, 0x007a, 0x0000, 0x0040 },	/* 20 MHz */
	{ 0x0002, 0x007a, 0x0000, 0x0000 },	/* 40 MHz */
	{ 0x0000, 0x007e, 0x0008, 0x0000 },	/* 80 MHz */
};

static const struct b43_phy_ac_rxgain_bw *
b43_phy_ac_rxgain_bw(struct b43_wldev *dev)
{
	return &b43_phy_ac_rxgain_bw_tab[b43_phy_ac_bw_step(dev)];
}

/*
 * Program one chain's RX gain-control block, 0x0720-0x073e: open the
 * override bracket on 0x?73e, re-read the 13 saved registers in the stock
 * order, then the 26 programming ops, all masksets as in the capture.
 *
 * Emitted for every wired chain at the head of b43_phy_ac_tempsense().
 * 0x?736 = 0x0154 is a constant of this site (the other two sites have
 * 0x0152 in the b2j bank and 0x022a in rxgain_perchan_config()).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   14412-14466, 14467-14521, 32825-32879, 32880-32934, 35471-35525,
 *   35526-35580]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9826-9880, 9881-9935, 27221-27275, 27276-27330]
 */
static void b43_phy_ac_rx_gain_regs_program(struct b43_wldev *dev,
					    unsigned int core)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);

	const struct b43_phy_ac_rxgain_bw *g = b43_phy_ac_rxgain_bw(dev);

	u16 *saved = dev->phy.ac->rxgain_saved[core];
	unsigned int i;

	saved[0] = b43_phy_read_log(dev, b43_phy_ac_rxgain_regs[0] + s);
	b43_phy_write(dev,    0x073e + s, 0x0440);
	for (i = 1; i < ARRAY_SIZE(b43_phy_ac_rxgain_regs); i++)
		saved[i] = b43_phy_read_log(dev, b43_phy_ac_rxgain_regs[i] + s);

	b43_phy_maskset(dev, 0x0727 + s, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, 0x073c + s, (u16)~0x000e, 0x0002);
	b43_phy_maskset(dev, 0x0727 + s, (u16)~0x0001, 0x0001);
	b43_phy_maskset(dev, 0x073c + s, (u16)~0x0001, 0x0001);
	b43_phy_maskset(dev, 0x0721 + s, (u16)~0x0100, 0x0100);
	b43_phy_maskset(dev, 0x0729 + s, (u16)~0x0100, 0x0000);
	b43_phy_maskset(dev, 0x0720 + s, (u16)~0x0020, 0x0020);
	b43_phy_maskset(dev, 0x0728 + s, (u16)~0x0020, 0x0020);
	b43_phy_maskset(dev, 0x0720 + s, (u16)~0x0040, 0x0040);
	b43_phy_maskset(dev, 0x0728 + s, (u16)~0x0040, 0x0000);
	b43_phy_maskset(dev, 0x0720 + s, (u16)~0x0010, 0x0010);
	b43_phy_maskset(dev, 0x0728 + s, (u16)~0x0010, 0x0010);
	b43_phy_write(dev,   0x0736 + s, 0x0154);
	/*
	 * TSSI floor from the SROM. Unprogrammed (0x3ff) on every board here,
	 * which looks like a literal; a board that programs it would be ignored
	 * by one.
	 */
	b43_phy_write(dev,   0x0724 + s,
		      dev->dev->bus_sprom->tssifloor5g[dev->phy.ac->pa5g_grp]
		      & 0x03ff);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0007, g->f73a_07);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0020, 0x0020);
	b43_phy_maskset(dev, 0x0739 + s, (u16)~0x007e, g->f739_7e);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0008, g->f73a_08);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0040, 0x0040);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0010, 0x0010);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0080, 0x0080);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0060, g->f73a_60);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0100, 0x0100);
	b43_phy_maskset(dev, 0x0734 + s, (u16)~0x0007, 0x0000);
	b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0004, 0x0004);
}


/* Per-core ADC registers, used by both phases of the setup. */
static const u16 b43_phy_ac_adc_hi[8] = { 0x33a, 0x33b, 0x33e, 0x33f,
					  0x342, 0x343, 0x346, 0x347 }; /* = 0x03ac */
static const u16 b43_phy_ac_adc_lo[8] = { 0x33c, 0x33d, 0x340, 0x341,
					  0x344, 0x345, 0x348, 0x349 }; /* = 0x032c */

/*
 * One cell of the gain curve, table 0x0020.
 *
 * The cell is 48 bits, read as three words through the data port 0x0011:
 * the first is the bbmult, the other two the 32 bits the stock driver
 * unpacks into the three coefficient cells of table 0x0007, as fields of
 * 8/16/8 bits from bit 0.
 *
 * Checked on two different cells: 0x0040 reads (0x0035, 0x1300, 0xf32f) and
 * the stock driver writes (0x0000, 0x2f13, 0x00f3) with bbmult 0x35; 0x0000
 * reads (0x0044, 0x7f00, 0xf3ff) and writes (0x0000, 0xff7f, 0x00f3) with
 * bbmult 0x44. Constant on all 43 segments.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11284-11301, 28531-28554]
 */
struct b43_phy_ac_gaincurve {
	u8 bbmult;
	u16 coeff[3];
};

static void b43_phy_ac_read_gaincurve(struct b43_wldev *dev, u16 offset,
				      struct b43_phy_ac_gaincurve *gc)
{
	u16 cell[3];
	u32 packed;

	b43_actab_read_bulk(dev, 0x0020, offset, 48, 1, cell);
	packed = ((u32)cell[2] << 16) | cell[1];

	gc->bbmult = cell[0] & 0xff;
	gc->coeff[0] = (u16)(packed & 0xff);
	gc->coeff[1] = (u16)((packed >> 8) & 0xffff);
	gc->coeff[2] = (u16)(packed >> 24);
}

/*
 * Whether chain @c is calibrated with chain 0's settings: chain 0 itself,
 * and on the tg789vac chain 2 from ch149 up. There the stock driver reads
 * chain 0's GAINCTRLBBMULT entry for chain 2's TX cal and loads chain 0's
 * ladder in table 0x000c, at every width, on all nine UNII-3 segments and
 * none of the others (the d6220's chain 2 is unwired, the agcombo has no
 * UNII-3 capture). The rxgains and maxp5ga the port reads are equal on the
 * three chains, so the rule is transcribed: why UNII-3 differs is unknown.
 */
static bool b43_phy_ac_cal_like_chain0(struct b43_wldev *dev, unsigned int c)
{
	return c == 0 || (c == 2 && dev->phy.ac->pa5g_grp == 3);
}

/*
 * The GAINCTRLBBMULT entry of a chain's TX cal: 0x14 for chain 0's
 * settings, 0x1e otherwise.
 */
static u16 b43_phy_ac_gaincurve_off(struct b43_wldev *dev, unsigned int c)
{
	return b43_phy_ac_cal_like_chain0(dev, c) ? 0x0014 : 0x001e;
}

/*
 * Read each wired chain's entry: the bbmult and the three TX gain code cells
 * the cal writes back, unlocking after each read as the stock driver does.
 */
static void b43_phy_ac_read_chain_gaincurves(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	struct b43_phy_ac_gaincurve gc;
	unsigned int c, i;

	for_each_set_bit(c, &ac->coremask, ac->num_cores) {
		b43_phy_ac_read_gaincurve(dev, b43_phy_ac_gaincurve_off(dev, c), &gc);
		ac->bbmult_cal[c] = gc.bbmult;
		for (i = 0; i < ARRAY_SIZE(gc.coeff); i++)
			ac->gaincurve_coeff[c][i] = gc.coeff[i];
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11299-11388]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6969-7058]
 */
static void b43_phy_ac_adc_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * The RX-chain arm/restore on 0x0739/0x0725/0x073a is not part of this:
	 * it is b43_radio_2069_afecal()'s save-arm-restore, which runs right
	 * before, inside its RCCAL_EN1 bracket. adc_reset proper begins at the
	 * gain table.
	 *
	 * Per active core: read the gain curve at 0x40, then the ADC gain words
	 * into tables 7 and 0xc. These are the words of
	 * b43_phy_ac_txpwr_by_index(0x40), open-coded because the trace has
	 * them only here, wrapped in the gain-curve read-back.
	 */
	for_each_set_bit(c, &dev->phy.ac->coremask, num_cores) {
		struct b43_phy_ac_gaincurve gc;
		u16 bbmult, saved, inner;

		saved = b43_phy_ac_tbl_write_lock(dev);

		/*
		 * Gain curve read-back at 0x40 (agcombo cold01 #17178, #17223,
		 * #17268).
		 */
		b43_phy_ac_read_gaincurve(dev, 0x40, &gc);
		bbmult = gc.bbmult;

		b43_actab_write_bulk(dev, 7, 0x100 + c, 16, 1, &gc.coeff[0]);
		b43_actab_write_bulk(dev, 7, 0x103 + c, 16, 1, &gc.coeff[1]);
		b43_actab_write_bulk(dev, 7, 0x106 + c, 16, 1, &gc.coeff[2]);

		/* The table 0x0c group, under its own nested lock. */
		inner = b43_phy_ac_tbl_write_lock(dev);
		b43_actab_write_bulk(dev, 0xc, 0x63 + c * 4, 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0xc, 0x73 + c * 4, 16, 1, &bbmult);
		b43_phy_ac_tbl_write_unlock(dev, inner);

		b43_phy_ac_tbl_write_unlock(dev, saved);
	}
}

/*
 * TX power-control enable, the second PHY phase of the setup.
 *
 * The boundary with b43_phy_ac_adc_reset(), which set_channel calls just
 * before, is this block's marker: PHY.WR 0x1641, once in the cold capture.
 *
 * The power-target writes on 0x0644/0x0646 per core are a different phase,
 * after the core's BSS config: b43_phy_txpower_check() from b43_op_config().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   11389-11445]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7059-7133]
 */
static void b43_phy_ac_txpwrctrl_enable(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;
	unsigned int i;

	/*
	 * TX power-control enable (0x70[15:13]), framed by 0x1641, the
	 * broadcast gain register: peek 0x0070, clear 15:13, 0x1641 = 0x7f18,
	 * set 15:13, then per active core 0x0644 and 0x0678.
	 */
	b43_phy_read_log(dev, 0x0070);
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0x0000);
	b43_phy_write(dev, 0x1641, 0x7f18);
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);

	/*
	 * The 0x0644 pair is absent on a first bring-up. The 0x14 is not
	 * derivable from the SROM; see docs/retrace-todo.md.
	 */
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		for_each_set_bit(c, &dev->phy.ac->coremask, num_cores) {
			b43_phy_maskset(dev, 0x0644 + c * 0x200,
					(u16)~0x007f, 0x0014);
		}
	}

	for_each_set_bit(c, &dev->phy.ac->coremask, num_cores) {
		b43_phy_maskset(dev, 0x0678 + c * 0x200, (u16)~0x0004, 0x0000);
	}

	/*
	 * PLL lock check, the counterpart of channel_switch_prep(). The stock
	 * driver reads radio 0x090b once here, before releasing the RX freeze,
	 * after tens of milliseconds of setup; this polls instead, in case the
	 * PLL has not locked.
	 */
	{
		unsigned int tries;
		u16 stat = 0;

		for (tries = 0; tries < 100; tries++) {
			udelay(10);
			stat = b43_radio_read(dev, 0x090b);
			if (stat & 0x0100)
				break;
		}
		if (!(stat & 0x0100))
			b43warn(dev->wl,
			       "radio 2069: PLL lock timeout (0x90b=0x%04x)\n",
			       stat);
	}

	/* PHY update strobe. */
	b43_phy_ac_cca_pulse(dev);

	/*
	 * Release the RX gate and hold the ADC bracket, with a full write of
	 * 0x0140 as the stock driver does, so that bit 0x0800 ends up in the
	 * same state.
	 */
	b43_phy_ac_rx_gate_with_adc_hold(dev, false);

	/* ADC config. */
	b43_phy_write(dev, 0x0339, 0x0fff);
	/*
	 * Clear MHF slot 0 bit 13 through b43_phy_ac_mhf_maskset():
	 * b43_hf_write() covers three slots, the AC-PHY uses five.
	 */
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x2000, 0x0000);

	/*
	 * ADC config: 0x03ac/0x032c, then, from the second bring-up on, a
	 * second round with 0x03bf/0x0340.
	 */
	for (i = 0; i < 8; i++)
		b43_phy_write(dev, b43_phy_ac_adc_hi[i], 0x03ac);
	for (i = 0; i < 8; i++)
		b43_phy_write(dev, b43_phy_ac_adc_lo[i], 0x032c);
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		for (i = 0; i < 8; i++)
			b43_phy_write(dev, b43_phy_ac_adc_hi[i], 0x03bf);
		for (i = 0; i < 8; i++)
			b43_phy_write(dev, b43_phy_ac_adc_lo[i], 0x0340);
	}

	b43_phy_maskset(dev, 0x016e, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, 0x016e, (u16)~0x0001, 0x0001);
	b43_phy_maskset(dev, 0x016e, (u16)~0x0010, 0x0010);
	b43_phy_write(dev, 0x016f, 0x07d0);
	b43_phy_write(dev, 0x0170, 0x07d0);

	/*
	 * Arm the RX gate and drop the ADC bracket: the open of the bracket the
	 * idle-TSSI measurement runs in, so it goes with the measurement. Where
	 * that does not run the stock driver does not arm either (cold05:
	 * 0x0140, 0x0339 and 0x02ed counts match).
	 */
	if (!b43_phy_ac_may_calibrate_tx(dev))
		return;

	b43_phy_ac_rx_gate_with_adc_hold(dev, true);
	b43_phy_write(dev, 0x0339, 0x0000);

	/*
	 * PHY update strobe. The per-core TSSI-path enable is emitted by
	 * idle_tssi_meas(), not here.
	 */
	b43_phy_ac_cca_pulse(dev);
}


/*
 * CRS clip-detector thresholds: eight registers, four per core at a +0xc
 * stride, interleaved between cores. The low byte gets the bandwidth
 * threshold in chanspec_tail() (0x31 at 20 MHz on 5 GHz, 0x36 at 40 and 80)
 * and 0x34 in block E of rxiqcal_finalize().
 */
static const u16 b43_phy_ac_crs_regs[8] = {
	0x0324, 0x0330,
	0x0321, 0x032d,
	0x032a, 0x0336,
	0x0327, 0x0333,
};

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7501-7508, 30902-30909]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   3171-3178, 26062-26069]
 */
static void b43_phy_ac_crs_regs_write(struct b43_wldev *dev, u16 val)
{
	B43_AC_FN();
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_crs_regs); i++)
		b43_phy_maskset(dev, b43_phy_ac_crs_regs[i],
				(u16)~0x00ff, val);

	dev->phy.ac->crs_low = val;
}

/*
 * CRS min-power threshold, the low byte of 0x0321 through 0x0336: an entry of
 * the current width's row, plus 4 when cold.
 *
 * The ladder is the D6220's: three zero-terminated rows of fifteen u8, from
 * the .rodata of the 7.14.89 blob at +0x40e84, +0x40e94 and +0x40ea4; see
 * docs/crs-min-power.md. The agcombo uses the same: on both its sweeps every
 * threshold and every 0x0910 bank value is an entry of these rows.
 *
 * Open: the DSL-3580L's 6.30 blob has the rows at 0x1fa19c, 0x1fa1a8 and
 * 0x1fa1b4, eleven entries each, and the 20 and 80 MHz rows differ (two and
 * nine entries). On that board this table programs wrong values, and the
 * D6220-based gate does not see it.
 */
static const u8 b43_phy_ac_crs_ladder[3][15] = {
	/* BW20 */ { 45, 48, 51, 53, 54, 57, 60, 63, 66, 68, 70, 72, 75, 78, 80 },
	/* BW40 */ { 44, 46, 48, 50, 52, 54, 56, 58, 60, 63, 66, 69, 71, 74, 76 },
	/* BW80 */ { 41, 44, 46, 48, 50, 52, 55, 57, 60, 63, 65, 68, 70, 72, 74 },
};

/*
 * From the noise sample to the ladder index, per chain.
 *
 * The watchdog latches the SHM window 0x0308-0x0314, one 32-bit cell per
 * chain (0x0308 chain 0, 0x030c chain 1, 0x0310 chain 2, zero on the 4352).
 * Each sample becomes an index
 *
 *   n     = sample >> bandwidth step
 *   v     = how many thresholds of b43_phy_ac_crs_noise_th[] n reaches
 *   index = b43_phy_ac_crs_noise_idx[v] + the width's anchor
 *
 * and the chain's index in force is the mean of the last four, rounded up.
 * Chain 0 gives the threshold common to the eight CRS registers, the others
 * the 0x0910 bank; see b43_phy_ac_prog_bank_0910().
 *
 * Verified on every CRS block preceded by a sample, on both sweeps of the
 * D6220 and of the agcombo: 125 common thresholds and 148 bank values. A
 * window of three, five or six samples gets 3 to 10 of 126 wrong on the
 * D6220 cold sweep.
 *
 * The sample is a power integrated over the channel and scales with the
 * width (median 1384 at 20 MHz, 2525 at 40, 3832 at 80); divided by it, the
 * thresholds are the same at every width, and the anchor is 0 at 20 MHz,
 * +2 on the bonded widths.
 *
 * The captures pin 1536 and 2048 to three units, 1024 between 1001 and
 * 1025, 3072 between 2914 and 3147; 4096 -> 7 rests on a single sample, and
 * what lies above is not known.
 */
static const u16 b43_phy_ac_crs_noise_th[] = { 1024, 1536, 2048, 3072, 4096 };
static const u8 b43_phy_ac_crs_noise_idx[] = { 0, 1, 3, 4, 6, 7 };
static const u8 b43_phy_ac_crs_noise_anchor[3] = { 0, 2, 2 };

/*
 * Fold one latch of the noise window into the per-chain rings. Called from
 * the watchdog, where the window is latched; @sample holds one value per
 * chain.
 */
static void b43_phy_ac_crs_note_noise(struct b43_wldev *dev, const u16 *sample)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int bw = b43_phy_ac_bw_step(dev);
	unsigned int slot = ac->crs_ring_head;
	unsigned int core;

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		unsigned int n = sample[core] >> bw;
		unsigned int v = 0;
		while (v < ARRAY_SIZE(b43_phy_ac_crs_noise_th) &&
		       n >= b43_phy_ac_crs_noise_th[v])
			v++;

		ac->crs_ring[core][slot] = b43_phy_ac_crs_noise_idx[v] +
					   b43_phy_ac_crs_noise_anchor[bw];
	}

	ac->crs_ring_head = (slot + 1) % ARRAY_SIZE(ac->crs_ring[0]);
	if (ac->crs_ring_len < ARRAY_SIZE(ac->crs_ring[0]))
		ac->crs_ring_len++;
}

/*
 * The rings are emptied when the sub-band changes, on pa5g_group's
 * partition (5250 and 5500 MHz), not b43_ppr_ac_subband()'s.
 *
 * The reset is at the channel change, not in the index getter: in the
 * post-bring-up tail the sample latch precedes the block that reads the
 * index by a few ops, and a lazy reset there would drop it.
 *
 * Open: the cold sweeps cannot tell (every segment is a module load), and
 * on the hot sweep the first block of an `up` fits neither emptied rings
 * nor rings carried over from the previous segment.
 */
static void b43_phy_ac_crs_note_channel(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	u8 sb = (u8)b43_phy_ac_pa5g_group(dev, ac->cal_freq);

	if (sb != ac->crs_subband) {
		ac->crs_subband = sb;
		ac->crs_ring_len = 0;
		ac->crs_ring_head = 0;
	}
}

/*
 * Ladder index in force for @core: the ceiling of the mean of its ring. An
 * empty ring gives the floor.
 */
static unsigned int b43_phy_ac_crs_index(struct b43_wldev *dev,
					 unsigned int core)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int i, sum = 0;

	if (!ac->crs_ring_len)
		return 0;

	for (i = 0; i < ac->crs_ring_len; i++)
		sum += ac->crs_ring[core][i];

	return DIV_ROUND_UP(sum, ac->crs_ring_len);
}

/*
 * Record what a CRS write left on the hardware: chain 0 at @idx0, the common
 * threshold, and every other chain at the index its bank was taken from.
 */
static void b43_phy_ac_crs_note_prog(struct b43_wldev *dev, unsigned int idx0)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core;

	ac->crs_prog[0] = (u8)idx0;
	for (core = 1; core < B43_PHY_AC_MAX_CORES; core++)
		ac->crs_prog[core] = (u8)(ac->crs_ring_len
					  ? b43_phy_ac_crs_index(dev, core)
					  : idx0);
}

/*
 * Whether a noise latch moves the thresholds enough to be written: some
 * chain's index in force three ladder steps or more from the one it carries.
 * Only then does the stock driver rewrite the block on a latch; after a
 * calibration it writes regardless.
 *
 * On the 85 cold segments of the d6220 and the tg789vac, +-3 steps gives
 * every latch-driven block and no other on 80; +-2 gives 68.
 */
#define B43_PHY_AC_CRS_HYST	3

static bool b43_phy_ac_crs_moved(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core;

	if (!ac->crs_ring_len)
		return false;

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		int d = (int)b43_phy_ac_crs_index(dev, core) - ac->crs_prog[core];

		if (abs(d) >= B43_PHY_AC_CRS_HYST)
			return true;
	}
	return false;
}

static u8 b43_phy_ac_crs_min_pwr(struct b43_wldev *dev, unsigned int idx,
				 bool cold)
{
	u8 crs = b43_phy_ac_crs_ladder[b43_phy_ac_bw_step(dev)][idx];

	if (cold)
		crs = (u8)(crs + 4);	/* cold bump */

	return crs;
}

static void b43_phy_ac_op_pwork_60sec(struct b43_wldev *dev)
{
	bool cold = (dev->phy.ac->cal_cycles < 2);
	unsigned int idx;
	u8 crs;

	B43_AC_FN();
	B43_AC_BLOCK(dev, "pwork_60sec");

	idx = b43_phy_ac_crs_index(dev, 0);
	crs = b43_phy_ac_crs_min_pwr(dev, idx, cold);

	if (dev->phy.ac->cal_cycles < 2)
		dev->phy.ac->cal_cycles++;

	/*
	 * Only when the threshold changes. The core calls this every minute,
	 * and the stock driver does not rewrite on a cadence: over the 43 cold
	 * segments the CRS writes are chanspec_tail() and block E, plus at most
	 * one or two at bss-up, never at a multiple of 60 s (none of the six
	 * segments past 120 turns has one at turn 121).
	 */
	if (crs == dev->phy.ac->crs_low)
		return;

	b43_phy_ac_crs_regs_write(dev, crs);
	dev->phy.ac->crs_prog[0] = (u8)idx;
}

/*
 * The 0x0910-0x0913 bank: for every chain past chain 0, the distance between
 * its own CRS threshold and the common one written just before, which is
 * chain 0's. Four registers per chain at a 0x200 stride from 0x0710 (chain
 * 0's, never written). Only the chains the board wires: the d6220 has three
 * cores and two chains, and writes one bank. The value is a signed byte
 * (0xfb is -5) repeated in both halves of all four registers.
 *
 * Both thresholds come from b43_phy_ac_crs_index(), so the cold bump
 * cancels. Derivation and verification: docs/crs-min-power.md.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7509-7516, 30910-30917]
 */
static void b43_phy_ac_prog_bank_0910(struct b43_wldev *dev)
{
	const u8 *row = b43_phy_ac_crs_ladder[b43_phy_ac_bw_step(dev)];
	unsigned int ref = row[b43_phy_ac_crs_index(dev, 0)];
	unsigned int core;
	unsigned long others = dev->phy.ac->coremask & ~BIT(0);

	B43_AC_FN();

	for_each_set_bit(core, &others, dev->phy.ac->num_cores) {
		u16 base = 0x0710 + core * 0x200;
		s16 off = (s16)row[b43_phy_ac_crs_index(dev, core)] - (s16)ref;
		u16 hi = (u16)((off & 0xff) << 8);
		u16 lo = (u16)(off & 0xff);
		b43_phy_maskset(dev, base + 0, (u16)~0xff00, hi);
		b43_phy_maskset(dev, base + 0, (u16)~0x00ff, lo);
		b43_phy_maskset(dev, base + 2, (u16)~0xff00, hi);
		b43_phy_maskset(dev, base + 2, (u16)~0x00ff, lo);
		b43_phy_maskset(dev, base + 1, (u16)~0x00ff, lo);
		b43_phy_maskset(dev, base + 1, (u16)~0xff00, hi);
		b43_phy_maskset(dev, base + 3, (u16)~0x00ff, lo);
		b43_phy_maskset(dev, base + 3, (u16)~0xff00, hi);
	}
}

/*
 * The AFE gain registers, emitted at two points of rxiqcal_finalize() (block
 * E and the final tail) through b43_phy_ac_afe_gain_regs_reemit(), and
 * around txpwrctrl_setup() with their own condition:
 *   0x0070 set 0xe000        top-3 gain enable bits
 *   0x?644 = 0x14, mask 0x7f per-core AFE gain word
 *   0x?678 clear bit 2       per-core AFE bypass
 * The per-core writes go to every wired chain.
 *
 * The gain word belongs to the IQ/LO state, not to the AFE gain: it is
 * emitted wherever @iqlo_saved holds and nowhere else (cold05 and every
 * segment above 5250 MHz have the other three with no 0x?644). Block E
 * always qualifies, so the re-emit reads the flag rather than taking it.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   30728-30732, 36462-36466]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   26010-26014, 29004-29008]
 */
static void b43_phy_ac_afe_gain_regs(struct b43_wldev *dev, bool gain_word)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int c;

	B43_AC_FN();
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);
	if (gain_word)
		for_each_set_bit(c, &ac->coremask, ac->num_cores)
			b43_phy_maskset(dev, 0x0644 + c * 0x200, (u16)~0x007f,
					0x0014);
	for_each_set_bit(c, &ac->coremask, ac->num_cores)
		b43_phy_maskset(dev, 0x0678 + c * 0x200, (u16)~0x0004, 0);
}

static void b43_phy_ac_afe_gain_regs_reemit(struct b43_wldev *dev)
{
	b43_phy_ac_afe_gain_regs(dev, dev->phy.ac->iqlo_saved);
}

/*
 * Chanspec tail: the 35 ops right after the BW1A/BW1F loop in
 * channel_setup(), with the 0x019e gate held by the caller:
 *   1. clear bit 4 of 0x0164, undoing coeff_bank_init()'s set
 *   2. clear bits 14 and 15 of 0x030f
 *   3. the gain into 0x031c-0x031f: 0x00bf at 20 MHz on 5 GHz, 0x0100 at
 *      80 MHz on 5 GHz, 0x00ff on 2.4 GHz
 *   4. the eight CRS registers
 *   5. the 0x0910-0x0913 bank
 *   6. peek 0x03a9, then clear bits 0-6 and bit 11
 *   7. ten raw writes to 0x00ec-0x00f5, identical on all 104 segments of
 *      the d6220 cold and hot and agcombo cold sweeps
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7495-7533]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   3165-3203]
 */
static void b43_phy_ac_chanspec_tail(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const struct { u16 reg; u16 val; } post_bw1f_raw[10] = {
		{ 0x00ec, 0x0b54 }, { 0x00ed, 0x0290 }, { 0x00ee, 0x0004 },
		{ 0x00ef, 0x0a40 }, { 0x00f0, 0x0290 }, { 0x00f1, 0x0005 },
		{ 0x00f2, 0x0a06 }, { 0x00f3, 0x0240 }, { 0x00f4, 0x0005 },
		{ 0x00f5, 0x0080 },
	};
	u16 gain = 0x00bf;
	u16 crs = 0x0031;
	unsigned int crs_idx;
	unsigned int i;

	if (b43_current_band(dev->wl) != NL80211_BAND_5GHZ)
		gain = 0x00ff;
	else if (dev->wl->hw->conf.chandef.width == NL80211_CHAN_WIDTH_80)
		gain = 0x0100;

	/*
	 * CRS minimum power. The sample is latched by the watchdog, which runs
	 * after this, so the index in force is the one the previous cycle left:
	 * the first block of a cycle repeats the standing value, and only a
	 * sub-band change resets it to the floor.
	 */
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP ||
	    dev->phy.ac->cal_width != NL80211_CHAN_WIDTH_20) {
		/*
		 * First bring-up, or any bonded width: the observed constant,
		 * 0x3a at 20 MHz, 0x3c at 40 and 0x3d at 80, the first CRS
		 * value of every cold segment. On the ladder it is entry 4, 6
		 * and 7 with the cold bump.
		 */
		static const u8 first_idx[3] = { 4, 6, 7 };

		crs_idx = first_idx[b43_phy_ac_bw_step(dev)];
	} else
		crs_idx = b43_phy_ac_crs_index(dev, 0);
	crs = b43_phy_ac_crs_min_pwr(dev, crs_idx, true);

	dev->phy.ac->crs_written = (u8)crs;

	/*
	 * The clear of 0x0164 bit 4 is skipped at 80 MHz, where the bandwidth
	 * selector left the bit clear: the stock driver writes it twice per
	 * cycle at 20 and 40 MHz and once at 80.
	 */
	if (dev->phy.ac->cal_width != NL80211_CHAN_WIDTH_80)
		b43_phy_maskset(dev, 0x0164, (u16)~0x0010, 0x0000);
	b43_phy_maskset(dev, 0x030f, (u16)~0xc000, 0x0000);

	b43_phy_write(dev, 0x031c, gain);
	b43_phy_write(dev, 0x031d, gain);
	b43_phy_write(dev, 0x031e, gain);
	b43_phy_write(dev, 0x031f, gain);

	b43_phy_ac_crs_regs_write(dev, crs);
	b43_phy_ac_prog_bank_0910(dev);
	b43_phy_ac_crs_note_prog(dev, crs_idx);

	b43_phy_read_log(dev, 0x03a9);
	b43_phy_maskset(dev, 0x03a9, (u16)~0x007f, 0x0000);
	b43_phy_maskset(dev, 0x03a9, (u16)~0x0800, 0x0000);

	for (i = 0; i < ARRAY_SIZE(post_bw1f_raw); i++)
		b43_phy_write(dev, post_bw1f_raw[i].reg, post_bw1f_raw[i].val);
}

/*
 * Seed for one chain's RX gain LUT, emitted during the noise-shaping
 * tables. Two of the three fields come from the SPROM:
 *
 *   gainctx  = ((triso[core] + 4) << 1) + 2   -- the +2 is wl 7's
 *   hdr      = (elnagain[core] + 3) << 1      -- eLNA header
 *   lna1_idx = 0x02                           -- pinned by the stock driver
 *
 * lna1_idx is not read back from table 0x45 at 0x20 because that is written
 * three ops later, with fill_02, whose first element is that same 0x02.
 *
 * The core loop is the caller's; the stock driver emits this for every
 * silicon core, 2x2 boards included.
 *
 * On 5 GHz it uses rxgains_5gl (U-NII-1); 5gm and 5gh need a per-sub-band
 * selection. On 2.4 GHz rxgains_2g: the archer-t5e has elnagain 4 there and
 * 3 in 5gl, and writes the header as 0x0e on 2.4 GHz and 0x0c on 5 GHz.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   10578-10621, 10738-10781, 10898-10941]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6248-6291, 6408-6451, 6568-6611]
 */
static void b43_phy_ac_rxgain_init(struct b43_wldev *dev, unsigned int core)
{
	B43_AC_FN();
	static const u16 fill_07[10] = { 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 };
	static const u16 fill_02[10] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	const struct ssb_sprom_rxgains *rxgains =
		b43_current_band(dev->wl) == NL80211_BAND_2GHZ ?
		&sprom->rxgains_2g : &sprom->rxgains_5gl;
	u16 ps = (u16)(core * 0x0200);
	u16 ta = (u16)(0x0044 + core * 0x0020);
	u16 tb = (u16)(0x0045 + core * 0x0020);
	u16 gainctx = (u16)(((rxgains->triso[core] + 4) << 1) + 2);
	u16 hdr_val = (u16)((rxgains->elnagain[core] + 3) << 1);
	u16 lna1_idx = fill_02[0];
	u16 hdr_arr[2] = { hdr_val, hdr_val };

	b43_phy_read_log(dev, 0x073e + ps);
	b43_phy_write(dev,    0x073e + ps, 0x0000);
	b43_phy_maskset(dev,  0x06f9 + ps, (u16)~0x7f00,
			(u16)((gainctx << 8) & 0x7f00));
	b43_phy_maskset(dev,  0x06f9 + ps, (u16)~0x007f, lna1_idx & 0x007f);

	b43_actab_write_bulk(dev, ta, 0x0000, 16, 2, hdr_arr);
	b43_phy_write(dev,    0x173b, 0x002c);
	b43_phy_write(dev,    0x1726, 0x000c);
	b43_actab_write_bulk(dev, ta, 0x0020, 16, ARRAY_SIZE(fill_07), fill_07);
	b43_actab_write_bulk(dev, tb, 0x0020, 16, ARRAY_SIZE(fill_02), fill_02);
}

/*
 * Whether this channel carries the radar-detection duty at all
 * (IEEE80211_CHAN_RADAR, set by the regulatory domain). The detector poll
 * depends on this alone.
 */
static bool b43_phy_ac_chan_has_radar_duty(struct b43_wldev *dev)
{
	return !!(dev->phy.chandef->chan->flags & IEEE80211_CHAN_RADAR);
}

/*
 * Whether the calibrations that transmit may run on this channel. They
 * drive the tone generator, and on a channel with the radar duty nothing may
 * transmit until the channel availability check has completed; mac80211
 * tells the driver when it has (see ac->cac_pending in phy_ac.h).
 *
 * The captures agree: PHY 0x0380 (tone generator command) and radio 0x0b22
 * are accessed on every cold segment from ch48 down and on none from ch52
 * up, while the hot sweep runs them at every channel. A 5250 MHz threshold
 * would fit the captures as well, but would wrongly suppress U-NII-3, which
 * has no radar duty and no capture.
 *
 * The CAC owner differs: wl runs its own inside the attach, mac80211 runs
 * it after tuning the hardware. On b43 the check has finished by the time
 * an AP comes up, so these run where the cold captures have them absent;
 * see docs/retrace-todo.md.
 */
static bool b43_phy_ac_may_calibrate_tx(struct b43_wldev *dev)
{
	if (!b43_phy_ac_chan_has_radar_duty(dev))
		return true;
	return !dev->phy.ac->cac_pending;
}

/*
 * The calibrations proper, without the gate and the probe-phase tail. Two
 * callers: below, when the channel is available on entry, and
 * b43_phy_ac_bss_up(), where the capture puts them on a radar channel.
 */
static void b43_phy_ac_calibration_block(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Post-cal finalize, iterations 2 and 3. */
	b43_phy_ac_post_cal_finalize(dev);
	b43_phy_ac_post_cal_finalize_iter3(dev);
	b43_phy_ac_rxiqcal_apply(dev);
	b43_phy_ac_post_rxiqcal_stage2(dev);

	/* RX AFE calibration, about 1500 ops. */
	b43_phy_ac_rxcal_afe_calibrate(dev);
	b43_phy_ac_rxcal_afe_finalize_gain_luts(dev);

	/* First post-cal RXIQ round. */
	b43_phy_ac_txpwr_by_index(dev, B43_PHY_AC_TXPWR_INDEX_DEFAULT);
	b43_phy_ac_rxgain_defaults_pulse(dev);
	b43_phy_ac_radio_chain_range_setup(dev, true);
	b43_phy_ac_rxgain_perchan_config(dev);
	b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(dev);
	/* DDS tone seed. */
	b43_phy_ac_rxiqcal_dds_seed(dev);
	b43_phy_ac_rxiqcal_prep_second_iter(dev);
	b43_phy_ac_rxiqcal_run_meas_iters(dev);
	b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(dev);
	/* Clear the IQ coefficient tables 0x42/0x62/0x82. */
	b43_phy_ac_rxiqcal_coeff_tables_reset(dev);

	/*
	 * Second post-cal round: apply the coefficients measured by iterations
	 * 19-24.
	 */
	b43_phy_ac_txpwr_by_index(dev, B43_PHY_AC_TXPWR_INDEX_DEFAULT);
	b43_phy_ac_rxgain_defaults_pulse(dev);
	b43_phy_ac_radio_chain_range_setup(dev, false);
	b43_phy_ac_rxiqcal_apply_second_stage(dev);
	b43_phy_ac_rxgain_config_readback(dev);
	b43_phy_ac_rxgain_config_apply(dev);
	/* Radio IQ-cal configuration. */
	b43_phy_ac_radio_iqcal_config(dev);

	/* Loopback gain search; see b43_phy_ac_loopback_gain_search(). */
	b43_phy_ac_loopback_gain_search(dev);

	/*
	 * The measurement passes: seed a tone, then meas_post_dds_apply_v2. Two
	 * up to 40 MHz, six at 80, at the steps of b43_phy_ac_tone_steps.
	 */
	{
		unsigned int pass, n = b43_phy_ac_meas_passes(dev);

		for (pass = 0; pass < n; pass++) {
			b43_phy_ac_rxiqcal_dds_seed_tone(dev,
					b43_phy_ac_tone_steps[pass]);
			b43_phy_ac_rxiqcal_meas_post_dds_apply_v2(dev);
		}
	}

	/* Final RXIQ teardown. */
	b43_phy_ac_rxiqcal_apply_coefficients(dev);
	b43_phy_ac_radio_iqcal_teardown(dev);
	b43_phy_ac_rxiqcal_teardown_apply_defaults(dev);
	b43_phy_ac_rxiqcal_finalize(dev);
}

/*
 * On a radar channel the bss-up is when the channel becomes available:
 * mac80211 tuned the hardware before the check, cfg80211 kept its timer,
 * and hostapd starts the AP on the same channel, where b43_op_config()
 * does not switch again. What the switch deferred starts here, which is
 * where the capture puts it: the AMT row restored and, three ops later,
 * the calibration block, between watchdog turns and detector polls.
 *
 * The check's duration is not the driver's: in the captures the restore
 * comes 62.9-76.4 s after the arm, always between two watchdog turns, an
 * event of the stack. The measure block and latch that go with it belong
 * here too, not to the surrounding watchdog turn.
 */
void b43_phy_ac_bss_up(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;

	if (!ac->cac_pending)
		return;

	B43_AC_FN();
	ac->cac_pending = false;

	/*
	 * The bss-up opens its calibrations with b43_phy_ac_tempsense(), with
	 * the MAC suspended, right after the AMT row restore. The watchdog's
	 * reading has its own cadence; see b43_phy_ac_watchdog().
	 */
	if (b43_phy_ac_cal_reads_temp(dev)) {
		b43_mac_suspend(dev);
		b43_phy_ac_tempsense(dev);
		b43_mac_enable(dev);
	}

	b43_phy_ac_calibration_block(dev);

	/*
	 * The window latch and CRS block E close the calibrations but are not
	 * emitted here: the latch is the incoming sample,
	 * b43_phy_ac_noise_sample_done(), which brings block E with it, as
	 * after the first watchdog turn of a switch. Emitting the latch here
	 * too gives two where the stock driver has one.
	 */
	ac->crs_update_pending = true;
}

/*
 * The calibrations after the channel setup, in the stock driver's order:
 * post_cal_finalize iterations 2 and 3, rxiqcal iterations 1 to 24, the RX
 * AFE calibrate and finalize, a first txpwr round with its rxiqcal
 * iteration, a second with the gainctrl_final loop, then the RXIQ teardown
 * and finalize.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   14968-36041]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   10382-28591]
 */
static void b43_phy_ac_post_switch_calibrations(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * Entered with the MAC enabled and the classifier in WAITED mode
	 * (MAC_EN | RX_OFDM | RX_WAITED); post_cal_finalize() sets CLIP_ALL_DIS
	 * and suspends the MAC itself. RX_WAITED is the minimum precondition,
	 * and CCA_RESET is incompatible with any radio operation.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED,
			   B43_PHY_AC_STATE_CCA_RESET);

	/*
	 * The whole procedure transmits, so where b43_phy_ac_may_calibrate_tx()
	 * says no none of it runs, and b43_phy_ac_bss_up() runs it once the
	 * check completes. The gate is on the whole because the captures show
	 * it so: on cold05 (ch52/20) the port's surplus was a single run of
	 * 1714 ops from here to the end of rxiqcal_finalize(), bounded on both
	 * sides by matched ops. The registers only one phase touches (OBJ
	 * 0x00b8, PHY 0x0640/0x0840, 0x0211, 0x040f = 0x09ff of
	 * rxiqcal_finalize(), table 0x000e, the IQ coefficient tables, the
	 * radio IQ-cal registers) are present on every cold segment below 5250
	 * MHz and absent on all above. On the hot sweep, with the check long
	 * done, all of it runs everywhere.
	 *
	 * rxiqcal_finalize() being inside the gate also means the coefficients
	 * it saves for b43_phy_ac_down() keep a previous bring-up's values, as
	 * in the stock driver.
	 */
	if (!b43_phy_ac_may_calibrate_tx(dev)) {
		b43_phy_ac_post_bringup_tail(dev);
		return;
	}

	b43_phy_ac_calibration_block(dev);
	b43_phy_ac_post_bringup_tail(dev);
}


/*
 * The TX power adjust that follows a channel switch.
 *
 * b43 reaches it from b43_op_config() through b43_phy_txpower_check() and
 * the txpower_adjust_work, after b43_switch_channel() has returned and the
 * core has written the BSS configuration. The boundary is in the data: the
 * last op of the switch is the basic-rate map, the first of this one the
 * PLCP, with the core's BSS block in between.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   13665-14082]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9295-9714]
 */
static void b43_phy_ac_txpwr_adjust(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;
	u16 gate;

	/* The chain mask into 0x00cc, see b43_phy_ac_bss_cc(). */
	b43_phy_ac_bss_cc_update(dev, B43_PHY_AC_CHAIN_TXPWR);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ce,
			b43_phy_ac_beacon_pwr_offset_at(dev, B43_PHY_AC_CHAIN_TXPWR));
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00d0, 0x0000);

	/* Second pass of the twelve-rate loop. */
	b43_phy_ac_chainmask_block(dev, B43_PHY_AC_CHAIN_TXPWR);
	b43_phy_ac_prb_rsp_rate_po(dev, B43_PHY_AC_CHAIN_TXPWR);
	gate = b43_phy_ac_tbl_write_lock(dev);

	/*
	 * Second txpwrctrl_setup(), the same sequence op for op: the LUT comes
	 * from the same SPROM coefficients and the ppr values are unchanged.
	 */
	b43_phy_ac_txpwrctrl_setup(dev, 5000 + 5 * ac->cal_channel);

	/*
	 * Then the AFE gain registers, whose gain word is absent on a first
	 * bring-up.
	 */
	b43_phy_ac_tbl_write_unlock(dev, gate);
	b43_phy_ac_afe_gain_regs(dev, !(dev->phy.ac->status_mask &
				       B43_PHY_AC_STATE_FIRST_BRINGUP));
}

/*
 * Saved by tempsense_radio_setup(), restored by tempsense_radio_restore(),
 * in the stock driver's order.
 */
static const u16 b43_phy_ac_tempsense_radio_regs[7] = {
	0x016e, 0x000e, 0x0161, 0x0017, 0x015f, 0x0024, 0x0025,
};

/*
 * Radio side of one chain: read the seven registers above, then nine
 * masksets, each a MOD/RD/WR triplet in the trace -- 7 + 9 * 3 ops.
 */
static void b43_phy_ac_tempsense_radio_setup(struct b43_wldev *dev, u8 core)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_tempsense_radio_regs); i++)
		dev->phy.ac->tempsense_radio_saved[core][i] =
			b43_radio_read(dev,
				       b43_phy_ac_tempsense_radio_regs[i] + s);

	b43_radio_maskset(dev, 0x0161 + s, (u16)~0x4000, 0x4000);
	b43_radio_maskset(dev, 0x000e + s, (u16)~0x0001, 0x0001);
	b43_radio_maskset(dev, 0x0161 + s, (u16)~0x1000, 0x1000);
	b43_radio_maskset(dev, 0x0017 + s, (u16)~0x0001, 0x0001);
	b43_radio_maskset(dev, 0x0017 + s, (u16)~0x0002, 0x0000);
	b43_radio_maskset(dev, 0x015f + s, (u16)~0x2000, 0x2000);
	b43_radio_maskset(dev, 0x0025 + s, (u16)~0x03ff, 0x0091);
	b43_radio_maskset(dev, 0x015f + s, (u16)~0x4000, 0x4000);
	b43_radio_maskset(dev, 0x0024 + s, (u16)~0x0700, 0x0300);
}

static void b43_phy_ac_tempsense_radio_restore(struct b43_wldev *dev, u8 core)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_tempsense_radio_regs); i++)
		b43_radio_write(dev, b43_phy_ac_tempsense_radio_regs[i] + s,
				dev->phy.ac->tempsense_radio_saved[core][i]);
}

/*
 * One chain's measurement: arm the sampler on the chain (0x0394 = 0x0110 |
 * core), then four configurations of bits 1 and 2 of radio 0x?00e, each
 * through its gate on 0x?16e and followed by eight reads of PHY 0x0013. The
 * step order is fixed. What the bits select is not established: the reading
 * is in the difference between the steps with bit 1 set and clear, and bit
 * 2 moves it by a few LSB.
 */
static void b43_phy_ac_tempsense_chain(struct b43_wldev *dev, u8 core)
{
	B43_AC_FN();
	static const struct { u16 bit1, bit2; } step[4] = {
		{ 0x0002, 0x0000 },
		{ 0x0000, 0x0000 },
		{ 0x0002, 0x0004 },
		{ 0x0000, 0x0004 },
	};
	u16 s = (u16)(core * 0x200);
	unsigned int k, i;

	b43_phy_read_log(dev, 0x0393);
	b43_phy_write(dev, 0x0394, (u16)(0x0110 | core));
	b43_phy_write(dev, 0x0393, 0x8000);

	for (k = 0; k < ARRAY_SIZE(step); k++) {
		b43_radio_maskset(dev, 0x016e + s, (u16)~0x0002, 0x0002);
		b43_radio_maskset(dev, 0x000e + s, (u16)~0x0002, step[k].bit1);
		b43_radio_maskset(dev, 0x016e + s, (u16)~0x0001, 0x0001);
		b43_radio_maskset(dev, 0x000e + s, (u16)~0x0004, step[k].bit2);
		udelay(10);
		for (i = 0; i < 8; i++)
			dev->phy.ac->tempsense_samples[core][k][i] =
				b43_phy_read_log(dev, 0x0013);
	}
}

/*
 * Temperature sense: a four-step measurement on every wired chain, with the
 * chain's RX gain block and radio put into a measurement configuration and
 * put back at the end.
 *
 * Three sites run it, all with the MAC suspended: before the full
 * calibration (b43_phy_ac_op_channel_calibrate() when the channel is
 * available at once, b43_phy_ac_bss_up() after the availability check) and
 * from the watchdog every b43_phy_ac_temps_period() turns. The periodic copy follows the SROM's temps_period (10 s on the
 * d6220, 5 s on the tg789vac), the bss-up copy sits where brcmsmac's
 * PHY_PERICAL_UP_BSS takes the temperature, and the reading drifts with the
 * device's thermal state.
 *
 * The block is the same at every site. On all 100 blocks of the two cold
 * sweeps each register a chain drives is restored to the value read at the
 * head.
 *
 * The bss-up reading is not unconditional: the tg789vac skips it on one of
 * 40 cold segments, and nothing in the trace tells that cycle apart; see
 * "Tempsense at bss-up" in docs/retrace-todo.md.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   14407-14966, 32820-33379, 35466-36025]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9821-10380, 27216-27775]
 */
static void b43_phy_ac_tempsense(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;
	struct b43_phy_ac_meas_ctl ctl;
	u16 gate, gate_meas;
	unsigned int c, i;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_RX_OFDM,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CLIP_ALL_DIS |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	gate = b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0);

	for_each_set_bit(c, &ac->coremask, ac->num_cores)
		b43_phy_ac_rx_gain_regs_program(dev, c);
	for_each_set_bit(c, &ac->coremask, ac->num_cores)
		b43_phy_ac_tempsense_radio_setup(dev, c);

	gate_meas = b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_ac_meas_ctl_save(dev, &ctl);
	b43_phy_ac_rxgain_perchan_tail(dev);

	for_each_set_bit(c, &ac->coremask, ac->num_cores)
		b43_phy_ac_tempsense_chain(dev, c);

	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, gate);
	for_each_set_bit(c, &ac->coremask, ac->num_cores) {
		u16 s = (u16)(c * 0x200);

		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxgain_regs); i++)
			b43_phy_write(dev, b43_phy_ac_rxgain_regs[i] + s,
				      ac->rxgain_saved[c][i]);
	}
	for_each_set_bit(c, &ac->coremask, ac->num_cores)
		b43_phy_ac_tempsense_radio_restore(dev, c);

	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_ac_meas_ctl_restore(dev, &ctl);
	b43_phy_ac_tbl_write_unlock(dev, gate_meas);
}

/*
 * Whether a full calibration opens with a temperature reading: when
 * phycal_tempdelta is set, as brcmsmac does for the N-PHY on the bss-up
 * (PHY_PERICAL_UP_BSS). The captures agree on both calibrations that open
 * with one: the tg789vac (phycal_tempdelta=0 in NVRAM) reads on the 39
 * segments captured with `wl phycal_tempdelta 40` and skips both readings
 * on the four without; the d6220, the DSL-3580L and the agcombo (255 in
 * NVRAM) always read.
 *
 * 255 is the unprogrammed value, and the DSL-3580L reports 40 through
 * `wl phycal_tempdelta` with 255 in NVRAM; whether 40 is the driver's
 * default or set by its userspace is unverified. Only zero against non-zero
 * matters here.
 */
static bool b43_phy_ac_cal_reads_temp(struct b43_wldev *dev)
{
	return dev->dev->bus_sprom->phycal_tempdelta != 0;
}

/*
 * channel_calibrate: the calibrations that close a channel switch, run by
 * b43_op_config() in place of its final mac_enable. The temperature reading
 * needs the MAC suspended and the calibrations after it need it running,
 * so the enable sits between the two, where the stock driver emits it
 * (cold01 #14406-#14968, checked with annotate_enables.py).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   14407-36041]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9821-28591]
 */
static void b43_phy_ac_op_channel_calibrate(struct b43_wldev *dev)
{
	B43_AC_FN();
	if (b43_phy_ac_cal_reads_temp(dev)) {
		B43_AC_BLOCK(dev, "tempsense");
		b43_phy_ac_tempsense(dev);
	}
	b43_mac_enable(dev);
	B43_AC_BLOCK(dev, "post_switch_calibrations");
	b43_phy_ac_post_switch_calibrations(dev);
}

/*
 * Read the RF-chain inventory into ac->{num_cores, coremask}.
 *
 * Called from both op_init and op_software_rfkill, since b43_phy_init()
 * calls software_rfkill(false) before ops->init; a no-op once num_cores is
 * set.
 *
 * num_cores is the physical core count from PHY 0x000b, 3 on the 4352 and
 * the 4360 alike, and bounds every per-core access: touching a core past it
 * hangs the device. coremask is the chains wired to the front end, the
 * SROM's rxchain: 0x3 on the 4352, whose core 2 is present but unconnected,
 * 0x7 on the 4360. Per-core loops follow one or the other as the stock
 * driver does. Cores 0 and 1 are assumed when rxchain is unset.
 */
static void b43_phy_ac_probe_cores(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;

	if (ac->num_cores)
		return;

	ac->num_cores = b43_phy_read(dev, 0x000b) & 0x07;
	if (ac->num_cores > B43_PHY_AC_MAX_CORES)
		ac->num_cores = B43_PHY_AC_MAX_CORES;

	ac->coremask = dev->dev->bus_sprom->rxchain & 0x07;
	if (!ac->coremask)
		ac->coremask = 3;

	b43info(dev->wl, "phy-ac: num_cores=%u coremask=0x%lx\n",
	       ac->num_cores, ac->coremask);
}

/*
 * Pre-op_init analog front end, after the radio bring-up
 * (op_software_rfkill): pdet and the per-core RF front-end gates, so that
 * init_regs and the first channel_setup see a settled analog state. The
 * stock driver does it again inside channel_setup.
 *
 * pdet is the short form (no 0x0358 trio). Per core: PHY 0x0?29/0x0?21
 * bit 12, RAD 0x0?33 nibble -> 0x4000. Closes with PHY 0x01b0 bit 15.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   1202-1234]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   568-600]
 */
static void b43_phy_ac_pre_init_frontend(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int core, num_cores = dev->phy.ac->num_cores;

	b43_phy_ac_set_pdet_on_reset(dev, false);

	for (core = 0; core < num_cores; core++) {
		u16 stride = (u16)(core * 0x200);

		b43_phy_maskset(dev, 0x0729 + stride, (u16)~0x1000, 0x1000);
		b43_phy_maskset(dev, 0x0721 + stride, (u16)~0x1000, 0x1000);
		b43_radio_maskset(dev, 0x0033 + stride, (u16)~0xf000, 0x4000);
	}

	b43_phy_maskset(dev, 0x01b0, (u16)~0x8000, 0x8000);

	/* The stock driver reads PHY 0x0000 here, before the PMU regctl. */
	b43_phy_read_log(dev, 0x0000);
}

/*
 * The seven MHF clears the stock driver emits twice during a cold attach:
 * in the analog-on preamble, and again after the two MHF sets further on.
 * What the bits mean is unknown; this is transcribed.
 */
static void b43_phy_ac_mhf_bringup_clears(struct b43_wldev *dev)
{
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x0010, 0);
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0100, 0);
	b43_phy_ac_mhf_maskset(dev, 2, (u16)~0x2000, 0);
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0200, 0);
	b43_phy_ac_mhf_maskset(dev, 2, (u16)~0x1504, 0);
	b43_phy_ac_mhf_maskset(dev, 2, (u16)~0x1000, 0);
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x0006, 0);
}

/*
 * PHY attach/init entry (.init op). Each step carries its own capture range
 * on its own function. The first op on the wire is the num_cores read, the
 * last the trailing PMU regctl/GPIO pair.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   1202-5006]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   568-676]
 */
static int b43_phy_ac_op_init(struct b43_wldev *dev)
{
	B43_AC_FN();
	dev->phy.ac->tuned = false;
	if (dev->dev->bus_type != B43_BUS_BCMA) {
		b43err(dev->wl, "AC-PHY is supported only on BCMA bus!\n");
		return -EOPNOTSUPP;
	}

	if (dev->dev->chip_id != 0x4352 && dev->dev->chip_id != 0x4360) {
		b43err(dev->wl,
		       "AC-PHY: chip 0x%04x not in the implemented acphychipid dispatch {0x4352,0x4360}\n",
		       dev->dev->chip_id);
		return -EOPNOTSUPP;
	}

	/*
	 * PLLCTL3 is set before this runs, and its value depends on the board
	 * more than on the chip: the DSL-3580L (4352) only reads the PLL, the
	 * D6220 (4352) and the agcombo (4360) write 0xc31 and 0x100e early.
	 * 0x00133333 is the DSL's, not the 4352's, so it is only logged: there
	 * is no sound condition to refuse on.
	 */
	{
		u32 pllctl3 = bcma_chipco_pll_read(&dev->dev->bdev->bus->drv_cc,
						   BCMA_CC_PMU_PLL_CTL3);
		b43info(dev->wl, "AC-PHY: PLLCTL3 = 0x%08x on op_init entry\n",
		       pllctl3);
	}

	B43_AC_BLOCK(dev, "probe_cores");
	b43_phy_ac_probe_cores(dev);

	/*
	 * Analog front end, run by the stock driver between the radio bring-up
	 * and op_init proper, before the PMU regctl strobe.
	 */
	B43_AC_BLOCK(dev, "pre_init_frontend");
	b43_phy_ac_pre_init_frontend(dev);

	/*
	 * PMU regctl chip-dependent field [24:20] (0x1 on 4352, 0x2 on 4360)
	 * and GPIO.CTL clear. The PLLCTL2/3 writes and the regctl 0x2 strobe of
	 * this window are in bcma_pmu_{pll,resources}_init.
	 */
	B43_AC_BLOCK(dev, "pmu_regctl_gpio");
	if (dev->dev->chip_id == 0x4360)
		bcma_chipco_regctl_maskset(&dev->dev->bdev->bus->drv_cc, 0, ~0x01f00000, 0x200000);
	else
		bcma_chipco_regctl_maskset(&dev->dev->bdev->bus->drv_cc, 0, ~0x01f00000, 0x100000);

	bcma_chipco_gpio_control(&dev->dev->bdev->bus->drv_cc, 0xffff, 0);

	B43_AC_BLOCK(dev, "mode_init");
	b43_phy_ac_mode_init(dev);

	/*
	 * tables_init runs on a first bring-up only: its rows are PHY constants
	 * or PAPD state a warm cycle must not clear. The ids it writes that
	 * appear in a warm capture (0x04, 0x40, 0x42, 0x60, 0x62) come from
	 * txpwrctrl_setup() and the RX-IQ path; the first table select of a
	 * warm cycle is id 0x0a.
	 */
	if (dev->phy.do_full_init) {
		B43_AC_BLOCK(dev, "tables_init");
		b43_phy_ac_tables_init(dev);
	}

	B43_AC_BLOCK(dev, "init_regs");
	b43_phy_ac_init_regs(dev);

	/*
	 * No MHF configuration here on a warm cycle: the attach capture has it
	 * in the preamble, but a sweep segment has it at the very end
	 * (#29093-#29114 of 29162), before MAC.MCTRL and the MAC shared-memory
	 * block, i.e. some 22000 ops after the head of op_init. Whether it is
	 * the tail of this cycle or the head of the next is not settled, so it
	 * is left out.
	 */

	/* Latch the phase; see B43_PHY_AC_STATE_FIRST_BRINGUP. */
	if (dev->phy.do_full_init)
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_FIRST_BRINGUP;
	else
		dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_FIRST_BRINGUP;

	/*
	 * No trailing mac_suspend: the MAC is already disabled here, so it
	 * would only raise the refcount, and the suspend/enable pairs of
	 * op_switch_channel(), which b43_phy_init() calls next, would all
	 * become no-ops.
	 */
	return 0;
}

/*
 * Program the PHY analog front-end bank (the AFE_C1 registers at +0x1000,
 * 0x1720-0x173e) for @mode; see enum b43_phy_ac_afe_mode.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   554-561, 573-580, 1262-1265, 36531-36538]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   628-631, 29073-29080]
 */
static void b43_phy_ac_enable_afe(struct b43_wldev *dev,
				  enum b43_phy_ac_afe_mode mode)
{
	B43_AC_FN();
	switch (mode) {
	case B43_PHY_AC_AFE_OFF:
		b43_phy_write(dev, 0x173e, 0x0000);
		b43_phy_write(dev, 0x1739, 0x0000);
		b43_phy_write(dev, 0x173a, 0x0000);
		b43_phy_write(dev, 0x1725, 0x1fff);
		b43_phy_write(dev, 0x1729, 0x0000);
		b43_phy_write(dev, 0x1721, 0xffff);
		b43_phy_write(dev, 0x1728, 0x0000);
		b43_phy_write(dev, 0x1720, 0x03ff);
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_AFE_OFF;
		break;
	case B43_PHY_AC_AFE_ON:
		b43_phy_write(dev, 0x1728, 0x0080);
		b43_phy_write(dev, 0x1720, 0x0180);
		b43_phy_write(dev, 0x1729, 0x0000);
		b43_phy_write(dev, 0x1721, 0x5000);
		dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_AFE_OFF;
		break;
	}
}

/*
 * The analog arm: the AFE bank, the chan-select bit and the 0x0417/0x0416
 * pair, in that order.
 *
 * Three sites emit it, differing only in the pair: the entry of
 * b43_phy_ac_switch_analog_once() and its cold tail put back what the entry
 * saved, the bss-up step at the end of b43_phy_ac_rxiqcal_finalize() writes
 * 0x0000 and 0x0001 (26 cold and 51 of 52 warm segments).
 */
static void b43_phy_ac_afe_arm(struct b43_wldev *dev,
			       enum b43_phy_ac_afe_mode mode,
			       u16 v417, u16 v416)
{
	b43_phy_ac_enable_afe(dev, mode);
	b43_phy_maskset(dev, 0x0408, (u16)~0x0002, 0);
	b43_phy_write(dev, 0x0417, v417);
	b43_phy_write(dev, 0x0416, v416);
}

/*
 * regctl 0 bit 1 is a PMU resource request, used by the stock driver as a
 * bracket around the channel work: raised in the attach preamble and
 * lowered at its end, then on a later bring-up lowered on entry to
 * op_switch_channel() and raised on exit. The bit cannot be read back, so
 * its state is tracked.
 */
static void b43_phy_ac_pmu_req(struct b43_wldev *dev, bool on)
{
	struct bcma_drv_cc *cc = &dev->dev->bdev->bus->drv_cc;

	bcma_chipco_regctl_maskset(cc, 0x0000, ~0x00000002u,
				   on ? 0x00000002u : 0x00000000u);
	if (on)
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_PMU_REQ;
	else
		dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_PMU_REQ;
}

/*
 * Whether the cold preamble is due on this entry into switch_analog().
 *
 * b43 calls switch_analog(dev, true) from four sites: the attach reset
 * (right after b43_phy_allocate()), the core-init reset, b43_chip_init()
 * and b43_phy_init(). The stock driver splits the preamble between its
 * attach and its first up: the attach reset takes the first part (see
 * b43_phy_ac_op_switch_analog()), and one of the other three the rest.
 *
 * That one is the first entry with a channel: b43_phy_init() points
 * phy->chandef at the hardware config right before switch_analog(), and
 * nothing sets it earlier. The state bit keeps later entries out;
 * op_prepare_structs() clears it on every ifconfig up. Cold, this picks the
 * b43_phy_init() entry, warm the b43_chip_init() one, the same position in
 * the op stream.
 */
static bool b43_phy_ac_cold_preamble_due(struct b43_wldev *dev)
{
	if (!dev->phy.do_full_init)
		return false;
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_COLD_PREAMBLE)
		return false;
	if (!dev->phy.chandef || !dev->phy.chandef->chan)
		return false;
	return true;
}

/*
 * The end of the stock attach: the PMU resource request goes up, and stays
 * up until b43_phy_ac_cold_mac_preamble() in the first bring-up; three host
 * flags reach the shadow only.
 */
static void b43_phy_ac_attach_mac_preamble(struct b43_wldev *dev)
{
	B43_AC_FN();

	b43_phy_ac_pmu_req(dev, true);

	b43_phy_ac_mhf_maskset(dev, 2, (u16)~0x0040, 0);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);
}

/*
 * Host-flag preamble of a cold bring-up, up to the release of the PMU
 * request raised at attach. The stock driver interleaves it with core work
 * b43 does from main.c on its own schedule: the MACCONTROL write of each
 * core reset, the PSM jump and start around the ucode upload, and the GPOUT
 * clear and GPIO setup of b43_gpio_init(). Those stay out of the PHY: this
 * runs after the ucode is up, and a PSM_JMP0 would restart it.
 */
static void b43_phy_ac_cold_mac_preamble(struct b43_wldev *dev)
{
	B43_AC_FN();

	B43_AC_CORE_SITE(dev, CORE_RESET);

	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x0080, 0x0080);
	B43_AC_CORE_SITE(dev, CORE_RESET);
	B43_AC_CORE_SITE(dev, CORE_RESET);
	/*
	 * From here on a HOSTFn change reaches the cell: the slot 4 write above
	 * leaves no OBJ.WR, the slot 0 write below does.
	 */
	dev->phy.ac->mhf_writethrough = true;
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x0100, 0x0100);
	B43_AC_CORE_SITE(dev, CORE_RESET);

	/*
	 * Second write of the radar pulse threshold, only on channels with the
	 * radar duty: one write on ch36-48 and ch144-165, three from ch52 to
	 * ch140, on all 43 segments. The third is in the channel-setup tail,
	 * which no site here emits; see docs/retrace-todo.md.
	 */
	if (b43_phy_ac_chan_has_radar_duty(dev))
		b43_phy_ac_radar_thresh(dev);

	b43_phy_ac_pmu_req(dev, false);

	B43_AC_CORE_SITE(dev, UCODE_LOAD);
	b43_phy_ac_mhf_bringup_clears(dev);
	B43_AC_CORE_SITE(dev, UCODE_START);
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   528-583]
 */
static void b43_phy_ac_switch_analog_once(struct b43_wldev *dev, bool on,
					  bool cold)
{
	B43_AC_FN();
	u16 saved_417, saved_416;

	/*
	 * Only the cold entry, the attach reset, emits anything: the stock
	 * driver arms the AFE in its attach, 2.3 s before its up on cold01, and
	 * in the cold capture the AFE_ON bank appears three times, twice in
	 * this entry and once at bss-up. An emitting entry would also re-read
	 * the ten save registers each time.
	 *
	 * Nor is anything emitted on the way down: over 26 insmod/up/down/rmmod
	 * cycles there is no PHY, RAD or TBL op between the last PHY op and the
	 * rmmod, and the power-down bank appears once per cycle, from
	 * b43_phy_ac_mode_init(). b43_wireless_core_exit() must not touch the
	 * front end.
	 */
	if (!cold)
		return;

	/*
	 * The generic b43_phyop_switch_analog_generic() writes B43_MMIO_PHY0
	 * (0x3e6), which the stock driver never touches; like the N, HT and LCN
	 * PHYs, the AC drives its own analog front end.
	 *
	 * The unit is save, AFE bank, restore, twice back to back at the top of
	 * a cold attach: ten reads, the AFE bank on the override page, then
	 * chan-select bit 1 cleared and 0x0417/0x0416 put back. The first five
	 * reads are the registers the bank then masks through the 0x17xx
	 * override page (0x1739/0x173a/0x1725/0x1729/0x1721). Only 0x0417 and
	 * 0x0416 are restored; the other eight are read because the capture
	 * reads them.
	 */
	b43_phy_read_log(dev, 0x0739);
	b43_phy_read_log(dev, 0x073a);
	b43_phy_read_log(dev, 0x0725);
	b43_phy_read_log(dev, 0x0729);
	b43_phy_read_log(dev, 0x0721);
	b43_phy_read_log(dev, 0x0728);
	b43_phy_read_log(dev, 0x0720);
	b43_phy_read_log(dev, 0x0408);
	saved_417 = b43_phy_read_log(dev, 0x0417);
	saved_416 = b43_phy_read_log(dev, 0x0416);

	b43_phy_ac_afe_arm(dev, on ? B43_PHY_AC_AFE_OFF : B43_PHY_AC_AFE_ON,
			   saved_417, saved_416);

	/*
	 * Cold attach only: the radar pulse threshold, right after the unit
	 * above and before the MHF block and the second copy of the unit. One
	 * op on that register in the cold preamble of both boards, 0x0f00 on
	 * the 4352 and the 4360 alike. A later bring-up of the d6220 does not
	 * touch 0x02e4; the DSL (wl 6.30) writes 0x0800 there on its down->up,
	 * a version difference not reproduced (docs/retrace-todo.md). The
	 * 0x0800 set_channel writes on the 4360 is a different site.
	 */
	if (!cold)
		return;

	b43_phy_ac_radar_thresh(dev);


	/*
	 * Tail of the preamble: the seven MHF clears, then the second copy of
	 * the AFE bank and the triplet, reusing the values saved above.
	 *
	 * The cold-only gate for 0x02e4 is backed by the d6220's down-to-up
	 * path not emitting it; for the MHF clears it is by analogy, since the
	 * down-to-up captures start after this phase.
	 */
	b43_phy_ac_mhf_bringup_clears(dev);

	b43_phy_ac_afe_arm(dev, B43_PHY_AC_AFE_OFF, saved_417, saved_416);
}

/*
 * The analog block, once per entry.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   528-645]
 */
static void b43_phy_ac_op_switch_analog(struct b43_wldev *dev, bool on)
{
	bool attach = on && !dev->phy.ac->attach_preamble_done;

	B43_AC_FN();
	b43_phy_ac_switch_analog_once(dev, on, attach);

	if (attach) {
		b43_phy_ac_attach_mac_preamble(dev);
		dev->phy.ac->attach_preamble_done = true;
	} else if (on && b43_phy_ac_cold_preamble_due(dev)) {
		b43_phy_ac_cold_mac_preamble(dev);
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_COLD_PREAMBLE;
	}
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   691-1201, 36045-36542]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   72-567, 28595-29084]
 */
static void b43_phy_ac_op_software_rfkill(struct b43_wldev *dev, bool blocked)
{
	B43_AC_FN();
	if (dev->dev->chip_id != 0x4352 && dev->dev->chip_id != 0x4360) {
		b43err(dev->wl,	"AC-PHY: chip 0x%04x not in the implemented {0x4352,0x4360}\n",
			dev->dev->chip_id);
		return;
	}

	if (dev->phy.radio_ver != 0x2069) {
		b43err(dev->wl,	"AC-PHY: radio unsupported: 0x%04x \n", dev->phy.radio_ver);
		return;
	}

	/*
	 * RF blocked is the PHY half of `wl down`: b43_phy_exit() calls this
	 * from b43_wireless_core_exit(), and the stock down phase is one
	 * contiguous burst ending with the AFE_OFF bank and the PMU release,
	 * all of b43_phy_ac_down(). switch_analog(false), which b43 calls after
	 * it, has nothing left to emit, matching the capture.
	 */
	if (blocked) {
		b43_phy_ac_down(dev);
		return;
	}

	/*
	 * The chanspec heads the radio bring-up: right before the radio
	 * prologue on the cold attach, right before b43_radio_2069_init() on a
	 * warm cycle.
	 */
	b43_phy_ac_write_chanspec(dev);

	/*
	 * b43_phy_init() calls this with blocked=false before ops->init, so the
	 * core inventory is probed here as well.
	 */
	b43_phy_ac_probe_cores(dev);

	if (dev->phy.ac->num_cores == 0 || dev->phy.ac->coremask == 0) {
		b43err(dev->wl,	"AC-PHY: not initialized correctly\n");
		return;
	}

	b43_radio_2069_init(dev);
	b43_radio_2069_pwron(dev);
	b43_radio_2069_rccal(dev);

	/*
	 * Per-core AFE / radio-LPF stage, right after RC-cal, after the 0x08ea
	 * RCCAL_EN cleanup and before the op_init analog reset.
	 */
	b43_radio_2069_afe_lpf_stage(dev, 0x0800);

	/*
	 * software_rfkill ends with the radio front end up. The two-phase GPIO
	 * front end, the per-core PA bias and the final PMU regctl enable,
	 * which the stock driver emits only in steady state, come from the
	 * rxiqcal / TX-enable path.
	 */
}

/*
 * Common A1 restore, used by every rxcal iteration after the first: the RX
 * gate armed (classifier armed, ADC bracket dropped, clip detect disabled),
 * then 0x0339 = 0, disabling the RX-IQ cal accumulator, and a CCA pulse.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   14968-14980, 15744-15756, 16532-16544]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   10382-10394, 11158-11170, 11946-11958]
 */
static void b43_phy_ac_rxcal_a1_restore(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_ac_classctl_write_peeked(dev, true);
	b43_phy_ac_adc_hold(dev, false);
	b43_phy_ac_clip_det(dev, false);

	b43_phy_write(dev, 0x0339, 0);
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Post-cal finalize: the second idle-TSSI iteration, with the MAC up
 * (enabled at the end of op_switch_channel()). The base index is measured
 * each time; iteration 2 gives 0x0207 on core 0 where iteration 1 gave
 * 0x0206, and 0x0200 on core 1.
 */
void b43_phy_ac_post_cal_finalize(struct b43_wldev *dev)
{
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_MAC_EN | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_RX_WAITED,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CCA_RESET |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS);


	b43_phy_ac_rxcal_a1_restore(dev);
	b43_phy_ac_idle_tssi_meas(dev);
}

/*
 * RX-cal iteration 3, the third and last idle-TSSI measurement: an RX
 * suspend, the statistics clear, shm 0x00b8, the mac_suspend, a diagnostic
 * preamble, the A1 restore and the measurement, whose base index returns
 * to iteration 1's. What follows is b43_phy_ac_rxiqcal_apply().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   15728-16503]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   11142-11917]
 */
void b43_phy_ac_post_cal_finalize_iter3(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int c;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_MAC_EN | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_RX_WAITED,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CCA_RESET |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS);


	/*
	 * RX suspend: the same 0x0339 = 0x0fff that closes iteration 1 in
	 * op_switch_channel(), without the MHF and gate relock that follow it
	 * there.
	 */
	b43_phy_write(dev, 0x0339, 0x0fff);

	/*
	 * The statistics window clear and the noise-sample request, as on a
	 * watchdog tick; both skipped over a sample in flight.
	 */
	if (!dev->phy.ac->noise_pending) {
		b43_phy_ac_wd_stats_clear(dev);
		b43_phy_ac_noise_sample_request(dev);
	}

	/*
	 * A shared-memory word, between the clear and the mac_suspend. Its
	 * meaning is not known, but it is 0x7148 on all 66 writes (d6220 cold
	 * and hot, agcombo): independent of channel, width, chip and phase,
	 * never read, not in NVRAM. The DSL (wl 6.30) writes 0x1130 and others,
	 * so it follows the blob version. b43.h does not name it. A MAC cell,
	 * here for the same reason as b43_phy_ac_shm_readback_block().
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00b8, 0x7148);

	/*
	 * Make sure the MAC is suspended without nesting: op_switch_channel()
	 * already suspended it, and a second suspend would leave the refcount
	 * at 2, turning the pairs in rxiqcal_finalize() into no-ops. The
	 * caller expects the MAC suspended on return.
	 */
	if (!dev->mac_suspended)
		b43_mac_suspend(dev);

	/* Diagnostic preamble: 0x0070, 0x?640 per wired chain, then 0x0070. */
	b43_phy_read_log(dev, 0x0070);
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores)
		b43_phy_read_log(dev, 0x0640 + c * 0x200);
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0);

	b43_phy_ac_rxcal_a1_restore(dev);
	b43_phy_ac_idle_tssi_meas(dev);
}

/*
 * B2j write-back table: the 56 ops (54 MODs and two writes) the stock
 * driver emits to configure the gain stages after the RX-IQ measurement,
 * as core 0's registers, +0x200 for core 1. An entry is
 * { reg_off, mask_bits, val }: a non-zero mask_bits is a maskset, zero a
 * raw write.
 *
 * These are not per-chain I/Q coefficients: both chains and both phases
 * get the same values, while the coefficients differ. They are an arm and
 * disarm of the tone generator, the same on all 26 d6220 configurations;
 * the 0x0736 value is this call site's constant.
 */
struct b43_ac_b2j_op {
	u16 reg_off;
	u16 mask_bits;   /* 0 = raw write */
	u16 val;
	/*
	 * Added to the value per bandwidth step, in the field's weight: [2:0]
	 * grows by 1, [10:8] by 0x100, [13:11] by 0x800. See
	 * b43_phy_ac_bw_step().
	 */
	u16 bw_step;
	/*
	 * Where the value comes from when not the literal: one of the
	 * per-width fields of b43_phy_ac_rxgain_bw(), or the calling site's
	 * constant (different for 0x0736 between the two sites).
	 */
	u8 from;
};

enum {
	B2J_LIT = 0,
	B2J_73A_07,
	B2J_739_7E,
	B2J_73A_08,
	B2J_73A_60,
	B2J_SITE,
	/* f6 of the current width's RX-LPF cell, read back by the caller. */
	B2J_LPF,
};

static u16 b43_phy_ac_rxgain_field(const struct b43_phy_ac_rxgain_bw *g,
				   u8 from, u16 lit)
{
	switch (from) {
	case B2J_73A_07: return g->f73a_07;
	case B2J_739_7E: return g->f739_7e;
	case B2J_73A_08: return g->f73a_08;
	case B2J_73A_60: return g->f73a_60;
	}
	return lit;
}

static const struct b43_ac_b2j_op b43_phy_ac_b2j_ops[] = {

	{ 0x0728, 0x0002, 0x0000 },
	{ 0x0720, 0x0002, 0x0002 },
	{ 0x0721, 0x0040, 0x0040 },
	{ 0x0729, 0x0040, 0x0000 },
	{ 0x0721, 0x0080, 0x0080 },
	{ 0x0729, 0x0080, 0x0000 },
	{ 0x0721, 0x0020, 0x0020 },
	{ 0x0729, 0x0020, 0x0000 },
	{ 0x0721, 0x2000, 0x2000 },
	{ 0x0729, 0xe000, 0x0000 },

	{ 0x0721, 0x0800, 0x0800 },
	{ 0x0729, 0x0800, 0x0000 },
	{ 0x0721, 0x0400, 0x0400 },
	{ 0x0729, 0x0400, 0x0000 },
	{ 0x0721, 0x4000, 0x4000 },
	{ 0x0728, 0x3800, 0x0000 },
	{ 0x0721, 0x1000, 0x1000 },
	{ 0x0729, 0x1000, 0x0000 },

	{ 0x0720, 0x0020, 0x0020 },
	{ 0x0728, 0x0020, 0x0020 },
	{ 0x0720, 0x0040, 0x0040 },
	{ 0x0728, 0x0040, 0x0040 },
	{ 0x0720, 0x0010, 0x0010 },
	{ 0x0728, 0x0010, 0x0010 },
	{ 0x0721, 0x0100, 0x0100 },
	{ 0x0729, 0x0100, 0x0100 },
	{ 0x0727, 0x0004, 0x0004 },
	{ 0x073c, 0x0010, 0x0010 },
	{ 0x0724, 0x0000, 0x03ff },
	{ 0x0736, 0x0000, 0x0000, .from = B2J_SITE },

	{ 0x073a, 0x0007, 0x0000, .from = B2J_73A_07 },
	{ 0x0725, 0x0020, 0x0020 },
	{ 0x0739, 0x007e, 0x0000, .from = B2J_739_7E },
	{ 0x0725, 0x0002, 0x0002 },
	{ 0x073a, 0x0008, 0x0000, .from = B2J_73A_08 },
	{ 0x0725, 0x0040, 0x0040 },
	{ 0x073a, 0x0010, 0x0010 },
	{ 0x0725, 0x0080, 0x0080 },
	{ 0x073a, 0x0060, 0x0000, .from = B2J_73A_60 },
	{ 0x0725, 0x0100, 0x0100 },

	{ 0x0723, 0x0008, 0x0008 },
	{ 0x0723, 0x0010, 0x0010 },
	{ 0x0723, 0x0800, 0x0800 },
	/*
	 * Three fields that grow by one per bandwidth doubling: 0x0735[10:8]
	 * and 0x0735[13:11] go 3/4/5, like 0x0738[2:0] (cold17, cold18 at 40
	 * MHz and cold24 at 80; above 5250 MHz this calibration does not run
	 * cold).
	 */
	{ 0x0735, 0x0700, 0x0300, .bw_step = 0x0100 },
	{ 0x0735, 0x3800, 0x1800, .bw_step = 0x0800 },
	{ 0x0738, 0x0007, 0x0003, .bw_step = 1 },
	{ 0x0723, 0x0001, 0x0001 },
	{ 0x0735, 0x0001, 0x0000 },
	{ 0x0723, 0x0020, 0x0020 },
	{ 0x0735, 0x4000, 0x0000 },
	{ 0x0723, 0x0002, 0x0002 },
	{ 0x0735, 0x001e, 0x0008 },
	{ 0x0727, 0x0002, 0x0002 },
	{ 0x073c, 0x000e, 0x0004 },
	{ 0x0727, 0x0001, 0x0001 },
	{ 0x073c, 0x0001, 0x0001 },
};

/*
 * Per-core body of iteration 4, 79 ops; the B2g preamble (peek the gate,
 * tone generator off) is emitted once by the caller.
 *   B2h,  8 ops: 0x073e: write 0, clear bits 4-7, set bits 10 and 12
 *   B2i, 15 ops: peek the gain registers 0x0720-0x073c
 *   B2j, 56 ops: write-back from b43_phy_ac_b2j_ops
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   16631-16725, 16726-16820]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   12045-12139, 12140-12234]
 */
static void b43_phy_ac_rxiqcal_apply_body_core(struct b43_wldev *dev,
					       u16 core_off, u16 site_0736)
{
	B43_AC_FN();
	const struct b43_phy_ac_rxgain_bw *g = b43_phy_ac_rxgain_bw(dev);
	unsigned int i;

	/* B2h: 0x073e config: clear bits 4-7, set bits 10/12 */
	b43_phy_read_log(dev, 0x073e + core_off);
	b43_phy_write(dev,   0x073e + core_off, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0010, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0040, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0080, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x1000, 0x1000);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0400, 0x0400);

	/* B2i: peek the gain registers */
	b43_phy_read_log(dev, 0x0725 + core_off);
	b43_phy_read_log(dev, 0x0739 + core_off);
	b43_phy_read_log(dev, 0x073a + core_off);
	b43_phy_read_log(dev, 0x0721 + core_off);
	b43_phy_read_log(dev, 0x0729 + core_off);
	b43_phy_read_log(dev, 0x0720 + core_off);
	b43_phy_read_log(dev, 0x0728 + core_off);
	b43_phy_read_log(dev, 0x0724 + core_off);
	b43_phy_read_log(dev, 0x0736 + core_off);
	b43_phy_read_log(dev, 0x0723 + core_off);
	b43_phy_read_log(dev, 0x0735 + core_off);
	b43_phy_read_log(dev, 0x0737 + core_off);
	b43_phy_read_log(dev, 0x0738 + core_off);
	b43_phy_read_log(dev, 0x0727 + core_off);
	b43_phy_read_log(dev, 0x073c + core_off);

	/* B2j: write-back from the table */
	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_b2j_ops); i++) {
		const struct b43_ac_b2j_op *op = &b43_phy_ac_b2j_ops[i];
		u16 addr = op->reg_off + core_off;
		u16 val = op->val;

		if (op->bw_step)
			val = (u16)(val + op->bw_step *
				    b43_phy_ac_bw_step(dev));
		val = op->from == B2J_SITE ? site_0736 :
		      b43_phy_ac_rxgain_field(g, op->from, val);

		if (op->mask_bits == 0)
			b43_phy_write(dev, addr, val);
		else
			b43_phy_maskset(dev, addr,
					(u16)~op->mask_bits, val);
	}
}

static const u16 b43_phy_ac_chain_range_regs[7] = {
	0x001a, 0x001b, 0x001c, 0x001e, 0x001f, 0x0024, 0x0170,
};

static void b43_phy_ac_chain_range_save(struct b43_wldev *dev,
					unsigned int core)
{
	u16 *saved = dev->phy.ac->chain_range_saved[core];
	u16 s = (u16)(core * 0x200);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_chain_range_regs); i++)
		saved[i] = b43_radio_read(dev,
					  b43_phy_ac_chain_range_regs[i] + s);
}

/* Save 0x040f for a later write-back, then clear its bit 9. */
static void b43_phy_ac_rxcal_040f_save(struct b43_wldev *dev)
{
	dev->phy.ac->rxcal_040f_saved = b43_phy_read(dev, 0x040f);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
}

/*
 * RX-IQ compensation apply, phase B2, the fourth iteration of the cal
 * cycle, called with the MAC suspended:
 *
 *   B2a,  1 op:  write 0x0339 = 0x0fff, an RX suspend
 *   B2b, 14 ops: read the gain curve entries at 0x14 and 0x1e of table 0x20
 *   B2c,  6 ops: per-core RX-IQ path disable, peek then clear bit 0 of
 *                0x?78, on all three cores regardless of the coremask
 *   B2d, 12 ops: the common A1 restore
 *   B2e,  3 ops: two peeks of 0x0140, then a write of 0x0df4
 *   B2f, 50 ops: per-core radio commit, coremask-guarded
 *   B2g,  3 ops: peek the gate, tone generator off, once
 *   B2h-B2j:     per core, 79 ops, coremask-guarded
 *   B2k,  3 ops: set bits 6, 7 and 8 of 0x019e
 *   B2l, 18 ops: tone generator configuration, forward then reversed
 *   B2m, 32 ops: read-back and write of coefficient table 0x0007
 *
 * TODO: the coefficients are not computed at runtime yet.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   16504-16973]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   11918-12387]
 */
void b43_phy_ac_rxiqcal_apply(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_OFDM | B43_PHY_AC_STATE_RX_WAITED,
			   B43_PHY_AC_STATE_MAC_EN | B43_PHY_AC_STATE_RX_CCK |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_CLIP_ALL_DIS);

	/* B2a: RX suspend before the new cal block. */
	b43_phy_write(dev, 0x0339, 0x0fff);

	/* B2b: read the gain curve entries, unlocking after each read. */
	b43_phy_ac_read_chain_gaincurves(dev);

	/*
	 * B2c: per-core RX-IQ path disable, on all three cores with no coremask
	 * check, like clip_det() on 0x?d4.
	 */
	b43_phy_read_log(dev, 0x0678);
	b43_phy_maskset(dev, 0x0678, (u16)~0x0001, 0);
	b43_phy_read_log(dev, 0x0878);
	b43_phy_maskset(dev, 0x0878, (u16)~0x0001, 0);
	b43_phy_read_log(dev, 0x0a78);
	b43_phy_maskset(dev, 0x0a78, (u16)~0x0001, 0);

	/* B2d: common A1 restore */
	b43_phy_ac_rxcal_a1_restore(dev);

	/* B2e: extra classifier reset: a peek, then the peeked write. */
	b43_phy_read_log(dev, 0x0140);
	b43_phy_ac_classctl_write_peeked(dev, true);

	/*
	 * B2f, 50 ops: per-core radio commit, coremask-guarded unlike B2c.
	 */
	{
		u8 c;
		u8 num_cores = dev->phy.ac->num_cores;

		for_each_set_bit(c, &dev->phy.ac->coremask, num_cores) {
			u16 s = (u16)(c * 0x200);

			b43_phy_ac_chain_range_save(dev, c);
			b43_radio_maskset(dev, 0x001a + s, (u16)~0x00f0, 0x00b0);
			b43_radio_maskset(dev, 0x001f + s, (u16)~0x0004, 0x0004);
			b43_radio_maskset(dev, 0x0170 + s, (u16)~0x0100, 0x0100);
			b43_radio_maskset(dev, 0x0170 + s, (u16)~0x4000, 0);
			b43_radio_maskset(dev, 0x001e + s, (u16)~0x0004, 0);
			b43_radio_maskset(dev, 0x001a + s, (u16)~0x0300, 0);
		}
	}

	/*
	 * Iteration-4 body: B2g once, then B2h to B2j per wired chain.
	 */
	{
		u8 c;
		u8 num_cores = dev->phy.ac->num_cores;

		dev->phy.ac->rxcal_gate_saved =
			b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_ac_rxcal_040f_save(dev);

		for_each_set_bit(c, &dev->phy.ac->coremask, num_cores) {
			u16 s = (u16)(c * 0x200);
			b43_phy_ac_rxiqcal_apply_body_core(dev, s, 0x0152);
		}

		/* B2k: set bits 6/7/8 of the gate register. */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0x0040);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0x0080);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0x0100);

		/*
		 * B2l: tone generator configuration, a forward pass then a
		 * reversed one; see b43_phy_ac_rxgain_perchan_tail().
		 */
		b43_phy_ac_rxgain_perchan_tail(dev);

		/* B2m-B3: the chains' gain-curve entries into table 0x0007. */
		b43_phy_ac_tx_gain_bbmult_load(dev);
	}
}

/*
 * The ladders the calibrations load into table 0x000c before measuring a
 * chain: eighteen cells per chain, at 0x00-0x11 for chain 0 and 0x20-0x31
 * for chain 1, a gain in the high byte and an index in the low one. The
 * stock driver writes one of three, whole, six times per cold segment with
 * three wired chains and five with two:
 *
 *   after stage 2 of the RX IQ cal                    ladder A
 *   after chain 0 of the AFE cal                      ladder B
 *   before chain 2 of the AFE cal, first bring-up     see below
 *   before the second RX IQ iteration                 ladder A
 *   after chain 0 of the frequency-dependent pass     ladder B
 *   before chain 2 of that pass                       see below
 *
 * The "before chain 2" loads are the ramp where chain 2 is unwired (d6220),
 * and on the tg789vac ladder B up to ch144 and ladder A from ch149 at every
 * width (ABBABB on 37 segments, ABAABA on the 9 in UNII-3); see
 * b43_phy_ac_cal_like_chain0(). The agcombo, captured in U-NII-1 only,
 * writes ABBABB.
 *
 * Each load: peek and lock the gate, the eighteen pairs interleaved,
 * unlock.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   17999-18218, 18723-18942]
 */
struct b43_phy_ac_gain_ladder {
	u16 c0[18];
	u16 c1[18];
};

static const struct b43_phy_ac_gain_ladder b43_phy_ac_ladder_a = {
	{ 0x0100, 0x0200, 0x0300, 0x0500, 0x0800, 0x0b00, 0x1000,
	  0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007,
	  0x1607, 0x2007, 0x2d07, 0x4007 },
	{ 0x0100, 0x0200, 0x0300, 0x0500, 0x0800, 0x0b00, 0x1000,
	  0x1600, 0x2000, 0x2d00, 0x4000, 0x4001, 0x4002, 0x4003,
	  0x4004, 0x4005, 0x4006, 0x4007 },
};

static const struct b43_phy_ac_gain_ladder b43_phy_ac_ladder_b = {
	{ 0x0100, 0x0200, 0x0300, 0x0500, 0x0700, 0x0a00, 0x0f00,
	  0x0f01, 0x0f02, 0x0f03, 0x0f04, 0x0f05, 0x0f06, 0x0f07,
	  0x1507, 0x1e07, 0x2a07, 0x3c07 },
	{ 0x0100, 0x0200, 0x0300, 0x0500, 0x0700, 0x0a00, 0x0f00,
	  0x1500, 0x1e00, 0x2a00, 0x3c00, 0x3c01, 0x3c02, 0x3c03,
	  0x3c04, 0x3c05, 0x3c06, 0x3c07 },
};

/* The index alone, no gain: ladder A's low bytes. */
static const struct b43_phy_ac_gain_ladder b43_phy_ac_ladder_ramp = {
	{ 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 7 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7 },
};

static const struct b43_phy_ac_gain_ladder *
b43_phy_ac_ladder_before_chain2(struct b43_wldev *dev)
{
	if (!(dev->phy.ac->coremask & BIT(2)))
		return &b43_phy_ac_ladder_ramp;
	return b43_phy_ac_cal_like_chain0(dev, 2) ? &b43_phy_ac_ladder_a :
						    &b43_phy_ac_ladder_b;
}

static void b43_phy_ac_gain_ladder_write(struct b43_wldev *dev,
				const struct b43_phy_ac_gain_ladder *l)
{
	unsigned int i;

	B43_AC_FN();
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	for (i = 0; i < ARRAY_SIZE(l->c0); i++) {
		b43_actab_write_bulk(dev, 0x000c, 0x00 + i, 16, 1, &l->c0[i]);
		b43_actab_write_bulk(dev, 0x000c, 0x20 + i, 16, 1, &l->c1[i]);
	}
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * Close the gate scope rxiqcal_apply() opened and clear 12 slots of table
 * 0x000c, each write self-contained.
 *
 * The three 0x000c groups are fixed, not per coremask: the d6220 wires two
 * chains and clears all three. The agcombo wires all three, so no board
 * here tells this apart from a coremask loop.
 *
 * TODO: "stage2" is a provisional name.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   16974-17422]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   12388-12836]
 */
void b43_phy_ac_post_rxiqcal_stage2(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const u16 zero2[2] = { 0x0000, 0x0000 };
	static const u16 zero1[1] = { 0x0000 };
	u8 c;

	/* B4 preamble */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	/*
	 * The clock force stays up across the whole B4b configuration window,
	 * not only the CCA pulse: the capture has PHY.FGC 1 right before this
	 * write and 0 right after the 0x0382 = 0 that closes the window, with a
	 * cca_pulse nested inside (FGC 1,1,0,0 rather than 1,0,1,0). Only the
	 * core's clock: at the bus PSM_PHY_HDR moves for the nested CCA reset
	 * alone.
	 */
	b43_phy_force_clock(dev, true);
	b43_phy_write(dev, 0x0382, 0x8a09);

	/*
	 * B4a: three fixed per-core groups, each of four self-contained table
	 * writes at base (length 2), +3, +4 and +5 (length 1).
	 */
	for (c = 0; c < dev->phy.ac->num_cores; c++) {
		u16 base = (u16)(0x40 + c * 0x08);

		b43_actab_write_bulk_scoped(dev, 0x000c, base + 0, 16, 2, zero2);
		b43_actab_write_bulk_scoped(dev, 0x000c, base + 3, 16, 1, zero1);
		b43_actab_write_bulk_scoped(dev, 0x000c, base + 4, 16, 1, zero1);
		b43_actab_write_bulk_scoped(dev, 0x000c, base + 5, 16, 1, zero1);
	}

	/*
	 * B4b: one period of the tone in table 0x000e, transcribed from the
	 * d6220 ch36 capture; whether it depends on the channel or follows a
	 * formula is not known. The length is b43_phy_ac_tone_table_write()'s.
	 */
	{
		static const u32 b4b_tbl_data[B43_PHY_AC_TONE_PERIOD] = {
			0x0003e800, 0x0003b84d, 0x00032893, 0x00024cca,
			0x000134ee, 0x000000fa, 0x000eccee, 0x000db4ca,
			0x000cd893, 0x000c484d, 0x000c1800, 0x000c4bb3,
			0x000cdb6d, 0x000db736, 0x000ecf12, 0x00000306,
			0x00013712, 0x00024f36, 0x00032b6d, 0x0003bbb3,
		};

		b43_phy_ac_tone_table_write(dev, b4b_tbl_data, 1);
	}

	/*
	 * B4c, 33 ops: one self-contained read of 0x000c per wired chain at
	 * 0x63 + 4 * core (b43_phy_ac_bbmult_peek()), then 19 PHY ops: clear
	 * bit 0 of 0x0471, the sample-play triplet, set bit 0 of 0x0400, clear
	 * bits 0 and 2 of 0x0460, 0x0382 from 0x8a09 to 0x8009, wait on 0x0403,
	 * write 0x0400 = 0, then six MODs on 0x073a and 0x0725 over three fixed
	 * cores.
	 */
	{
		u8 c2;

		b43_phy_ac_bbmult_peek(dev);

		b43_phy_ac_run_samples(dev, b43_phy_ac_rxiqcal_kick_len(dev), true);
		udelay(5);

		/*
		 * Three cores regardless of the coremask: clear bit 8 of
		 * 0x?73a, set bit 10 of 0x?725.
		 */
		for (c2 = 0; c2 < 3; c2++) {
			u16 s = (u16)(c2 * 0x200);

			b43_phy_maskset(dev, 0x073a + s, (u16)~0x0100, 0);
			b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0400, 0x0400);
		}
	}

	b43_phy_ac_gain_ladder_write(dev, &b43_phy_ac_ladder_a);
}

/*
 * Keep the result of a commit iteration: only the six offsets that the tail
 * of rxcal_afe_calibrate() duplicates per antenna, since the other
 * iterations' results are not needed afterwards.
 */
static void b43_phy_ac_afe_res_store(struct b43_wldev *dev, u16 off,
				     const u16 *v, u8 n)
{
	static const u16 wanted[6] = { 0x40, 0x43, 0x48, 0x4b, 0x50, 0x53 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wanted); i++) {
		if (wanted[i] != off)
			continue;
		dev->phy.ac->afe_res[i].off = off;
		dev->phy.ac->afe_res[i].n = n;
		dev->phy.ac->afe_res[i].v[0] = v[0];
		if (n > 1)
			dev->phy.ac->afe_res[i].v[1] = v[1];
		return;
	}
}

/*
 * RX AFE calibration, phase B5. Each iteration emits:
 *   write 0x0381               cal parameter A
 *   optional pre-clears of table 0x000c
 *   write 0x0383 = 0x003d      cal parameter B
 *   write 0x0380 = CMD         the armed command, bit 15 set
 *   poll while 0x0380 & 0x8000
 *   read radio 0x0144 + core_off, the per-core result
 *   read table 0x000c at 0x8X (scratch), unlock by hand
 *   write table 0x000c at 0x4X with the result
 *
 * core_off is 0x0000 for the 0x8XXX command group (core 0), 0x0200 for
 * 0x9XXX (core 1) and 0x0400 for 0xaXXX (core 2).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   17423-17530, 17531-17628, 17629-17719, 17720-17855, 17856-17966,
 *   17967-17998, 18219-18318, 18319-18414, 18415-18507, 18508-18641,
 *   18642-18676, 18677-18722, 18943-19050, 19051-19148, 19149-19241,
 *   19242-19377, 19378-19496, 19497-19556, 23753-23847, 23848-23974,
 *   24195-24237, 24238-24286, 24287-24327, 24328-24448]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   12837-12942, 12943-13040, 13041-13135, 13136-13267, 13268-13348,
 *   13349-13418, 13639-13716, 13717-13768, 13769-13859, 13860-13995,
 *   13996-14118, 14119-14210, 14211-14318, 14319-14362, 14363-14437,
 *   14438-14535, 14536-14622, 14623-14684, 18881-18975, 18976-19098,
 *   19319-19411, 19412-19490, 19491-19565, 19566-19660]
 */
void b43_phy_ac_rxcal_afe_iter(struct b43_wldev *dev,
			       u16 cmd, u16 core_off,
			       const u16 *pre_clear_offs, u8 n_pre_clear,
			       u16 rd_off, u8 rw_len, u16 wr_off)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const u16 zero = 0;
	u8 i;
	u16 result[2];

	/*
	 * Cal parameter A, 0x11 per bandwidth step: 0x7976 at 20 MHz, 0x7987 at
	 * 40, 0x7998 at 80.
	 */
	b43_phy_write(dev, 0x0381,
		      (u16)(0x7976 + 0x11 * b43_phy_ac_bw_step(dev)));
	for (i = 0; i < n_pre_clear; i++)
		b43_actab_write_bulk_scoped(dev, 0x000c, pre_clear_offs[i],
					    16, 1, &zero);
	b43_phy_write(dev, 0x0383, 0x003d);
	b43_phy_write(dev, 0x0380, cmd);

	/*
	 * Wait for bit 15 to clear, with a finite budget: on an untested
	 * channel or board this must not spin in the kernel. Giving up degrades
	 * the calibration but is not fatal.
	 */
	{
		unsigned int tries;

		for (tries = 0; tries < 1000; tries++) {
			if (!(b43_phy_read(dev, 0x0380) & 0x8000))
				break;
			udelay(1);
		}
		if (tries == 1000)
			b43err(dev->wl,
			       "AC-PHY: RX AFE cal busy timeout (0x0380 stuck, cmd=0x%04x)\n",
			       cmd);
	}

	b43_radio_read_log(dev, 0x0144 + core_off);
	/* What is read is written back: 0x80 + n to 0x40 + n. */
	b43_actab_read_bulk(dev, 0x000c, rd_off, 16, rw_len, result);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);  /* manual unlock */
	b43_actab_write_bulk_scoped(dev, 0x000c, wr_off, 16, rw_len, result);
	b43_phy_ac_afe_res_store(dev, wr_off, result, rw_len);
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   17423-19694]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   12837-14822]
 */
void b43_phy_ac_rxcal_afe_calibrate(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * 18 iterations, six commands over three cores. The results are read
	 * back and written by rxcal_afe_iter(); the "result" in the notes below
	 * is what the capture shows, not a constant.
	 */

	/* ==== Core 0 (cmd 0x8XXX, RAD.RD 0x0144) ==== */

	/* Iter 1: cmd=0x8434, result 0x0301, pre-clears [0x43, 0x44] */
	{
		static const u16 pre[] = { 0x0043, 0x0044 };
		b43_phy_ac_rxcal_afe_iter(dev, 0x8434, 0x0000,
					  pre, 2, 0x0085, 1, 0x0045);
	}
	/* Iter 2: cmd=0x8334, result 0x0102 */
	{
		static const u16 pre[] = { 0x0043 };
		b43_phy_ac_rxcal_afe_iter(dev, 0x8334, 0x0000,
					  pre, 1, 0x0084, 1, 0x0044);
	}
	/* Iter 3: cmd=0x8084, result (0x0060, 0x0000) len=2 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x8084, 0x0000,
					  NULL, 0, 0x0080, 2, 0x0040);
	}
	/* Iter 4: cmd=0x8267, result 0xff01 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x8267, 0x0000,
					  NULL, 0, 0x0083, 1, 0x0043);
	}
	/* Iter 5: cmd=0x8056, result (0x0064, 0x000e) len=2 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x8056, 0x0000,
					  NULL, 0, 0x0080, 2, 0x0040);
	}
	/*
	 * Iteration 6, cmd 0x8234, observed result 0xfe02. Ends the core-0 group
	 * with ladder B; see b43_phy_ac_gain_ladder_write().
	 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x8234, 0x0000,
					  NULL, 0, 0x0083, 1, 0x0043);
		b43_phy_ac_gain_ladder_write(dev, &b43_phy_ac_ladder_b);
	}

	/* ==== Core 1 (cmd 0x9XXX, RAD.RD 0x0344) ==== */

	/* Iter 7: cmd=0x9434, result 0x0001, pre-clears [0x4b, 0x4c] */
	{
		static const u16 pre[] = { 0x004b, 0x004c };
		b43_phy_ac_rxcal_afe_iter(dev, 0x9434, 0x0200,
					  pre, 2, 0x008c, 1, 0x004d);
	}
	/* Iter 8: cmd=0x9334, result 0x0000 */
	{
		static const u16 pre[] = { 0x004b };
		b43_phy_ac_rxcal_afe_iter(dev, 0x9334, 0x0200,
					  pre, 1, 0x008b, 1, 0x004c);
	}
	/* Iter 9: cmd=0x9084, result (0x0020, 0x0000) len=2 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x9084, 0x0200,
					  NULL, 0, 0x0087, 2, 0x0048);
	}
	/* Iter 10: cmd=0x9267, result 0x0100 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x9267, 0x0200,
					  NULL, 0, 0x008a, 1, 0x004b);
	}
	/* Iter 11: cmd=0x9056, result (0x0022, 0x0004) len=2 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x9056, 0x0200,
					  NULL, 0, 0x0087, 2, 0x0048);
	}
	/*
	 * Iteration 12, cmd 0x9234, observed result 0x0100. Iteration 13's
	 * pre-clears at 0x53 and 0x54 come inside iteration 13, after 0x0381.
	 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x9234, 0x0200,
					  NULL, 0, 0x008a, 1, 0x004b);
	}

	/* The load before chain 2, on the first bring-up only. */
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)
		b43_phy_ac_gain_ladder_write(dev,
					     b43_phy_ac_ladder_before_chain2(dev));

	/* ==== Core 2 (cmd 0xaXXX, RAD.RD 0x0544) ==== */

	/* Iter 13: cmd=0xa434, result 0xfb03, pre-clears [0x53, 0x54] */
	{
		static const u16 pre[] = { 0x0053, 0x0054 };
		b43_phy_ac_rxcal_afe_iter(dev, 0xa434, 0x0400,
					  pre, 2, 0x0093, 1, 0x0055);
	}
	/* Iter 14: cmd=0xa334, result 0x05fe */
	{
		static const u16 pre[] = { 0x0053 };
		b43_phy_ac_rxcal_afe_iter(dev, 0xa334, 0x0400,
					  pre, 1, 0x0092, 1, 0x0054);
	}
	/* Iter 15: cmd=0xa084, result (0xfee0, 0x0080) len=2 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0xa084, 0x0400,
					  NULL, 0, 0x008e, 2, 0x0050);
	}
	/* Iter 16: cmd=0xa267, result 0x0d6e */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0xa267, 0x0400,
					  NULL, 0, 0x0091, 1, 0x0053);
	}
	/* Iter 17: cmd=0xa056, result (0xfed6, 0x00b6) len=2 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0xa056, 0x0400,
					  NULL, 0, 0x008e, 2, 0x0050);
	}
	/* Iter 18: cmd=0xa234, result 0x0764; the tail follows. */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0xa234, 0x0400,
					  NULL, 0, 0x0091, 1, 0x0053);
	}

	/*
	 * Tail of iteration 18, in three parts (a), (b) and (c). Part (a)
	 * duplicates the commit iterations' results per antenna. Unverified
	 * reading: (a) gain-override defaults for the second pass, (b) a reset
	 * of bits set in B4c, (c) a scaling of the gain channels.
	 */

	/* (a): 12 self-contained writes, six offsets over two antennas. */
	{
		/* Destination offset for each of the six commit iterations. */
		static const u16 dst[6] = {
			0x60, 0x62, 0x64, 0x66, 0x68, 0x6a,
		};
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(dst); i++) {
			const u16 *v = dev->phy.ac->afe_res[i].v;
			u8 n = dev->phy.ac->afe_res[i].n;

			if (!n)
				continue;
			b43_actab_write_bulk_scoped(dev, 0x000c, dst[i],
						    16, n, v);
			b43_actab_write_bulk_scoped(dev, 0x000c,
						    (u16)(dst[i] + 0x10),
						    16, n, v);
		}
	}

	/* (b): three individual ops */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_mask(dev, 0x0382, (u16)~0x8000);
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);

	/* (c): each chain's cal bbmult back into its two cells. */
	{
		unsigned int c;

		for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores)
			b43_phy_ac_bbmult_write(dev, c, &dev->phy.ac->bbmult_cal[c]);
	}

	/*
	 * Snapshot of the pass-1 results: rxiqcal_run_meas_iters() rewrites the
	 * same offsets with the second tone's results, but the final
	 * reapplications use these. See afe_res_cal in phy_ac.h.
	 */
	memcpy(dev->phy.ac->afe_res_cal, dev->phy.ac->afe_res,
	       sizeof(dev->phy.ac->afe_res_cal));
}

/*
 * The LOFT LUTs 0x0042/0x0062/0x0082 are two 8-bit fields per word, the LO
 * leakage compensation (di, dq) that the hardware picks by TX power index.
 * Arithmetic on them is per field: a borrow must not cross into the other.
 */
static u16 b43_phy_ac_loft_add(u16 a, u16 b)
{
	return (u16)((((a >> 8) + (b >> 8)) & 0xff) << 8 |
		     ((a + b) & 0xff));
}

/*
 * Base LOFT LUT entry per core and index: what rxiqcal_coeff_tables_reset()
 * writes and what the calibrated word is added onto.
 *
 * A step function whose step (where and how much) follows the channel's
 * pa5g group, as does the base. Read on the reset pass, which writes the
 * bare base, on all 87 segments of the two sweeps that run the phase:
 *
 *   group       0x0042      0x0062                    0x0082
 *   0 and 1     0           0                         f8fc, f6f2 from 0x21
 *   2           0           0                         fafb, f6f6 from 0x1d
 *   3           0           f6fb, ec00 from 0x1b      fafb, f6f6 from 0x1d
 *
 * Why the base exists, why the step falls where it does and why it follows
 * the pa5g partition is not known; only that it does.
 */
static u16 b43_phy_ac_loft_lut_base(struct b43_wldev *dev, unsigned int core,
				    unsigned int off)
{
	unsigned int grp = b43_phy_ac_pa5g_group(dev,
			5000 + 5 * dev->phy.ac->cal_channel);

	if (core == 0)
		return 0x0000;
	if (core == 1) {
		if (grp < 3)
			return 0x0000;
		return off <= 0x1a ? 0xf6fb : 0xec00;
	}
	if (grp < 2)
		return off <= 0x20 ? 0xf8fc : 0xf6f2;
	return off <= 0x1c ? 0xfafb : 0xf6f6;
}

/*
 * Fill the LOFT LUTs from the TX IQ/LO calibration, phase C.
 *
 * Preamble: a CCA pulse, 0x0382 = 0 (resetting the B4b configuration), and
 * the end of the clock force opened at 0x0382 = 0x8a09.
 *
 * Body, 2688 ops: 128 indices over three cores, one self-contained write
 * each to 0x42, 0x62 and 0x82. Every entry of a core's LUT is the LO leakage
 * word that core's commit iteration produced (what rxcal_afe_calibrate()
 * wrote to IQLOCAL 0x62 + 4*core) added per field onto the base entry; the
 * captured LUTs follow it entry for entry. The word changes from run to run
 * because the calibration does.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   19695-22769]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   14823-17897]
 */
void b43_phy_ac_rxcal_afe_finalize_gain_luts(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/* IQLOCAL 0x62, 0x66, 0x6a: afe_res slots 1, 3, 5. */
	static const u16 tbl[3] = { 0x0042, 0x0062, 0x0082 };
	u16 lo[3];
	unsigned int core, i;

	for (core = 0; core < dev->phy.ac->num_cores; core++)
		lo[core] = dev->phy.ac->afe_res[2 * core + 1].v[0];

	b43_phy_ac_cca_pulse(dev);
	b43_phy_write(dev, 0x0382, 0x0000);
	b43_phy_force_clock(dev, false);

	for (i = 0; i < 0x80; i++) {
		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 v = b43_phy_ac_loft_add(lo[core],
					b43_phy_ac_loft_lut_base(dev, core, i));

			b43_actab_write_bulk_scoped(dev, tbl[core], (u16)i,
						    16, 1, &v);
		}
	}
}

/*
 * Return the gain registers 0x0720-0x073e to their defaults and close with
 * the commit pulse, right after txpwr_by_index(). The same values on every
 * active chain.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   22842-22877, 27632-27667]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   17970-18005, 22844-22879]
 */
void b43_phy_ac_rxgain_defaults_pulse(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const struct { u16 off; u16 val; } gain_cfg[16] = {
		{ 0x073e, 0x0000 },
		{ 0x0721, 0x5000 },
		{ 0x0729, 0x1000 },
		{ 0x0720, 0x0180 },
		{ 0x0728, 0x0880 },
		{ 0x0724, 0x0000 },
		{ 0x0736, 0x0000 },
		{ 0x0723, 0x0000 },
		{ 0x0735, 0x0000 },
		{ 0x0737, 0x0000 },
		{ 0x0738, 0x0000 },
		{ 0x0727, 0x0004 },
		{ 0x073c, 0x0000 },
		{ 0x0725, 0x0600 },
		{ 0x0739, 0x0000 },
		{ 0x073a, 0x0180 },
	};
	unsigned int core, k;

	for_each_set_bit(core, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 stride = (u16)(core * 0x200);

		for (k = 0; k < ARRAY_SIZE(gain_cfg); k++)
			b43_phy_write(dev, gain_cfg[k].off + stride,
				      gain_cfg[k].val);
	}

	/* The gate and 0x040f the previous step saved, then the commit pulse */
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
		      dev->phy.ac->rxcal_gate_saved);
	b43_phy_write(dev, 0x040f, dev->phy.ac->rxcal_040f_saved);
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Mixed RX and TX chain setup: restore the per-chain radio registers saved
 * by the previous step of the cal, open a bracket (classifier, ADC hold, clip detect) that closes
 * to a net zero, and with @with_tune add per-chain read-back and tuning.
 * The steps are labelled 3a to 3r in the stock driver's order. What the
 * bits of 0x02ed/f1/f5/f9 and the closing pulse do is not known.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   22878-23027, 27668-27709]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   18006-18155, 22880-22921]
 */
void b43_phy_ac_radio_chain_range_setup(struct b43_wldev *dev, bool with_tune)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int core, k;

	/* 3a: restore the chain range registers saved last */
	for_each_set_bit(core, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 stride = (u16)(core * 0x200);

		for (k = 0; k < ARRAY_SIZE(b43_phy_ac_chain_range_regs); k++)
			b43_radio_write(dev,
					b43_phy_ac_chain_range_regs[k] + stride,
					dev->phy.ac->chain_range_saved[core][k]);
	}

	/*
	 * 3b: open the bracket: arm the classifier, then the peeked release, so
	 * that the peek samples the armed state.
	 */
	b43_phy_ac_classctl_write(dev, true);
	b43_phy_ac_classctl_write_peeked(dev, false);

	/* 3c-3d: ADC hold, clip detect enabled (RX released). */
	b43_phy_ac_adc_hold(dev, true);
	b43_phy_ac_clip_det(dev, true);

	b43_phy_write(dev, 0x0339, 0x0fff);

	/* 3f: 0x?78 = 0x0008 on every silicon core */
	for (core = 0; core < dev->phy.ac->num_cores; core++)
		b43_phy_write(dev, 0x0678 + core * 0x200, 0x0008);

	/* 3g: read the chains' gain curve entries, full variant only. */
	if (with_tune) {
		b43_phy_ac_read_chain_gaincurves(dev);

		/* 3h: peek and clear bit 0 of 0x?78 on every silicon core */
		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 reg = 0x0678 + core * 0x200;

			b43_phy_read_log(dev, reg);
			b43_phy_maskset(dev, reg, (u16)~0x0001, 0);
		}
	}

	/*
	 * 3i-3k: close the bracket, a net zero against the state on entry.
	 */
	b43_phy_ac_rx_gate_with_adc_hold(dev, true);

	b43_phy_write(dev, 0x0339, 0x0000);

	/* 3m: commit pulse */
	b43_phy_ac_cca_pulse(dev);

	if (!with_tune)
		return;

	/* 3n: a diagnostic peek, then arm the classifier. */
	b43_phy_read_log(dev, 0x0140);
	b43_phy_ac_classctl_write_peeked(dev, true);

	/* 3o-3r: per chain, seven radio reads and six radio MODs. */
	for_each_set_bit(core, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 s = (u16)(core * 0x200);

		b43_phy_ac_chain_range_save(dev, core);
		/* each maskset is MOD + RD + WR */
		b43_radio_maskset(dev, 0x001a + s, (u16)~0x00f0, 0x00b0);
		b43_radio_maskset(dev, 0x001f + s, (u16)~0x0004, 0x0004);
		b43_radio_maskset(dev, 0x0170 + s, (u16)~0x0100, 0x0100);
		b43_radio_maskset(dev, 0x0170 + s, (u16)~0x4000, 0);
		b43_radio_maskset(dev, 0x001e + s, (u16)~0x0004, 0);
		b43_radio_maskset(dev, 0x001a + s, (u16)~0x0300, 0);
	}
}

/*
 * Tail shared by rxgain_perchan_config(),
 * b43_phy_ac_rxiqcal_meas_readback_kick_tail() and the B2l block of
 * rxiqcal_apply(): per active chain, read 0x?739, 0x?73a and 0x?725 and
 * write them back with bit 7, nothing and bit 2 set; then, from the highest
 * chain down, put back what was read. The same at every occurrence.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   16824-16847, 23226-23249, 28483-28506, 28855-28878, 29231-29254,
 *   29556-29579, 29779-29802, 30081-30104, 33044-33067, 35690-35713]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   12238-12261, 18354-18377, 23695-23718, 24067-24090, 24443-24466,
 *   24768-24791, 24991-25014, 25415-25438, 27440-27463]
 */
static void b43_phy_ac_rxgain_perchan_tail(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int num_cores = dev->phy.ac->num_cores;
	u8 coremask = dev->phy.ac->coremask;
	unsigned int c;

	/* Per-chain gain state: saved, driven, put back in reverse order. */
	u16 s739[3], s73a[3], s725[3];

	for_each_set_bit(c, &dev->phy.ac->coremask, num_cores) {
		u16 s = (u16)(c * 0x200);


		s739[c] = b43_phy_read_log(dev, 0x0739 + s);
		b43_phy_write(dev,    0x0739 + s, s739[c] | 0x0080);
		s73a[c] = b43_phy_read_log(dev, 0x073a + s);
		b43_phy_write(dev,    0x073a + s, s73a[c]);
		s725[c] = b43_phy_read_log(dev, 0x0725 + s);
		b43_phy_write(dev,    0x0725 + s, s725[c] | 0x0004);
	}

	/* Reverse pass, highest chain first. */
	for (c = num_cores; c-- > 0; ) {
		u16 s = (u16)(c * 0x200);

		if (!((coremask >> c) & 1))
			continue;

		b43_phy_write(dev, 0x0725 + s, s725[c]);
		b43_phy_write(dev, 0x073a + s, s73a[c]);
		b43_phy_write(dev, 0x0739 + s, s739[c]);
	}
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   23028-23249]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   18156-18377]
 */
void b43_phy_ac_rxgain_perchan_config(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core;

	dev->phy.ac->rxcal_gate_saved =
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_ac_rxcal_040f_save(dev);

	/*
	 * Per active chain, the same body as the B2 of the RX-IQ apply (0x073e,
	 * the 15 reads and b43_phy_ac_b2j_ops), with 0x022a on 0x0736. The
	 * tg789vac-v2 does it on core 2 too (its cold01).
	 */
	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		b43_phy_ac_rxiqcal_apply_body_core(dev, (u16)(core * 0x200),
						   0x022a);
	}

	/* Bridge: set bits 6/7/8 of the gate register. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0x0040);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0x0080);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0x0100);

	/* Common tail; see b43_phy_ac_rxgain_perchan_tail(). */
	b43_phy_ac_rxgain_perchan_tail(dev);
}

/*
 * Load each wired chain's GAINCTRLBBMULT entry, as read by
 * b43_phy_ac_read_chain_gaincurves(): its three TX gain code cells into
 * table 0x0007 at 0x100 + core + 3 * i, each read back first and discarded,
 * then its bbmult into 0x63/0x73 + 4 * core, saving the cell first. With
 * 0x14 and 0x1e that is (0x0000, 0x4f7f, 0x00f3) with bbmult 0x40 and
 * (0x0000, 0x2f7f, 0x00f3) with 0x3c on every board.
 */
static void b43_phy_ac_tx_gain_bbmult_load(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core, i;
	bool first_core = true;

	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		u16 lo = (u16)(0x0063 + 4 * core);
		u16 hi = (u16)(0x0073 + 4 * core);

		/* Bridge between cores: an idempotent lock MOD only. */
		if (!first_core)
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		first_core = false;

		for (i = 0; i < 3; i++)
			b43_actab_read_log(dev, 7, 0x0100 + core + 3 * i, 1);
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 7, (u16)(0x0100 + core + 3 * i),
					     16, 1, &ac->gaincurve_coeff[core][i]);

		b43_actab_read_bulk(dev, 0xc, lo, 16, 1, &ac->bbmult_saved[core]);
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0xc, lo, 16, 1, &ac->bbmult_cal[core]);
		b43_actab_write_bulk(dev, 0xc, hi, 16, 1, &ac->bbmult_cal[core]);
	}
}

/*
 * RX-IQ compensation on the TX side: the TX gain code in table 0x0007 (the
 * area txpwr_by_index() uses) and the baseband multiplier, per chain.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   23250-23377]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   18378-18505]
 */
void b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	b43_phy_ac_tx_gain_bbmult_load(dev);

	/* Postamble */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * RX-IQ DDS/NCO seed (phase E block 2, 111 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   23378-23492]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   18506-18620]
 */
void b43_phy_ac_rxiqcal_dds_seed(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * One period of the DDS/NCO tone, transcribed from the d6220 ch36
	 * capture; the table length follows the width, see
	 * b43_phy_ac_tone_table_write().
	 */
	static const u32 dds_seed[B43_PHY_AC_TONE_PERIOD] = {
		0x0003e800, 0x0003b84d, 0x00032893, 0x00024cca, 0x000134ee,
		0x000000fa, 0x000eccee, 0x000db4ca, 0x000cd893, 0x000c484d,
		0x000c1800, 0x000c4bb3, 0x000cdb6d, 0x000db736, 0x000ecf12,
		0x00000306, 0x00013712, 0x00024f36, 0x00032b6d, 0x0003bbb3,
	};
	static const u16 zeros[2] = { 0, 0 };

	/* Arm command, with the clock forced until the window closes. */
	b43_phy_force_clock(dev, true);
	b43_phy_write(dev, 0x0382, 0x8a09);

	/* Clear three two-slot areas of table 0x000c. */
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0040, 16, 2, zeros);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0048, 16, 2, zeros);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0050, 16, 2, zeros);

	b43_phy_ac_tone_table_write(dev, dds_seed, 1);
}

/*
 * RX-IQ preparation of the second iteration (216 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   23493-23752]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   18621-18880]
 */
void b43_phy_ac_rxiqcal_prep_second_iter(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int core;

	/*
	 * Segment A: read back the per-core bbmult. The gate is unlocked on
	 * entry, after the previous function's write_bulk_scoped().
	 */
	b43_phy_ac_bbmult_peek(dev);

	/* Segment B: kick sequence of the RX-IQ correlator. */
	b43_phy_ac_run_samples(dev, b43_phy_ac_rxiqcal_kick_len(dev), true);
	udelay(5);

	/* Every silicon core: clear bit 8 of 0x?73a, set bit 10 of 0x?725. */
	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		b43_phy_maskset(dev, 0x073a + s, (u16)~0x0100, 0);
		b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0400, 0x0400);
	}

	/* Segment C: ladder A. */
	b43_phy_ac_gain_ladder_write(dev, &b43_phy_ac_ladder_a);
}

/*
 * RX-IQ measurement iterations (group 4, 519 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   23753-24448]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   18881-19660]
 */
void b43_phy_ac_rxiqcal_run_meas_iters(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Frequency-dependent IQ correction: two iterations per chain, commands
	 * 0x?084 then 0x?056, the two taps of the complex filter, consistent
	 * with the tones seeded by the DDS.
	 *
	 * The coefficients are computed by the hardware: rxcal_afe_iter() arms
	 * the cal through 0x0380, waits on bit 15, reads two cells at rd_off
	 * and writes them back at wr_off (in the capture 0x0c[0x8e]/[0x8f] to
	 * [0x50]/[0x51]). Both iterations of a chain write the same wr_off, so
	 * afe_res ends up with the second one's result.
	 */
	static const struct {
		u16 cmd;
		u16 core_off;
		u16 rd_off;
		u16 wr_off;
	} iters[6] = {
		{ 0x8084, 0x0000, 0x0080, 0x0040 }, /* 19 */
		{ 0x8056, 0x0000, 0x0080, 0x0040 }, /* 20 */
		{ 0x9084, 0x0200, 0x0087, 0x0048 }, /* 21 */
		{ 0x9056, 0x0200, 0x0087, 0x0048 }, /* 22 */
		{ 0xa084, 0x0400, 0x008e, 0x0050 }, /* 23 */
		{ 0xa056, 0x0400, 0x008e, 0x0050 }, /* 24 */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(iters); i++) {
		b43_phy_ac_rxcal_afe_iter(dev, iters[i].cmd, iters[i].core_off,
					  NULL, 0,
					  iters[i].rd_off, 2,
					  iters[i].wr_off);

		/*
		 * After a chain's two iterations, the ladder, if a later chain
		 * is wired: after chain 0 on the d6220, after chains 0 and 1 on
		 * the agcombo and the tg789vac.
		 */
		if ((i & 1) && (dev->phy.ac->coremask & ~GENMASK(i / 2, 0)))
			b43_phy_ac_gain_ladder_write(dev, i == 1 ?
					&b43_phy_ac_ladder_b :
					b43_phy_ac_ladder_before_chain2(dev));
	}
}

/*
 * RX-IQ post-measurement apply (phase F segment A, 32 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   24449-24487]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   19661-19699]
 */
void b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Per-core bbmult, the values rxiqcal_apply_tx_gain_bbmult() writes, at
	 * 0x63/0x73 + 4 * core.
	 */
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core;

	/* Three standalone ops. */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_mask(dev, 0x0382, (u16)~0x8000);
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);

	/* Per core: peek and lock, the two bbmult cells, unlock. */
	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		u16 lo = (u16)(0x0063 + 4 * core);
		u16 hi = (u16)(0x0073 + 4 * core);
		const u16 *bbmult = &ac->bbmult_cal[core];

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* Final pulse and reset, closing the clock force opened at 0x8a09. */
	b43_phy_ac_cca_pulse(dev);
	b43_phy_write(dev, 0x0382, 0x0000);
	b43_phy_force_clock(dev, false);
}

/*
 * Coefficient table reset (2688 ops): the LOFT LUTs back to their base.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   24488-27559]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   19700-22771]
 */
void b43_phy_ac_rxiqcal_coeff_tables_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const u16 tbl[3] = { 0x0042, 0x0062, 0x0082 };
	unsigned int off, core;

	for (off = 0; off < 128; off++) {
		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 v = b43_phy_ac_loft_lut_base(dev, core, off);

			b43_actab_write_bulk_scoped(dev, tbl[core], (u16)off,
						    16, 1, &v);
		}
	}
}

/*
 * IQ-cal second stage apply (47 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   27710-27768]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   22922-22980]
 */
void b43_phy_ac_rxiqcal_apply_second_stage(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * The results the second AFE pass measured on each chain (see
	 * b43_phy_ac_rxiqcal_run_meas_iters()), reapplied at 0x60 + 4 * core of
	 * table 0x000c instead of 0x40 + 8 * core.
	 */
	unsigned int c;

	b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RST2RX);

	/* Zero 0x?6a0/0x?6a1 on every wired chain. */
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		b43_phy_write(dev, 0x06a0 + c * 0x200, 0x0000);
		b43_phy_write(dev, 0x06a1 + c * 0x200, 0x0000);
	}

	/* A real MOD (mask=0x0001), not a normalised AND. */
	b43_phy_maskset(dev, 0x0211, (u16)~0x0001, 0);

	/*
	 * One read/write pair per wired chain; afe_res holds each chain's
	 * result at 2 * core, see afe_res_store().
	 */
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 off = (u16)(0x0060 + 4 * c);

		b43_actab_read_log(dev, 0x000c, off, 2);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_actab_write_bulk_scoped(dev, 0x000c, off, 16, 2,
					    dev->phy.ac->afe_res[2 * c].v);
	}

	/* Close: peek and relock. */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
}

/*
 * RX gain configuration a core carries across the RX-IQ cal, in the stock
 * read order: rxgain_config_readback() saves it,
 * rxiqcal_teardown_apply_defaults() restores it (0x0727 before 0x0726 there).
 */
static const u16 b43_phy_ac_rxgain_cfg_regs[25] = {
		0x0720, 0x0721, 0x0722, 0x0723, 0x0724, 0x0725, 0x0726, 0x0727,
		0x0728, 0x0729, 0x0732, 0x0733, 0x0730, 0x0731, 0x0734, 0x0735,
		0x0737, 0x0738, 0x0736, 0x0739, 0x073a, 0x073b, 0x073c, 0x073d,
		0x0747,
};

static u16 b43_phy_ac_rxgain_cfg_saved(const struct b43_phy_ac *ac,
				       unsigned int core, u16 reg)
{
	unsigned int i;

	if (reg == 0x073e)
		return ac->rxgain_cfg_saved[core][25];
	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxgain_cfg_regs); i++)
		if (b43_phy_ac_rxgain_cfg_regs[i] == reg)
			return ac->rxgain_cfg_saved[core][i];
	WARN_ON(1);
	return 0;
}

/*
 * RX gain configuration read-back (94 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   27769-27930]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   22981-23142]
 */
void b43_phy_ac_rxgain_config_readback(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const u16 bbmult_off[B43_PHY_AC_MAX_CORES] = {
		0x0063, 0x0067, 0x006b
	};
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core, i;

	b43_phy_ac_rxcal_040f_save(dev);

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		u16 stride = (u16)(core * 0x200);

		/* The 25 gain registers, in the observed order. */
		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxgain_cfg_regs); i++)
			ac->rxgain_cfg_saved[core][i] =
				b43_phy_read_log(dev, b43_phy_ac_rxgain_cfg_regs[i] + stride);

		/* The bbmult cell, read and discarded. */
		b43_actab_read_log(dev, 0x000c, bbmult_off[core], 1);

		/*
		 * The three gain codes of table 7 at 0x0100 + core + 3 * i,
		 * which rxiqcal_teardown_apply_defaults() writes back.
		 */
		for (i = 0; i < 3; i++)
			b43_actab_read_bulk(dev, 0x0007,
					    (u16)(0x0100 + core + i * 3), 16, 1,
					    &ac->rfseq_gain_saved[core][i]);

		ac->rxgain_cfg_saved[core][25] = b43_phy_read_log(dev, 0x073e + stride);
	}
}

/*
 * RX gain configuration apply (146 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   27932-28084]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23144-23296]
 */
void b43_phy_ac_rxgain_config_apply(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Phase 1: per-core MODs, as core 0's registers (+0x200 per core), in
	 * the capture's order.
	 */
	struct mod_op { u16 reg; u16 mask; u16 val; u16 bw_step; u8 from; };
	static const struct mod_op phase1[52] = {
		{ 0x073e, 0x0010, 0x0000 }, { 0x073e, 0x0020, 0x0000 },
		{ 0x073e, 0x1000, 0x1000 }, { 0x0721, 0x0001, 0x0001 },
		{ 0x0729, 0x0001, 0x0001 }, { 0x073a, 0x0007, 0, .from = B2J_73A_07 },
		{ 0x0725, 0x0020, 0x0020 }, { 0x0739, 0x007e, 0, .from = B2J_739_7E },
		{ 0x0725, 0x0002, 0x0002 }, { 0x073a, 0x0008, 0, .from = B2J_73A_08 },
		{ 0x0725, 0x0040, 0x0040 }, { 0x073a, 0x0010, 0x0010 },
		{ 0x0725, 0x0080, 0x0080 }, { 0x073a, 0x0060, 0, .from = B2J_73A_60 },
		{ 0x0725, 0x0100, 0x0100 }, { 0x0729, 0x0020, 0x0000 },
		{ 0x0721, 0x0020, 0x0020 }, { 0x0729, 0x0040, 0x0000 },
		{ 0x0721, 0x0040, 0x0040 }, { 0x0729, 0x1000, 0x0000 },
		{ 0x0721, 0x1000, 0x1000 }, { 0x0729, 0xe000, 0x0000 },
		{ 0x0721, 0x2000, 0x2000 }, { 0x0728, 0x3800, 0x0000 },
		{ 0x0721, 0x4000, 0x4000 }, { 0x0729, 0x0400, 0x0000 },
		{ 0x0721, 0x0400, 0x0400 }, { 0x0728, 0x0002, 0x0000 },
		{ 0x0720, 0x0002, 0x0002 }, { 0x0729, 0x0020, 0x0020 },
		{ 0x0729, 0x0200, 0x0200 }, { 0x0721, 0x0200, 0x0200 },
		{ 0x0736, 0x0040, 0x0000 }, { 0x0724, 0x0040, 0x0040 },
		{ 0x0736, 0x0100, 0x0000 }, { 0x0724, 0x0100, 0x0100 },
		{ 0x0736, 0x0010, 0x0000 }, { 0x0724, 0x0010, 0x0010 },
		{ 0x0736, 0x0200, 0x0200 }, { 0x0724, 0x0200, 0x0200 },
		{ 0x0736, 0x0020, 0x0020 }, { 0x0724, 0x0020, 0x0020 },
		{ 0x0736, 0x0008, 0x0008 }, { 0x0724, 0x0008, 0x0008 },
		{ 0x0736, 0x0080, 0x0000 }, { 0x0724, 0x0080, 0x0080 },
		{ 0x0736, 0x0004, 0x0000 }, { 0x0724, 0x0004, 0x0004 },
		{ 0x0736, 0x0002, 0x0000 }, { 0x0724, 0x0002, 0x0002 },
		{ 0x0736, 0x0001, 0x0001 }, { 0x0724, 0x0001, 0x0001 },
	};
	/*
	 * Phase 2: 12 MODs after the table read. 0x0735[10:8] and [13:11] are
	 * the bandwidth step, 0/1/2 in their weight, on all 43 d6220 cold
	 * segments. 0x0737[7:0] is the f6 field of the current width's RX-LPF
	 * cell just read back, (cell >> 6) & 0xff, on every d6220 and agcombo
	 * segment.
	 */
	static const struct mod_op phase2[12] = {
		{ 0x0735, 0x0700, 0x0000, .bw_step = 0x0100 }, { 0x0723, 0x0008, 0x0008 },
		{ 0x0735, 0x3800, 0x0000, .bw_step = 0x0800 }, { 0x0723, 0x0010, 0x0010 },
		{ 0x0737, 0x00ff, 0x0000, .from = B2J_LPF }, { 0x0723, 0x0200, 0x0200 },
		{ 0x0735, 0x4000, 0x0000 }, { 0x0723, 0x0020, 0x0020 },
		{ 0x0735, 0x0001, 0x0001 }, { 0x0723, 0x0001, 0x0001 },
		{ 0x0729, 0x0100, 0x0100 }, { 0x0721, 0x0100, 0x0100 },
	};
	const struct b43_phy_ac_rxgain_bw *g = b43_phy_ac_rxgain_bw(dev);
	unsigned int bw = b43_phy_ac_bw_step(dev);
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core, i;
	u16 lpf;

	/* Header: peek and two MODs of 0x0401. */
	b43_phy_read_log(dev, 0x0401);
	b43_phy_maskset(dev, 0x0401, (u16)~0x0007, (u16)ac->coremask);
	b43_phy_maskset(dev, 0x0401, (u16)~0x7000, 0x0000);

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		u16 stride = (u16)(core * 0x200);


		for (i = 0; i < ARRAY_SIZE(phase1); i++)
			b43_phy_maskset(dev, phase1[i].reg + stride,
					(u16)~phase1[i].mask,
					b43_phy_ac_rxgain_field(g, phase1[i].from,
								phase1[i].val));

		b43_actab_read_bulk(dev, 0x0007,
				    b43_phy_ac_rxlpf_lo_cell[bw][core],
				    16, 1, &lpf);

		for (i = 0; i < ARRAY_SIZE(phase2); i++) {
			u16 val = phase2[i].from == B2J_LPF
				? (u16)((lpf >> 6) & 0xff)
				: (u16)(phase2[i].val + phase2[i].bw_step * bw);

			b43_phy_maskset(dev, phase2[i].reg + stride,
					(u16)~phase2[i].mask, val);
		}

		/* Peek and clear bit 0 of 0x?78. */
		b43_phy_read_log(dev, 0x0678 + stride);
		b43_phy_maskset(dev, 0x0678 + stride, (u16)~0x0001, 0);
	}

	/* Trailer: gate unlock. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/* The radio registers radio_iqcal_config() saves and clears. */
static const u16 b43_phy_ac_iqcal_radio_regs[6] = {
	0x0020, 0x0021, 0x0022, 0x0023, 0x003a, 0x003d,
};

/*
 * Radio 2069 IQ-cal configuration, per core (84 ops): save the six
 * registers for radio_iqcal_teardown(), zero them, then ten bit-field MODs
 * in the observed order.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   28085-28200]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23297-23412]
 */
void b43_phy_ac_radio_iqcal_config(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	struct rad_mod_op { u16 reg; u16 mask; u16 val; };
	static const struct rad_mod_op configs[10] = {
		{ 0x0023, 0x0100, 0x0000 },
		{ 0x003d, 0x0010, 0x0000 },
		{ 0x0023, 0x0020, 0x0000 },
		{ 0x0023, 0x0040, 0x0000 },
		{ 0x0021, 0x0020, 0x0000 },
		{ 0x0021, 0x0004, 0x0004 },
		{ 0x0023, 0x0001, 0x0001 },
		{ 0x0023, 0x0200, 0x0200 },
		{ 0x0021, 0x0003, 0x0000 },
		{ 0x0023, 0x0006, 0x0000 },
	};
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core, i;

	for_each_set_bit(core, &ac->coremask, ac->num_cores) {
		u16 stride = (u16)(core * 0x200);

		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_iqcal_radio_regs); i++)
			ac->iqcal_radio_saved[core][i] = b43_radio_read(dev,
				b43_phy_ac_iqcal_radio_regs[i] + stride);

		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_iqcal_radio_regs); i++)
			b43_radio_write(dev,
					b43_phy_ac_iqcal_radio_regs[i] + stride, 0);

		for (i = 0; i < ARRAY_SIZE(configs); i++)
			b43_radio_maskset(dev, configs[i].reg + stride,
					  (u16)~configs[i].mask,
					  configs[i].val);
	}
}

/*
 * Gain control final apply (129 ops). @core_mask is the search round's
 * set, not the board's coremask: the stock driver emits core 2 on a
 * two-chain board too.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   28201-28359, 28579-28731, 28955-29107, 29331-29432]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23413-23571, 23791-23943, 24167-24319, 24543-24644]
 */
void b43_phy_ac_gainctrl_final_apply(struct b43_wldev *dev,
				     bool with_peek_preamble,
				     u8 core_mask,
				     const u16 r734_vals[3])
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int core;

	if (with_peek_preamble) {
		/* Peek 0x?6dc on all three cores. */
		b43_phy_read_log(dev, 0x06dc);
		b43_phy_read_log(dev, 0x08dc);
		b43_phy_read_log(dev, 0x0adc);
	}

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 stride = (u16)(core * 0x200);
		struct b43_phy_ac_gaincurve gc;
		u16 bbmult_val;

		if (!(core_mask & (1u << core)))
			continue;

		/* Gain registers; 0x?734 is the caller's, per core. */
		b43_phy_write(dev, 0x0730 + stride, 0x00b0);
		b43_phy_write(dev, 0x0731 + stride, 0x0004);
		b43_phy_write(dev, 0x0734 + stride, r734_vals[core]);

		/* Set bits 1/2/3 of 0x?722. */
		b43_phy_maskset(dev, 0x0722 + stride, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, 0x0722 + stride, (u16)~0x0004, 0x0004);
		b43_phy_maskset(dev, 0x0722 + stride, (u16)~0x0008, 0x0008);

		/* Peek and lock. */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Gain curve entry 0x0000. */
		b43_phy_ac_read_gaincurve(dev, 0x0000, &gc);
		dev->phy.ac->bbmult_meas = gc.bbmult;
		bbmult_val = gc.bbmult;

		/* Its three gain codes into table 0x0007. */
		b43_actab_write_bulk(dev, 0x0007, (u16)(0x0100 + core),
				     16, 1, &gc.coeff[0]);
		b43_actab_write_bulk(dev, 0x0007, (u16)(0x0103 + core),
				     16, 1, &gc.coeff[1]);
		b43_actab_write_bulk(dev, 0x0007, (u16)(0x0106 + core),
				     16, 1, &gc.coeff[2]);

		/* Sync: peek and idempotent lock. */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* The bbmult into both cells. */
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0063 + core * 4),
				     16, 1, &bbmult_val);
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0073 + core * 4),
				     16, 1, &bbmult_val);

		/* Bridge: idempotent lock, then unlock. */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}
}

/*
 * Blocks A to D of the post-DDS meas_apply, 47 ops, shared by
 * rxiqcal_meas_post_dds_apply() and its _v2 variant (the fifth cycle, whose
 * blocks E to H differ):
 *
 *   A: one self-contained bbmult read per wired chain
 *   B,  2 ops: commit pulse
 *   C, 13 ops: kick sequence variant
 *   D, 18 ops: b43_phy_ac_rxgain_perchan_tail()
 */
static void b43_phy_ac_rxiqcal_meas_readback_kick_tail(struct b43_wldev *dev)
{

	b43_phy_ac_bbmult_peek(dev);

	/* B: commit pulse */
	b43_phy_ac_cca_pulse(dev);

	/* C: kick sequence variant */
	b43_phy_ac_run_samples(dev, b43_phy_ac_rxiqcal_kick_len(dev), false);

	/* D: common tail */
	b43_phy_ac_rxgain_perchan_tail(dev);
}

/*
 * Wait for an RX-IQ measurement to finish. Bit 0 of 0x0270 is the start flag,
 * which the hardware clears on completion, and the stock driver re-reads it
 * while it stays high. The budget is finite, as for the 0x0380 poll, so that
 * hardware in an unexpected state does not block the kernel.
 */
static void b43_phy_ac_rxiqcal_meas_wait(struct b43_wldev *dev)
{
	unsigned int tries;

	for (tries = 0; tries < 1000; tries++) {
		if (!(b43_phy_read(dev, 0x0270) & 0x0001))
			return;
		udelay(1);
	}
	b43err(dev->wl, "AC-PHY: RX-IQ meas busy timeout (0x0270 stuck)\n");
}

/*
 * IQ-cal measurement and apply after the second DDS seed (99 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   28447-28578, 28819-28954, 29195-29330, 29520-29655]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23659-23790, 24031-24166, 24407-24542, 24732-24867]
 */
void b43_phy_ac_rxiqcal_meas_post_dds_apply(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c;

	/* Blocks A to D */
	b43_phy_ac_rxiqcal_meas_readback_kick_tail(dev);

	/* E: set up 0x0272/0x0271/0x0270, arm, wait, final peek */
	b43_phy_write(dev,    0x0272, 0x0400);
	b43_phy_maskset(dev,  0x0271, (u16)~0x00ff, 0x0020);
	b43_phy_maskset(dev,  0x0270, (u16)~0x0002, 0);
	b43_phy_maskset(dev,  0x0270, (u16)~0x0001, 0x0001);
	b43_phy_ac_rxiqcal_meas_wait(dev);
	b43_phy_read_log(dev, 0x0270);

	/*
	 * F, 12 ops: peek the accumulators 0x?c0-0x?c5, in the observed order
	 * 0x?c3, 0x?c2, 0x?c5, 0x?c4, 0x?c1, 0x?c0.
	 */
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores)
		b43_phy_ac_iq_acc_peek(dev, c, false);

	/* G: per-core bbmult apply (variant) */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_set(dev,      B43_PHY_AC_SAMP_PLAY_CTL, B43_PHY_AC_SAMP_PLAY_STOP);
	b43_phy_mask(dev,     B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 lo = (u16)(0x0063 + 4 * c);
		u16 hi = (u16)(0x0073 + 4 * c);
		const u16 *bbmult = &dev->phy.ac->bbmult_meas;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* H: final pulse */
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Gain sub-block of the v2 variant, run before each core's measurement on the
 * other wired chains: four peeks and four MODs per chain in core order, one
 * arm and wait, four writes per chain in reverse core order, and one extra
 * peek. With two chains it is one chain each way; the agcombo and the
 * tg789vac run it on both of the other two.
 */
static void meas_v2_gain_prog_poll(struct b43_wldev *dev, unsigned long others)
{
	unsigned int core, n = 0;
	u8 order[B43_PHY_AC_MAX_CORES];

	for_each_set_bit(core, &others, dev->phy.ac->num_cores) {
		u16 stride = (u16)(core * 0x200);

		order[n++] = (u8)core;

		b43_phy_read_log(dev, 0x0720 + stride);
		b43_phy_read_log(dev, 0x0728 + stride);
		b43_phy_read_log(dev, 0x0721 + stride);
		b43_phy_read_log(dev, 0x0729 + stride);

		b43_phy_maskset(dev, 0x0720 + stride, (u16)~0x0001, 0x0001);
		b43_phy_maskset(dev, 0x0728 + stride, (u16)~0x0001, 0);
		b43_phy_maskset(dev, 0x0721 + stride, (u16)~0x0004, 0x0004);
		b43_phy_maskset(dev, 0x0729 + stride, (u16)~0x0002, 0);
	}

	/* Arm and wait. */
	b43_phy_maskset(dev, 0x0270, (u16)~0x0001, 0x0001);

	b43_phy_ac_rxiqcal_meas_wait(dev);

	/*
	 * Four writes per chain, in reverse core order: 0x?29, 0x?21, 0x?28,
	 * 0x?20.
	 */
	while (n--) {
		u16 stride = (u16)(order[n] * 0x200);

		b43_phy_write(dev, 0x0729 + stride, 0x0321);
		b43_phy_write(dev, 0x0721 + stride, 0x7761);
		b43_phy_write(dev, 0x0728 + stride, 0x0080);
		b43_phy_write(dev, 0x0720 + stride, 0x0182);
	}

	/* One extra peek of 0x0270. */
	b43_phy_read_log(dev, 0x0270);
}

/*
 * Loopback gain search. The mean power per sample is
 * round(ii / 1024) + round(qq / 1024); above PWR_HI the index is halved,
 * and the search stops when the power is at or below it or the index is 0.
 * The index never goes up: over the 323 searches of the d6220 and tg789vac
 * cold sweeps every chain's sequence of 0x?734 writes is non-increasing
 * (4, 2, 1, 0 and its prefixes), so there is no lower edge to the window.
 * Derivation and check: "Structure" in docs/rxiq-cal-analysis.md.
 */
#define B43_PHY_AC_LOOPBACK_PWR_HI	0x169e
#define B43_PHY_AC_LOOPBACK_IDX_5G	4
#define B43_PHY_AC_LOOPBACK_REG		0x0734

/* Mean power per sample from the latest accumulator round. */
static u32 b43_phy_ac_loopback_pwr(const struct b43_phy_ac_iq_acc *acc)
{
	/* rounded to 1024, not truncated */
	return ((acc->ii[0] + 512) >> 10) + ((acc->qq[0] + 512) >> 10);
}

/*
 * Update the index from the measurement. Returns true when the power is inside
 * the window, meaning the search is done and *idx is the value to apply.
 */
static bool b43_phy_ac_loopback_step(struct b43_wldev *dev, unsigned int core,
				     u8 *idx)
{
	u32 pwr;

	if (core >= B43_PHY_AC_MAX_CORES)
		return true;
	pwr = b43_phy_ac_loopback_pwr(&dev->phy.ac->iq_acc[core]);

	if (pwr > B43_PHY_AC_LOOPBACK_PWR_HI && *idx) {
		*idx = (u8)(*idx / 2);
		return false;
	}
	return true;
}

/*
 * Loopback gain search, rounds 6 to 9 of the txpwr apply. Each round writes
 * the current indices through gainctrl_final_apply(), runs a tone and a
 * measurement pass (dds_seed, then meas, which reads the accumulators with
 * iq_acc_peek()), then advances the indices with b43_phy_ac_loopback_step()
 * until every measured core converges. A converged core leaves the round's
 * set, but the measurement still reads every core (ch132/40: round 4 writes
 * only core 1 and still reads core 0).
 *
 * A core the board does not wire is never measured and follows a fixed
 * schedule of {4, 1, 0}: core 2 on the d6220, on every channel and width.
 * Transcribed; the rule behind it is unknown.
 *
 * The starting index is the 5 GHz one; the port refuses 2.4 GHz, where it
 * would start from 0. With a read oracle it reproduces the d6220 ch36/20
 * sequence {4,4,4} {2,2,1} {1,1,0} {0,0}. The round cap is a safety budget;
 * the most observed is 4.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   28201-29655]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23413-24867]
 */
static void b43_phy_ac_loopback_gain_search(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const u16 unwired_sched[3] = { 4, 1, 0 };
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned long searching = ac->coremask;
	unsigned long unwired = GENMASK(ac->num_cores - 1, 0) & ~ac->coremask;
	u16 r734[B43_PHY_AC_MAX_CORES] = { 0 };
	u8 idx[B43_PHY_AC_MAX_CORES];
	unsigned int round, c;

	for (c = 0; c < B43_PHY_AC_MAX_CORES; c++)
		idx[c] = B43_PHY_AC_LOOPBACK_IDX_5G;

	for (round = 0; searching; round++) {
		u8 mask = (u8)searching;

		if (round >= 8) {
			b43err(dev->wl,
			       "AC-PHY: loopback gain search did not converge\n");
			break;
		}

		for_each_set_bit(c, &searching, ac->num_cores)
			r734[c] = idx[c];
		if (round < ARRAY_SIZE(unwired_sched)) {
			mask |= (u8)unwired;
			for_each_set_bit(c, &unwired, ac->num_cores)
				r734[c] = unwired_sched[round];
		}

		b43_phy_ac_gainctrl_final_apply(dev, round == 0, mask, r734);
		b43_phy_ac_rxiqcal_dds_seed_tone(dev, 1);
		b43_phy_ac_rxiqcal_meas_post_dds_apply(dev);

		for_each_set_bit(c, &searching, ac->num_cores)
			if (b43_phy_ac_loopback_step(dev, c, &idx[c]))
				__clear_bit(c, &searching);
	}
}

/*
 * Capture a core's six RX-IQ accumulators into the state: the peeks of
 * 0x?c0-0x?c5 the stock driver emits anyway, in its order (0x?c3, 0x?c2,
 * 0x?c5, 0x?c4, 0x?c1, 0x?c0), with the value kept.
 *
 * @measurement separates the passes that enter the mean from those of the
 * loopback gain search, which read the same registers and must not be
 * averaged; in the captures the search rounds are the only ones preceded by
 * gainctrl_final_apply().
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   28517-28527, 28529-28539, 28893-28903, 28905-28915, 29269-29279,
 *   29281-29291, 29594-29604, 29606-29616, 29853-29863, 29908-29918,
 *   30213-30223, 30332-30342]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23729-23739, 23741-23751, 24105-24115, 24117-24127, 24481-24491,
 *   24493-24503, 24806-24816, 24818-24828, 25125-25135, 25242-25252,
 *   25523-25533, 25614-25624]
 */
static void b43_phy_ac_iq_acc_peek(struct b43_wldev *dev, unsigned int core,
				   bool measurement)
{
	B43_AC_FN();
	struct b43_phy_ac_iq_acc *acc;
	u16 stride = (u16)(core * 0x200);
	u16 ii_hi, ii_lo, qq_hi, qq_lo, iq_hi, iq_lo;
	unsigned int i;
	s64 iq;

	ii_hi = b43_phy_read_log(dev, 0x06c3 + stride);
	ii_lo = b43_phy_read_log(dev, 0x06c2 + stride);
	qq_hi = b43_phy_read_log(dev, 0x06c5 + stride);
	qq_lo = b43_phy_read_log(dev, 0x06c4 + stride);
	iq_hi = b43_phy_read_log(dev, 0x06c1 + stride);
	iq_lo = b43_phy_read_log(dev, 0x06c0 + stride);

	if (core >= B43_PHY_AC_MAX_CORES)
		return;
	acc = &dev->phy.ac->iq_acc[core];

	iq = (s64)(s32)(((u32)iq_hi << 16) | iq_lo);
	if (measurement && !acc->measuring) {
		acc->measuring = true;
		acc->rounds = 0;
		acc->solved = false;
	} else if (!measurement) {
		acc->measuring = false;
	}

	for (i = B43_PHY_AC_IQ_ROUNDS - 1; i > 0; i--) {
		acc->ii[i] = acc->ii[i - 1];
		acc->qq[i] = acc->qq[i - 1];
		acc->iq[i] = acc->iq[i - 1];
	}
	acc->ii[0] = ((u32)ii_hi << 16) | ii_lo;
	acc->qq[0] = ((u32)qq_hi << 16) | qq_lo;
	acc->iq[0] = (s32)iq;
	acc->rounds++;
}

static void meas_v2_peek_c0_c5(struct b43_wldev *dev, unsigned int core)
{
	b43_phy_ac_iq_acc_peek(dev, core, true);
}

/*
 * IQ-cal measurement, v2 variant (143 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   29743-29957, 30045-30381]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   24955-25291, 25379-25663]
 */
void b43_phy_ac_rxiqcal_meas_post_dds_apply_v2(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c;

	/* Blocks A to D */
	b43_phy_ac_rxiqcal_meas_readback_kick_tail(dev);

	/*
	 * Setup: 0x0272 is 0x4000 at 20 and 40 MHz and 0x3000 at 80, on the
	 * d6220, the agcombo and the tg789vac.
	 */
	b43_phy_write(dev,   0x0272,
		      b43_phy_ac_bw_step(dev) == 2 ? 0x3000 : 0x4000);
	b43_phy_maskset(dev, 0x0271, (u16)~0x00ff, 0x0020);
	b43_phy_maskset(dev, 0x0270, (u16)~0x0002, 0);

	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		meas_v2_gain_prog_poll(dev, dev->phy.ac->coremask & ~BIT(c));
		meas_v2_peek_c0_c5(dev, c);
	}

	/* G: per-core bbmult apply, as in v1 */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_set(dev,      B43_PHY_AC_SAMP_PLAY_CTL, B43_PHY_AC_SAMP_PLAY_STOP);
	b43_phy_mask(dev,     B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 lo = (u16)(0x0063 + 4 * c);
		u16 hi = (u16)(0x0073 + 4 * c);
		const u16 *bbmult = &dev->phy.ac->bbmult_meas;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* H: final pulse */
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Solve the RX-IQ coefficients (a, b) of one core from its measurement
 * rounds.
 *
 * Each round is one tone: two at +f and -f up to 40 MHz, six at +-f, +-3f,
 * +-4f at 80. What goes into 0x?a0/0x?a1 is the frequency-independent part,
 * the mean over the tones of the per-tone coefficients; summing the
 * accumulators instead would weight each tone by its power, which at 80 MHz
 * comes out one unit off.
 *
 * Per tone, in Q10:
 *
 *   a_r     = -iq * 2^10 / ii
 *   b_r + 1 = 2^10 * sqrt(qq * ii - iq^2) / ii
 *
 * a is the mean of the a_r, kept in Q24 so that the per-tone rounding does
 * not decide the final unit, each a_r taken on ii and iq cut to a 16-bit
 * mantissa of ii (the same shift on both): that truncation is what moves
 * values just under a half, as the tg789vac's a_r = 20.49 on core 0 at ch36,
 * which the stock driver writes as 21. b is the mean of the b_r computed
 * with the exact a_r under the root, rounded half up.
 *
 * Measured with reverse-tools/rxiq_points.py on 309 writes of 0x?a1 (cold
 * segments of the d6220 and the agcombo, the tg789vac's outside the radar
 * channels, the up segments of the d6220 and agcombo hot sweeps):
 *
 *                       a exact   b exact
 *   accumulator sum      258       253
 *   mean, full width     290       259
 *   this                 296       259
 *
 * The misses on b are mostly the stock driver one high, mostly on core 1:
 * an input outside the six accumulators is missing, hence the
 * b43_phy_ac_todo() at the write site.
 */
static void b43_phy_ac_iq_solve(struct b43_phy_ac_iq_acc *acc,
				s16 *a_out, s16 *b_out)
{
	unsigned int r, nr;
	s64 a_sum = 0, a_den;
	u64 b_sum = 0;
	s32 a, b;

	if (acc->solved) {
		*a_out = acc->a;
		*b_out = acc->b;
		return;
	}
	if (acc->rounds < 2) {
		*a_out = 0;
		*b_out = 0;
		return;
	}

	nr = acc->rounds < B43_PHY_AC_IQ_ROUNDS ? acc->rounds
						: B43_PHY_AC_IQ_ROUNDS;
	for (r = 0; r < nr; r++) {
		u64 ii = acc->ii[r], qq = acc->qq[r];
		s64 iq = acc->iq[r];
		unsigned int k;
		s64 num, den;
		u64 root;

		if (!ii) {
			*a_out = 0;
			*b_out = 0;
			return;
		}

		/*
		 * a_r in Q24, rounded half away from zero, on the two
		 * accumulators cut to a 16-bit mantissa of ii: the same shift
		 * on both, iq arithmetically.
		 */
		k = fls64(ii);
		k = k > 16 ? k - 16 : 0;
		den = (s64)(ii >> k);
		num = -((iq >> k) << 24);
		a_sum += div64_s64(num + (num < 0 ? -(den >> 1) : (den >> 1)),
				   den);

		/*
		 * b_r + 1 in Q10: 2^10 * sqrt(qq*ii - iq^2) / ii, rounded to
		 * nearest. qq*ii - iq^2 >= 0 by Cauchy-Schwarz, and fits in
		 * 64 bits for the 32-bit accumulators the estimator has.
		 */
		root = int_sqrt64(qq * ii - (u64)(iq * iq));
		b_sum += div64_u64((root << 11) + ii, ii << 1);
	}

	/* Mean of the Q24 a_r back to Q10, rounded half away from zero. */
	a_den = (s64)nr << 14;
	a = (s32)div64_s64(a_sum + (a_sum < 0 ? -(a_den >> 1) : (a_den >> 1)),
			   a_den);
	/* Mean of the b_r, rounded half up. */
	b = (s32)div64_u64(b_sum + (nr >> 1), nr) - (1 << 10);

	acc->a = (s16)a;
	acc->b = (s16)b;
	acc->solved = true;
	*a_out = acc->a;
	*b_out = acc->b;
}

/*
 * The eleven-tap bank the stock driver programs per chain at 80 MHz only,
 * right after the RX IQ coefficients: PHY.WR 0x?6a4 appears on every 80 MHz
 * segment that runs the calibration and on no 20 or 40 MHz one.
 *
 * Each row is an antisymmetric kernel around a centre tap of 0x0400 (unity
 * in Q10): the taps at distance k are c/(k + t) with alternating sign,
 * rounded, t about 0.01. It is the frequency-dependent IQ imbalance
 * correction: the term linear in frequency is a derivative, and the
 * derivative kernel is (-1)^k / k. Five rows are observed, c = 0, ~60.5,
 * ~121, ~182 and ~208, the same on three boards and every channel.
 *
 * Which row a chain takes follows its measurement: the slope, over the
 * tones at +-1, +-3, +-4 of the period, of the per-tone a, the part
 * b43_phy_ac_iq_solve() averages away. On the 56 80 MHz points of the
 * captures (d6220, tg789vac and agcombo, cold and hot, bss-up included) the
 * least-squares slope, in units of a per step, separates the five rows:
 *
 *   row          0          A           D           B           C
 *   slope     -1.1..1.2  1.7..3.9    4.2..6.0    6.3..8.5    8.9..9.7
 *
 * The edges between the bins are midpoints of the gaps, not a rule: the
 * bins are not evenly spaced in this slope, so the stock estimator is not
 * exactly this one.
 */
static const s16 b43_phy_ac_bw80_fir[][11] = {
	{ 0, 0, 0, 0, 0, 0x0400, 0, 0, 0, 0, 0 },
	{ 12, -15, 20, -30, 61, 0x0400, -60, 30, -20, 15, -12 },
	{ 24, -30, 41, -61, 122, 0x0400, -120, 60, -40, 30, -24 },
	{ 37, -46, 61, -92, 184, 0x0400, -180, 91, -60, 45, -36 },
	{ 42, -52, 70, -105, 211, 0x0400, -206, 103, -69, 52, -42 },
};

/* Upper edges of the slope bins above, in units of a per step, times 10. */
static const s16 b43_phy_ac_bw80_fir_edge[] = { 15, 40, 61, 87 };

/*
 * The slope of the per-tone a over the measurement tones of @acc, times 10:
 * least squares over the tone steps, the newest round being the last step.
 */
static int b43_phy_ac_iq_slope10(const struct b43_phy_ac_iq_acc *acc,
				 unsigned int n)
{
	s64 num = 0, den = 0;
	unsigned int j;

	for (j = 0; j < n; j++) {
		int k = b43_phy_ac_tone_steps[n - 1 - j];
		s64 ii = acc->ii[j];
		s64 a16;

		if (!ii)
			return 0;
		a16 = div64_s64(-((s64)acc->iq[j] << 26), ii);	/* a in Q16 */
		num += k * a16;
		den += k * k;
	}
	if (!den)
		return 0;
	/* Q16 per step to tenths per step. */
	return (int)div64_s64(num * 10, den << 16);
}

static void b43_phy_ac_bw80_fir_write(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int n = b43_phy_ac_meas_passes(dev);
	unsigned int c, i;

	if (ac->cal_width != NL80211_CHAN_WIDTH_80)
		return;

	for_each_set_bit(c, &ac->coremask, ac->num_cores) {
		u16 stride = (u16)(c * 0x200);
		const struct b43_phy_ac_iq_acc *acc = &ac->iq_acc[c];
		int s = acc->rounds >= n ? b43_phy_ac_iq_slope10(acc, n) : 0;
		unsigned int row = 0;

		while (row < ARRAY_SIZE(b43_phy_ac_bw80_fir_edge) &&
		       s >= b43_phy_ac_bw80_fir_edge[row])
			row++;

		b43_phy_maskset(dev, 0x0210, (u16)~0x0007, 0x0004);
		b43_phy_maskset(dev, 0x0211, (u16)~0x0004, 0);
		b43_phy_maskset(dev, 0x0211, (u16)~0x0002, 0);
		b43_phy_maskset(dev, 0x0212, (u16)~0x000f, 0x000a);
		for (i = 0; i < 11; i++)
			b43_phy_write(dev, (u16)(0x06a4 + stride + i),
				      (u16)b43_phy_ac_bw80_fir[row][i]);
	}
	b43_phy_maskset(dev, 0x0211, (u16)~0x0001, 0x0001);
}

/*
 * Per-chain write of the RX-IQ correction coefficients, then the 80 MHz
 * FIR bank. The coefficients are computed from the accumulators the
 * measurement gathered; on the d6220 attach-to-bss-up capture core 0 gives
 * a = -17, b = 77 and core 1 a = -44, b = 59, what the stock driver writes.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   30382-30385]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25664-25667]
 */
void b43_phy_ac_rxiqcal_apply_coefficients(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c;

	b43_phy_ac_todo(dev,
		"RX IQ coefficient b may be one LSB off the stock "
		"driver (about 1 point in 6, mostly core 1): image "
		"rejection may be slightly worse\n");

	/* Per core: 0x?a0 (coefficient a) and 0x?a1 (coefficient b) */
	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 stride = (u16)(c * 0x200);
		s16 a, b;

		b43_phy_ac_iq_solve(&dev->phy.ac->iq_acc[c], &a, &b);
		b43_phy_write(dev, 0x06a0 + stride, (u16)(a & 0x03ff));
		b43_phy_write(dev, 0x06a1 + stride, (u16)(b & 0x03ff));
	}

	b43_phy_ac_bw80_fir_write(dev);
}

/*
 * Radio 2069 IQ-cal teardown (12 ops): restore what radio_iqcal_config()
 * saved.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   30386-30397]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25668-25679]
 */
void b43_phy_ac_radio_iqcal_teardown(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c, i;

	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 stride = (u16)(c * 0x200);

		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_iqcal_radio_regs); i++)
			b43_radio_write(dev,
					b43_phy_ac_iqcal_radio_regs[i] + stride,
					dev->phy.ac->iqcal_radio_saved[c][i]);
	}
}

/*
 * RX-IQ cal teardown and defaults (185 ops): per wired chain, the saved
 * gain codes written back around a read of the gain curve at 0x40 and its
 * bbmult written twice, then the gain registers restored.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   30398-30613]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25680-25895]
 */
void b43_phy_ac_rxiqcal_teardown_apply_defaults(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	u16 bbmult_val;

	/*
	 * The gain registers restored, 27 per core in the stock driver's order,
	 * which is not by address (0x0727 before 0x0726, 0x0737 before 0x0736),
	 * as core 0's (+0x200 per core). 0x?678 gets 0x0008, the others the
	 * value rxgain_config_readback() saved.
	 */
	static const u16 reset_regs[27] = {
		0x073e, 0x0678, 0x0720, 0x0721, 0x0722, 0x0723, 0x0724, 0x0725,
		0x0727, 0x0726, 0x0728, 0x0729, 0x0732, 0x0733, 0x0730, 0x0731,
		0x0734, 0x0735, 0x0737, 0x0738, 0x0736, 0x0739, 0x073a, 0x073b,
		0x073c, 0x073d, 0x0747,
	};
	unsigned int c, i;

	/* Global preamble */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_write(dev,   0x0401, b43_phy_ac_rfseq_mode_all(dev));

	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
		u16 stride = (u16)(c * 0x200);
		struct b43_phy_ac_gaincurve gc;

		/* The three saved gain codes */
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 0x0007,
					     (u16)(0x0100 + c + i * 3),
					     16, 1, &dev->phy.ac->rfseq_gain_saved[c][i]);

		/* Sync */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Gain curve entry 0x0040 */
		b43_phy_ac_read_gaincurve(dev, 0x0040, &gc);
		bbmult_val = gc.bbmult;

		/* The three gain codes again, same values. */
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 0x0007,
					     (u16)(0x0100 + c + i * 3),
					     16, 1, &dev->phy.ac->rfseq_gain_saved[c][i]);

		/* Sync */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* The bbmult into both cells */
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0063 + c * 4), 16, 1, &bbmult_val);
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0073 + c * 4), 16, 1, &bbmult_val);

		/*
		 * Bridge: two idempotent lock MODs, a peek, a lock MOD. Purpose
		 * not known.
		 */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* The bbmult cells again */
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0063 + c * 4), 16, 1, &bbmult_val);
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0073 + c * 4), 16, 1, &bbmult_val);

		/* Trailer */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		for (i = 0; i < ARRAY_SIZE(reset_regs); i++)
			b43_phy_write(dev,
				      (u16)(reset_regs[i] + stride),
				      reset_regs[i] == 0x0678 ? 0x0008 :
				      b43_phy_ac_rxgain_cfg_saved(dev->phy.ac, c,
								  reset_regs[i]));
	}
}

/*
 * The registers of b43_phy_ac_probe_peek(). Reading the peeks and the mode
 * change on 0x0520[3:2] (probe_mode_next()) as a probe is an
 * interpretation; what they are for (a calibration measurement, an EVM
 * check) is not documented.
 */
static const u16 b43_phy_ac_probe_chain_regs[B43_PHY_AC_MAX_CORES][4] = {
	{ 0x07af, 0x07b3, 0x07ab, 0x07b1 },
	{ 0x09af, 0x09b3, 0x09ab, 0x09b1 },
	{ 0x0baf, 0x0bab, 0x0bb3, 0x0bb1 },
};

static const u16 b43_phy_ac_probe_tail_regs[4] = {
	0x0523, 0x0529, 0x0528, 0x0527,
};

/*
 * The peeks at the head of every probe iteration: four per wired chain,
 * then the four shared ones. Core 2 reads its four in a different order
 * (agcombo and tg789vac alike); the d6220 does not wire it.
 */
static void b43_phy_ac_probe_peek(struct b43_wldev *dev)
{
	unsigned int c, k;

	for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores)
		for (k = 0; k < ARRAY_SIZE(b43_phy_ac_probe_chain_regs[0]); k++)
			b43_phy_read_log(dev, b43_phy_ac_probe_chain_regs[c][k]);
	for (k = 0; k < ARRAY_SIZE(b43_phy_ac_probe_tail_regs); k++)
		b43_phy_read_log(dev, b43_phy_ac_probe_tail_regs[k]);
}

/*
 * Return the current 0x0520[3:2] and step it to the next wired chain: the
 * field selects a chain, walked in turn (0, 1 on the d6220, 0, 1, 2 on the
 * tg789vac). It lives in ac->probe_mode because the stock driver never
 * resets it: the walk runs on across probe blocks.
 */
static u16 probe_mode_next(struct b43_phy_ac *ac)
{
	u16 cur = ac->probe_mode;
	unsigned int c = find_next_bit(&ac->coremask, ac->num_cores,
				       (cur >> 2) + 1);

	if (c >= ac->num_cores)
		c = find_next_bit(&ac->coremask, ac->num_cores, 0);
	ac->probe_mode = (u16)(c << 2);
	return cur;
}

/*
 * The stock driver's read of a 32-bit SHM counter: high half, low half, high
 * half again. Nothing here consumes the counters; a consumer would have to
 * re-read the low half when the two high halves differ.
 */
static void b43_phy_ac_wd_shm_peek32(struct b43_wldev *dev, u16 lo_off)
{
	b43_shm_read16(dev, B43_SHM_SHARED, (u16)(lo_off + 2));
	b43_shm_read16(dev, B43_SHM_SHARED, lo_off);
	b43_shm_read16(dev, B43_SHM_SHARED, (u16)(lo_off + 2));
}

/*
 * Zero the six-word ucode statistics window, the clear half of the
 * latch-and-clear: b43_phy_ac_wd_stats_tail() reads the window, this zeroes
 * it, and without it the counters saturate. The latch reads up to 0x0314,
 * the clear stops at 0x0312, as in every capture.
 *
 * Two callers in different phases: the watchdog tick, after the TSSI peek,
 * and post_cal_finalize_iter3(), after the RX suspend.
 */
static void b43_phy_ac_wd_stats_clear(struct b43_wldev *dev)
{
	u16 off;

	for (off = 0x0308; off <= 0x0312; off += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, off, 0);
}

/*
 * Ask the microcode for a noise sample; it answers with
 * B43_IRQ_NOISESAMPLE_OK and the core calls b43_phy_ac_noise_sample_done().
 * The vendor writes the command bit alone, without reading MACCMD.
 */
static void b43_phy_ac_noise_sample_request(struct b43_wldev *dev)
{
	b43_write32(dev, B43_MMIO_MACCMD, B43_MACCMD_BGNOISE);
	dev->phy.ac->noise_pending = true;
}

/*
 * The phase peeks and, with @arm_tone, the tone generator pair.
 *
 * Separate from b43_phy_ac_wd_sample_phase_opt() because on entering the
 * probe phase the capture puts them between the four scattered cells of the
 * sweep and the mode change, while in steady state they come after both;
 * see docs/retrace-todo.md.
 */
static void b43_phy_ac_wd_peek(struct b43_wldev *dev, bool arm_tone)
{
	b43_mac_suspend(dev);
	b43_phy_ac_probe_peek(dev);
	if (arm_tone) {
		b43_phy_write(dev, 0x0554, 0x0bb8);
		b43_phy_write(dev, 0x0555, 0x0bb8);
	}
	b43_mac_enable(dev);
}

/* The mode change that closes the turn, separate for the same reason. */
static void b43_phy_ac_wd_mode_next(struct b43_wldev *dev)
{
	b43_mac_suspend(dev);
	b43_phy_maskset(dev, 0x0520, (u16)~0x000c,
			probe_mode_next(dev->phy.ac));
	b43_mac_enable(dev);
}

/*
 * Sampling phase: the phase peeks with the MAC suspended, the statistics
 * clear and the next 0x0520[3:2] mode, the same continuous walk as the probe
 * cycle (verified over 21 consecutive instances).
 *
 * The peek and the tone pair are optional because the probe-phase ticks
 * differ. On cold01 the twenty ticks are:
 *
 *   tick 0        poll, peek, 0x0554/0x0555, clear, mode
 *   ticks 1, 2    poll, latch, clear, mode              <- no peek
 *   ticks 3..18   poll, latch, peek, clear, mode        <- full shape
 *   tick 9        as above, with the tempsense after the poll
 *   tick 19       poll, tempsense, end of the phase
 *
 * The steady-state tick is the full shape.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   31063-31099, 31327-31335, 31496-31504, 31665-31699, 31860-31894,
 *   32055-32089, 32250-32284, 32445-32479, 32640-32674, 33397-33431,
 *   33592-33626, 33787-33821, 33982-34016, 34177-34211, 34372-34406,
 *   34567-34601, 34762-34796, 35024-35058, 35286-35320]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   26290-26473, 26660-26668, 26835-27070, 27793-27827, 27988-28022,
 *   28183-28217, 28384-28418]
 */
static void b43_phy_ac_wd_sample_phase_opt(struct b43_wldev *dev, bool peek,
					   bool arm_tone)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	B43_AC_FN();

	if (peek)
		b43_phy_ac_wd_peek(dev, arm_tone);

	/*
	 * The window clear arms the next sample (in brcmsmac the four
	 * M_PWRIND_MAP are cleared and MCMD_BG_NOISE issued together), so it
	 * is not done over a sample in flight, as brcmsmac's
	 * sampling_in_progress does.
	 */
	if (!ac->noise_pending) {
		b43_phy_ac_wd_stats_clear(dev);
		b43_phy_ac_noise_sample_request(dev);
	}
	b43_phy_ac_wd_mode_next(dev);
}

/*
 * SHM statistics poll, with the MAC active, in the reference tick's order:
 * four scattered words, the 0x0768-0x078a sweep, two hi/lo/hi passes over
 * the six 32-bit counters, the counters at 0x07e0, 0x07e4 and 0x07dc, the
 * 0x07d6-0x07da group and two trailing words. b43 has no use for the
 * counters: they are read for the stock driver's bus order.
 *
 * The three shapes the captures show:
 *
 *   @head_sweep, @ctr32_passes   where
 *   true, 2                      the full shape
 *   true, 0                      sweep only: the three polls of the up path
 *   false, 1                     second half, one pass: the MAC config block
 *
 * Plausibly the passes are missing where the MAC has not counted anything
 * yet; unproven.
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   12894-12956, 12961-13003, 13481-13523, 13540-13582, 30919-31061,
 *   31100-31309, 31336-31478, 31505-31647, 31700-31842, 31895-32037,
 *   32090-32232, 32285-32427, 32480-32622, 32675-32817, 33432-33574,
 *   33627-33769, 33822-33964, 34017-34159, 34212-34354, 34407-34549,
 *   34602-34744, 34797-35006, 35059-35268, 35321-35463]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8581-8643, 8649-8691, 9173-9215, 9229-9271, 26124-26212, 26214-26272,
 *   26474-26616, 26669-26757, 26759-26817, 27071-27213, 27828-27970,
 *   28023-28165, 28218-28306, 28308-28366, 28419-28567, 28587-28591,
 *   29136-29140]
 */
static void b43_phy_ac_wd_stats_poll_opt(struct b43_wldev *dev,
					 bool head_sweep,
					 unsigned int ctr32_passes,
					 bool ctr32_tail)
{
	B43_AC_FN();
	static const u16 ctr32[6] = {
		0x0768, 0x076c, 0x0770, 0x0774, 0x0778, 0x077c
	};
	unsigned int i, pass;
	u16 off;

	if (head_sweep) {
		b43_phy_ac_wd_head_words(dev, false);
		b43_phy_ac_wd_flat_sweep(dev);
	}

	if (!ctr32_passes)
		return;

	for (pass = 0; pass < ctr32_passes; pass++) {
		if (pass == 1)
			B43_AC_CORE_SITE(dev, BEACON_WD);
		for (i = 0; i < ARRAY_SIZE(ctr32); i++)
			b43_phy_ac_wd_shm_peek32(dev, ctr32[i]);
	}

	/*
	 * @ctr32_tail closes the poll with the three counters outside the list.
	 * A beacon reload can fall between the two passes, so the caller may
	 * split the poll and put the tail on the second half only.
	 */
	if (!ctr32_tail)
		return;

	b43_phy_ac_wd_shm_peek32(dev, 0x07e0);
	b43_phy_ac_wd_shm_peek32(dev, 0x07e4);
	b43_phy_ac_wd_shm_peek32(dev, 0x07dc);

	for (off = 0x07d6; off <= 0x07da; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
	b43_shm_read16(dev, B43_SHM_SHARED, 0x015a);
	b43_shm_read16(dev, B43_SHM_SHARED, 0x014e);
}

/* Closing re-read of the window zeroed at the head, plus 0x008c. */
static void b43_phy_ac_wd_stats_tail(struct b43_wldev *dev)
{
	u16 noise[B43_PHY_AC_MAX_CORES] = { 0 };
	u16 off;

	b43_shm_read16(dev, B43_SHM_SHARED, 0x008c);
	for (off = 0x0308; off <= 0x0314; off += 2) {
		u16 v = b43_shm_read16(dev, B43_SHM_SHARED, off);
		unsigned int core = (off - 0x0308) / 4;

		/*
		 * One 32-bit cell per chain from 0x0308: the noise statistic
		 * the CRS thresholds follow; see b43_phy_ac_crs_note_noise().
		 * The high words and 0x0314 are read for the latch-and-clear
		 * only.
		 */
		if (!(off & 2) && core < B43_PHY_AC_MAX_CORES)
			noise[core] = v;
	}
	b43_phy_ac_crs_note_noise(dev, noise);
}

static void b43_phy_ac_wd_stats_poll(struct b43_wldev *dev)
{
	b43_phy_ac_wd_stats_poll_opt(dev, true, 2, true);
}

/*
 * The second half of a watchdog turn: statistics poll, optional
 * tempsense (@tempsense, MAC suspended), latch of the SHM window (@tail).
 */
static void b43_phy_ac_wd_body(struct b43_wldev *dev, bool tempsense,
			       bool tail)
{
	b43_phy_ac_wd_stats_poll(dev);

	if (tempsense) {
		b43_mac_suspend(dev);
		b43_phy_ac_tempsense(dev);
		b43_mac_enable(dev);
	}

	if (tail)
		b43_phy_ac_wd_stats_tail(dev);
}

/*
 * Watchdog turns between two temperature readings: the SROM's
 * temps_period, or 10 when unprogrammed (the four-bit field reads 0xf; 0
 * is no programmed value either). The d6220, the agcombo and the DSL-3580L
 * leave it unprogrammed and measure every 10 s; the tg789vac programs 5.
 */
static unsigned int b43_phy_ac_temps_period(struct b43_wldev *dev)
{
	u8 p = dev->dev->bus_sprom->temps_period;

	return (p == 0 || p == 0xf) ? 10 : p;
}

/*
 * The region 0x00e0-0x015e read whole, one turn in thirty.
 *
 * The capture shows an `OBJ.BULKR len=128` and the 64 words, the tracer's
 * mark of a stock region read; b43 has no region accessor on shared memory,
 * so these are 64 single reads.
 *
 * Whether this runs on every channel or only on radar channels is not
 * known: the segments that reach thirty turns are exactly the radar ones.
 * Every channel is the simpler assumption.
 */
static void b43_phy_ac_wd_region_dump(struct b43_wldev *dev)
{
	u16 off;

	B43_AC_FN();
	for (off = 0x00e0; off <= 0x015e; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
}

/*
 * One watchdog turn, in the stock driver's order: the sampling phase (peek
 * group, window clear, mode change), then the body (statistics, tempsense
 * where due, latch).
 *
 * In steady state the stock driver polls TSSI and the SHM statistics about
 * once a second and runs a tempsense every temps_period turns; b43 calls
 * this from its pwork hook (b43_phy_ac_op_pwork_1sec()). The poll is also
 * the latch-and-clear of the ucode statistics: 0x0308 is the noise sample
 * the CRS thresholds follow, the rest is cleared so that it does not
 * saturate. Reference tick:
 * router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt.
 *
 * @wd_turns counts from the bring-up and survives channel changes: the
 * tempsense falls every b43_phy_ac_temps_period() turns and the region
 * dump every thirty, and on hot cycles the phase starts anywhere in the
 * period, as for a free-running timer. @wd_switch_turns counts from the
 * channel change and gives the shape of the first turns:
 *
 *   turn 0        statistics only, no latch: the window was cleared at
 *                 the end of the switch and has not counted
 *   turn 1        peek, 0x0554/0x0555, clear, mode, statistics, latch,
 *                 then CRS block E where the availability check is
 *                 pending (otherwise at the end of the bring-up)
 *   turns 2, 3    clear, mode, statistics, latch        <- no peek
 *   turns 4..     full shape
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   29136-29140]
 */
void b43_phy_ac_watchdog(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int k = ac->wd_switch_turns;
	bool half_turn = false;

	B43_AC_FN();

	if (k == 0 && ac->wd_entry_turn) {
		/*
		 * The entry turn has the full shape: the four scattered cells,
		 * the peek with the tone, the mode change, then the whole
		 * sweep; the sample armed at the end of the bring-up has not
		 * arrived and the window has counted. See b43_phy_ac_wd_peek().
		 */
		b43_phy_ac_wd_head_words(dev, true);
		b43_phy_ac_wd_peek(dev, true);
		/*
		 * The entry turn did the tone peek that turn 1 does in steady
		 * state: the next turn is 2.
		 */
		ac->wd_switch_turns = 1;
		/*
		 * After the entry turn there are three turns without a peek,
		 * not two: the stock driver resumes peeking one turn later.
		 * Extending the k rule to k == 4 would also hit cycles without
		 * an entry turn.
		 */
		ac->peek_skip_one = true;
		/*
		 * This is half of a stock callback: the mode change and the
		 * body come together with the next event, so the turn counter
		 * does not advance, or it would run one turn ahead of the stock
		 * driver's.
		 */
		half_turn = true;
	} else if (k == 0) {
		b43_phy_ac_wd_stats_poll(dev);
	} else {
		bool peek = k != 2 && k != 3;

		if (peek && ac->peek_skip_one) {
			peek = false;
			ac->peek_skip_one = false;
		}
		b43_phy_ac_wd_sample_phase_opt(dev, peek, k == 1);

		/*
		 * On the 43 cold segments the stock driver emits it at turns
		 * 29, 59, 89 and 119 of the phase, and never on the 22 with
		 * fewer than 30 turns. On the 44 up segments the period is the
		 * same and the phase is not, since the counter survives channel
		 * changes.
		 */
		if (ac->wd_turns % 30 == 29)
			b43_phy_ac_wd_region_dump(dev);

		b43_phy_ac_wd_body(dev,
				   ac->wd_turns % b43_phy_ac_temps_period(dev) ==
				   b43_phy_ac_temps_period(dev) - 1u,
				   false);
	}

	if (!half_turn)
		ac->wd_turns++;
	if (ac->wd_switch_turns < 0xffff)
		ac->wd_switch_turns++;
}

static void b43_phy_ac_op_pwork_1sec(struct b43_wldev *dev)
{
	u16 sm = dev->phy.ac->status_mask;
	static const u16 want = B43_PHY_AC_STATE_RX_WAITED |
				B43_PHY_AC_STATE_RX_OFDM;
	static const u16 forbid = B43_PHY_AC_STATE_RX_CCK |
				  B43_PHY_AC_STATE_CLIP_ALL_DIS |
				  B43_PHY_AC_STATE_CCA_RESET;

	/*
	 * The first round of the periodic work runs with zero delay from
	 * b43_periodic_tasks_setup(), before switch_channel() has released the
	 * PHY. The stock driver runs the tick only in its run window, and
	 * outside that state the skip must be silent: reaching the tempsense's
	 * REQUIRE would set FAULTED, which is sticky and disables every gated
	 * function. MAC_EN is not forbidden: the watchdog suspends the MAC
	 * itself.
	 */
	if (sm & B43_PHY_AC_STATE_FAULTED)
		return;
	if ((sm & want) != want || (sm & forbid))
		return;

	B43_AC_BLOCK(dev, "watchdog");
	b43_phy_ac_watchdog(dev);
}

/*
 * RX-IQ cal finalize (about 2700 ops).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   30614-36041]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25896-28591]
 */
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * Block D saves the LO DAC and the TX IQ/LO coefficients into @lo_dac
	 * and @txiqlo_coef; b43_phy_ac_down() writes them back.
	 */
	u16 (*lo_dac)[4] = dev->phy.ac->lo_dac;
	u16 (*txiqlo_coef)[3] = dev->phy.ac->txiqlo_coef;
	/*
	 * Called with the MAC suspended, after rxiqcal_teardown_apply_defaults(),
	 * in the canonical calibration state: classifier in RX_WAITED, clip
	 * detect disabled on every core.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Block A, 10 ops: 0x040f as rxgain_config_readback() found it, the
	 * RST2RX kick through force_rf_sequence() with the override gate, then
	 * the unlock closing the scope.
	 */
	b43_phy_write(dev, 0x040f, dev->phy.ac->rxcal_040f_saved);
	b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RST2RX);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

	/*
	 * Block B: one self-contained two-word write to table 0x000c per wired
	 * chain, reapplying the AFE cal's pass-1 results (the afe_res_cal
	 * snapshot of the 0x?056 iterations) at 0x60 + 4 * core.
	 */
	{
		unsigned int c;

		for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores)
			b43_actab_write_bulk_scoped(dev, 0x000c,
						    (u16)(0x0060 + 4 * c), 16, 2,
						    dev->phy.ac->afe_res_cal[2 * c].v);
	}

	/*
	 * Block C, 10 ops: the final post-cal RX release and a gate open,
	 * through the helpers so that status_mask follows the hardware.
	 */
	b43_phy_ac_classctl_write_peeked(dev, false);
	b43_phy_ac_adc_hold(dev, true);
	b43_phy_ac_clip_det(dev, true);
	b43_phy_write(dev, 0x0339, 0x0fff);

	/*
	 * Block D, 49 ops: a table write at 0x5f, then per core the save of the
	 * TX IQ/LO coefficients, four radio reads and the peeks of 0x?a0/0x?a1,
	 * the core's RX-IQ corrector state.
	 */
	{
		static const u16 tbl_5f_val = 0xacdc;
		unsigned int c;

		b43_actab_write_bulk_scoped(dev, 0x000c, 0x005f, 16, 1,
					    &tbl_5f_val);

		for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
			u16 stride = (u16)(c * 0x200);

			/* Coefficients at 0x60 + 4 * c, two words */
			b43_actab_read_bulk(dev, 0x000c,
					    (u16)(0x0060 + c * 4),
					    16, 2, &txiqlo_coef[c][0]);
			/* explicit unlock, closing actab_read_bulk()'s scope */
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
			/* and at 0x62 + 4 * c, one word */
			b43_actab_read_bulk(dev, 0x000c,
					    (u16)(0x0062 + c * 4),
					    16, 1, &txiqlo_coef[c][2]);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

			/*
			 * Radio 0x0002-0x0005 per core, the LO DAC leakage,
			 * kept because the per-core tail rewrites them: they
			 * are cal state, different between the warm and attach
			 * paths.
			 */
			lo_dac[c][0] = b43_radio_read(dev, 0x0002 + stride);
			lo_dac[c][1] = b43_radio_read(dev, 0x0003 + stride);
			lo_dac[c][2] = b43_radio_read(dev, 0x0004 + stride);
			lo_dac[c][3] = b43_radio_read(dev, 0x0005 + stride);

			/* Peek the RX-IQ correction coefficients. */
			b43_phy_read_log(dev, 0x06a0 + stride);
			b43_phy_read_log(dev, 0x06a1 + stride);
		}

		/* b43_phy_ac_down() has something to write back. */
		dev->phy.ac->iqlo_saved = true;
	}

	/*
	 * Block E, 28 ops: AFE, CRS and noise-floor reconfiguration: the AFE
	 * gain registers; a MAC toggle, MHF and GPIO sequence of the MAC/GPIO
	 * layer; the eight CRS registers at 0x34; the 0x0910-0x0913 bank; a
	 * closing MAC.MCTRL toggle.
	 */
	b43_phy_ac_afe_gain_regs_reemit(dev);

	/*
	 * The enable that closes the calibration: blocks A to E run with the
	 * MAC suspended, and what follows does not. It belongs here and not to
	 * the tail below, which on a channel where the calibration did not run
	 * is entered with the MAC already enabled by op_channel_calibrate().
	 */
	b43_mac_enable(dev);
}

/*
 * The radar pulse detection threshold, field 13:8 of PHY 0x02e4.
 *
 * Between the two driver versions the captures cover, it is the only
 * detector register with a different value, and the pulse FIFOs follow it:
 * wl 6.30 on the DSL-3580L writes 0x08 and the FIFOs fill continuously up
 * to saturation, while wl 7.14 on the d6220 writes 0x0f and on about 22700
 * reads of both sweeps they are always empty. 7.14's value is used on every
 * board. The unit is dB-related; two values do not tell the step.
 */
static void b43_phy_ac_radar_thresh(struct b43_wldev *dev)
{
	b43_phy_maskset(dev, 0x02e4, (u16)~0x3f00, 0x0f00);
}

/*
 * The channel availability check.
 *
 * On a channel with the radar duty a first bring-up may not transmit until
 * the check completes. Hence the calibrations that transmit do not run
 * (b43_phy_ac_may_calibrate_tx()), and the check runs: an arm and a
 * detector poll.
 *
 * The arm is one maskset on PHY 0x02e4, right after the host-flag clear
 * that opens the post-bring-up tail, with the BSS match row suspended and a
 * first poll turn. A poll turn is mac_suspend, reads of PHY 0x0251 and
 * 0x0252, mac_enable, the callback of a 150 ms timer: on cold05 680 of the
 * 705 gaps are 151 ms, the rest 1.3-1.4 s where the tempsense holds the
 * thread. The timer outlives the check (305 of 706 turns fall after it),
 * so its condition is the duty, not the calibration gate.
 *
 * 0x0251 and 0x0252 are the fill levels of the two radar pulse FIFOs, in
 * 16-bit words, and 0x0253 and 0x0254 their data ports: on the DSL-3580L,
 * with its lower threshold, the levels are multiples of four (four words a
 * pulse), saturate at 0x05fc, and the words drained match them. With this
 * driver's threshold the FIFOs stay empty on every capture, so a pulse is
 * an event, and the poll reports it. The pulse words are not decoded yet:
 * they are drained and ignored.
 */
#define B43_PHY_AC_RADAR_FIFO_WORDS	0x0600

static void b43_phy_ac_radar_fifo_drain(struct b43_wldev *dev, u16 data,
					u16 level)
{
	u16 i;

	for (i = 0; i < min_t(u16, level, B43_PHY_AC_RADAR_FIFO_WORDS); i++)
		b43_phy_read(dev, data);
}

static void b43_phy_ac_cac_poll(struct b43_wldev *dev, unsigned int turns)
{
	unsigned int i;

	/*
	 * The duty alone, not the calibration gate: the poll is in-service
	 * monitoring and outlives the check (cold05: detector read 706 times
	 * from +10.7 s to +147.5 s, 305 of them after the check settles at
	 * +73.6 s).
	 */
	if (!b43_phy_ac_chan_has_radar_duty(dev))
		return;

	B43_AC_FN();
	for (i = 0; i < turns; i++) {
		u16 level0, level1;

		b43_mac_suspend(dev);
		level0 = b43_phy_read_log(dev, 0x0251);
		level1 = b43_phy_read_log(dev, 0x0252);
		b43_phy_ac_radar_fifo_drain(dev, 0x0253, level0);
		b43_phy_ac_radar_fifo_drain(dev, 0x0254, level1);
		b43_mac_enable(dev);
		if (level0 || level1)
			dev->phy.ac->radar_pulses = true;
	}
}

bool b43_phy_ac_radar_poll(struct b43_wldev *dev)
{
	bool seen;

	b43_phy_ac_cac_poll(dev, 1);
	seen = dev->phy.ac->radar_pulses;
	dev->phy.ac->radar_pulses = false;
	return seen;
}

/* Arm the check, and the turn that goes with it. */
static void b43_phy_ac_cac_arm(struct b43_wldev *dev)
{
	if (b43_phy_ac_may_calibrate_tx(dev))
		return;

	B43_AC_FN();
	b43_phy_ac_radar_thresh(dev);
	B43_AC_CORE_SITE(dev, CAC_CLOSE);
	b43_phy_ac_cac_poll(dev, 1);
}

/*
 * Block E: the CRS thresholds and the 0x0910 bank, which depend on the
 * sample just latched.
 */
static void b43_phy_ac_crs_block_e(struct b43_wldev *dev)
{
	b43_mac_suspend(dev);

	/*
	 * The eight CRS registers under mask 0x00ff (see b43_phy_ac_crs_regs
	 * for the order), then the per-chain bank. Both come from the noise
	 * rings, whose newest sample was latched just before.
	 */
	b43_phy_ac_crs_regs_write(dev,
				  b43_phy_ac_crs_min_pwr(dev,
							 b43_phy_ac_crs_index(dev, 0),
							 true));
	b43_phy_ac_prog_bank_0910(dev);
	b43_phy_ac_crs_note_prog(dev, b43_phy_ac_crs_index(dev, 0));

	/*
	 * The MAC goes back up and stays up: what follows (the tick's
	 * statistics poll, or CAC turns) runs with the MAC active, each user
	 * suspending it itself. cold01 has a single enable here.
	 */
	b43_mac_enable(dev);
}

/*
 * Completion of the noise sample: the statistics window is read here, and
 * CRS block E follows if the bring-up tail armed it or the index moved.
 * Not the tail of another PHY function but an entry point the core calls
 * from its tasklet, as brcmsmac's brcms_c_dpc() on MI_BG_NOISE.
 *
 * The captures show it is a separate context: of the op classes with at
 * least twenty occurrences, the only ones on a single CPU are the eight
 * reads of this block (0x008c and the 0x0308-0x0314 window, 2665 times
 * each, all on cpu1), while everything around them spreads over both; the
 * last poll read before the latch is on cpu0 1298 times, the latch one op
 * later on cpu1 every time.
 *
 * That the sample is the noise sample is a hypothesis: brcmsmac has no AC
 * PHY, and there the callback does not touch the CRS thresholds.
 */
void b43_phy_ac_noise_sample_done(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;

	B43_AC_FN();
	b43_phy_ac_wd_stats_tail(dev);

	if (ac->crs_update_pending || b43_phy_ac_crs_moved(dev)) {
		ac->crs_update_pending = false;
		b43_phy_ac_crs_block_e(dev);
	}

	ac->noise_pending = false;
}

/*
 * Everything the driver emits once the bring-up is over: the host-flag
 * clear that opens it, the CAC arm, the noise-sample arm, then events (the
 * watchdog turns, the radar polls, the bss-up, the core's beacon reloads).
 *
 * Separate from b43_phy_ac_rxiqcal_finalize() because this runs where
 * b43_phy_ac_may_calibrate_tx() says no and the calibration does not: on
 * cold05 the stock driver goes straight from op_channel_calibrate()'s
 * mac_enable to the host-flag clear.
 */
static void b43_phy_ac_post_bringup_tail(struct b43_wldev *dev)
{
	B43_AC_FN();

	/*
	 * Origin of the 0x0520[3:2] walk: 0x0000 on a first bring-up, 0x0004 on
	 * a later channel switch.
	 */
	dev->phy.ac->probe_mode =
		(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)
		? 0x0000 : 0x0004;

	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);

	/* Between the host-flag clear and the beacon reloads, as captured. */
	b43_phy_ac_cac_arm(dev);

	/*
	 * The vendor reloads the beacon template here, once or twice, then
	 * lights the two LEDs (gpio 2, gpio 10 active-low); in b43 both belong
	 * to the core.
	 */

	/*
	 * A cell set to 0xffff once per segment, between the LED GPIOs and the
	 * window latch. b43.h does not name it and its purpose is unknown; the
	 * position is invariant.
	 */
	B43_AC_CORE_SITE(dev, BEACON_START);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0026, 0xffff);

	/*
	 * When the calibrations ran, their last iteration requested a noise
	 * sample, whose completion brings block E; otherwise block E comes
	 * after the latch of the watchdog's first full turn. The latch and
	 * block E come from b43_phy_ac_noise_sample_done(), which the core
	 * calls when the sample is ready.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		dev->phy.ac->crs_update_pending = true;

	/*
	 * The bring-up tail ends here; from now on the flow is events: watchdog
	 * turns, one callback a second, until the radio goes down; radar polls
	 * every 150 ms on radar channels; the bss-up, which there closes the
	 * check and brings the calibrations; the core's template reloads.
	 */
	{
		struct b43_phy_ac *ac = dev->phy.ac;

		ac->wd_switch_turns = 0;
		ac->last_cal_channel = ac->cal_channel;
	}
}

/*
 * The PHY half of `wl down`, emitted when the interface goes down: about
 * 460 ops over 19 ms, ending with the AFE_OFF bank and the PMU release that
 * pairs with the cold preamble's request. Every cold segment has it a
 * second before the rmmod, and every hot up/down segment has it with no
 * rmmod at all.
 *
 * It is not the tail of the channel setup: on cold04 the calibration ends
 * at t=869.844, the watchdog turns run to 889.993, and this block sits at
 * 890.450-890.469. Only the coefficient write-back is cal state, which is
 * why @lo_dac and @txiqlo_coef live in the phy state.
 *
 * b43 reaches it from b43_phy_exit() through software_rfkill(blocked=true).
 * [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   36045-36542]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   28595-29084]
 */
static void b43_phy_ac_down(struct b43_wldev *dev)
{
	B43_AC_FN();
	u16 (*lo_dac)[4] = dev->phy.ac->lo_dac;
	u16 (*txiqlo_coef)[3] = dev->phy.ac->txiqlo_coef;

	/*
	 * Entered with the MAC suspended by b43_wireless_core_stop(). The stock
	 * down reads one more statistics counter and drops beacon promiscuity
	 * before its own suspend; both are the core's.
	 *
	 * Third and last pass of the twelve-rate loop, with the second one's
	 * shm prologue: chain mask into 0x00cc, beacon power offset into
	 * 0x00ce, zero into 0x00d0, then the chain-mask block. Three 0x00ce
	 * writes on every cold segment and four on every hot up segment, on
	 * every channel and width, unlike the beacon reloads, whose count the
	 * host decides.
	 *
	 * 0x00d0 is a bitfield the ucode slices, left at zero on all 849 writes
	 * of the three boards; a condition that sets it would require revising
	 * this.
	 */
	b43_phy_ac_bss_cc_update(dev, B43_PHY_AC_CHAIN_SETUP);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ce,
			b43_phy_ac_beacon_pwr_offset_at(dev, B43_PHY_AC_CHAIN_SETUP));
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00d0, 0x0000);
	b43_phy_ac_chainmask_block(dev, B43_PHY_AC_CHAIN_SETUP);
	b43_phy_ac_prb_rsp_rate_po(dev, B43_PHY_AC_CHAIN_SETUP);

	/*
	 * Final AFE configuration: peek and lock the gate, then the programming
	 * of txpwrctrl_setup() again on its cached sub-band; the gate stays
	 * locked. The three writes of table 0x21 carry the same payload on
	 * every cold segment, and on 3-core boards this covers 0x0a44 and the
	 * core-2 LUT 0x80 (tg789vac-v2 cold01 #42170-#42579). The bits on
	 * 0x0070/0x0072 (0x8000, 0x4000, 0x0100, 0x0700) are not documented.
	 */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_ac_txpwrctrl_program(dev, dev->phy.ac->pa5g_grp);

	/*
	 * Final tail, 72 ops: close the RX-IQ scope and program the per-core
	 * final coefficients; the steps are labelled in the body. The 0x17xx
	 * block, a 0x1000 stride over the gain registers 0x1720-0x173e, is a
	 * different address space, possibly a shadow bank; unverified.
	 */

	/* Unlock the gate, then the AFE gain registers as in block E. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	b43_phy_ac_afe_gain_regs_reemit(dev);

	/*
	 * Here the stock driver leaves the BSS (INFRA off, DISCPMQ on, then AP
	 * off), toggling the MAC around each write. That is
	 * b43_adjust_opmode()'s from remove_interface, and b43 keeps the MAC
	 * suspended through the down, so a toggle from here would not reach the
	 * register.
	 */
	B43_AC_CORE_SITE(dev, DOWN_OPMODE);
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);
	B43_AC_CORE_SITE(dev, DOWN_OPMODE_END);

	/*
	 * Per core: restore the TX IQ/LO coefficients saved in block D ({a, b}
	 * at 0x60 + 4*core, the LO leakage word at 0x62 + 4*core), then the LO
	 * DAC registers and the solved RX-IQ coefficients. Only where block D
	 * ran; see @iqlo_saved in phy_ac.h.
	 *
	 * The solver runs again over the same accumulators and writes the same
	 * pair as the first apply. The stock pair here is what block D's peek
	 * returned, but using the peek would hide the solver's residual on b
	 * from the score (see VAL_TOLLERANZA in test/unit/compare.py).
	 */
	if (dev->phy.ac->iqlo_saved) {
		unsigned int c;

		for_each_set_bit(c, &dev->phy.ac->coremask, dev->phy.ac->num_cores) {
			u16 stride = (u16)(c * 0x200);
			s16 a, b;

			b43_actab_write_bulk_scoped(dev, 0x000c,
						    (u16)(0x0060 + c * 4),
						    16, 2, &txiqlo_coef[c][0]);
			b43_actab_write_bulk_scoped(dev, 0x000c,
						    (u16)(0x0062 + c * 4),
						    16, 1, &txiqlo_coef[c][2]);
			b43_radio_write(dev, 0x0002 + stride, lo_dac[c][0]);
			b43_radio_write(dev, 0x0003 + stride, lo_dac[c][1]);
			b43_radio_write(dev, 0x0004 + stride, lo_dac[c][2]);
			b43_radio_write(dev, 0x0005 + stride, lo_dac[c][3]);

			b43_phy_ac_iq_solve(&dev->phy.ac->iq_acc[c], &a, &b);
			b43_phy_write(dev, 0x06a0 + stride, (u16)(a & 0x03ff));
			b43_phy_write(dev, 0x06a1 + stride, (u16)(b & 0x03ff));
		}
	}

	/*
	 * Here the stock down path toggles the MAC once more, switches the LEDs
	 * off and resets the core: b43_wireless_core_stop() and
	 * b43_wireless_core_exit() own that.
	 */
	B43_AC_CORE_SITE(dev, CORE_DOWN);

	/*
	 * The front end powered down, without a save of its own, then the PMU
	 * release.
	 */
	b43_phy_ac_afe_arm(dev, B43_PHY_AC_AFE_OFF, 0x0000, 0x0001);
	b43_phy_ac_pmu_req(dev, true);
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   5007-13592]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   677-9281]
 */
static int b43_phy_ac_op_switch_channel(struct b43_wldev *dev, unsigned int new_channel)
{
	B43_AC_FN();
	struct ieee80211_channel *channel = dev->wl->hw->conf.chandef.chan;
	const struct cfg80211_chan_def *chandef = &dev->wl->hw->conf.chandef;
	enum nl80211_chan_width width = chandef->width;
	const struct b43_phy_ac_channeltab_e_radio2069 *e2069;
	struct b43_phy *phy = &dev->phy;
	u16 off, ifs;

	if (!ALLOW_24 && b43_current_band(dev->wl) == NL80211_BAND_2GHZ) {
		b43info(dev->wl,
		       "AC-PHY: 2.4 GHz channel %u not supported on this board\n",
		       new_channel);
		return -EOPNOTSUPP;
	}

	/*
	 * b43 keeps the MAC active between channel switches, and this runs with
	 * it suspended. Suspend only if not already suspended: on bring-up the
	 * MAC arrives disabled from the core init, and a nested suspend would
	 * raise the refcount without writing, turning the calibration's
	 * enable/suspend pairs into no-ops. The enable is the caller's.
	 *
	 * MAC_EN in status_mask is maintained by b43_mac_suspend() and
	 * b43_mac_enable(); setting it here would desynchronise it from the
	 * refcount.
	 */
	if (!dev->mac_suspended)
		b43_mac_suspend(dev);

	/* 5 GHz: the channel table is the filter (unknown channels: -ESRCH). */

	b43info(dev->wl, "phy-ac: set_channel ch%u (%u MHz) start\n",
	       channel->hw_value, channel->center_freq);

	if (phy->radio_ver != 0x2069)
		return -ESRCH;

	if (phy->radio_rev != 4)
		return -ESRCH;

	/*
	 * The table is keyed on the frequency the radio is tuned to, which for
	 * a bonded configuration is the centre of the block: it has rows every
	 * 10 MHz. The primary would pick a 20 MHz row and mistune the
	 * synthesiser (radio 0x08dc in the cold sweep takes radio_raw[3] from
	 * this row).
	 */
	e2069 = b43_phy_ac_get_channeltab_e_r2069(dev,
			width == NL80211_CHAN_WIDTH_20 ? channel->center_freq
						       : chandef->center_freq1);
	if (!e2069)
		return -ESRCH;

	if (dev->dev->chip_id != 0x4352 && dev->dev->chip_id != 0x4360) {
		b43warn(dev->wl,
			"AC-PHY: chip 0x%04x not validated: the transcribed "
			"values are those of the 4352/4360\n",
			dev->dev->chip_id);
		return -EOPNOTSUPP;
	}

	if (dev->phy.ac->tuned &&
	    dev->phy.ac->cal_channel == channel->hw_value &&
	    dev->phy.ac->cal_width == width &&
	    dev->phy.ac->cac_pending == dev->cac_pending)
		return 0;

	/*
	 * A previous switch released the forced HT clock at its end; hold it
	 * again while the PHY is reprogrammed. After a core reset it is held
	 * already and tuned is clear.
	 */
	if (dev->phy.ac->tuned)
		bcma_core_set_clockmode(dev->dev->bdev, BCMA_CLKMODE_FAST);

	/*
	 * The chanspec heads a channel switch, right before its first PHY op.
	 * On the first switch after the RF bring-up it is already there, and
	 * the stock driver writes it once.
	 */
	if (dev->phy.ac->chanspec != b43_phy_ac_chanspec(chandef))
		b43_phy_ac_write_chanspec(dev);

	dev->phy.ac->cac_pending = dev->cac_pending;
	dev->phy.ac->tuned = false;
	dev->phy.ac->cal_channel = channel->hw_value;
	dev->phy.ac->cal_width = width;
	dev->phy.ac->cal_freq = width == NL80211_CHAN_WIDTH_20
				? channel->center_freq : chandef->center_freq1;
	B43_AC_BLOCK(dev, "channel_state");
	b43_phy_ac_crs_note_channel(dev);
	b43_phy_ac_txpwr_recalc(dev);

	/*
	 * The MAC is suspended here and no precondition is put on the
	 * classifier or clip detect: after attach they are clear, on a channel
	 * change they hold the previous channel's state; both are set further
	 * down.
	 */
	B43_PHY_AC_REQUIRE_RET(dev,
			       0,
			       B43_PHY_AC_STATE_MAC_EN | B43_PHY_AC_STATE_CCA_RESET,
			       -EINVAL);

	/*
	 * Drop the PMU request before reconfiguring the channel, only if
	 * raised: on the attach path the preamble has already dropped it.
	 */
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_PMU_REQ)
		b43_phy_ac_pmu_req(dev, false);

	/*
	 * Freeze the RX path before the channel reconfiguration: classifier
	 * WAITED only, clip detectors frozen, CCA reset. Everything up to
	 * rx_enable runs frozen; the first release is the RX gate pulse in
	 * idle_tssi_meas.
	 */
	B43_AC_BLOCK(dev, "switch_prep");
	b43_phy_ac_channel_switch_prep(dev);

	B43_AC_BLOCK(dev, "radio_channel_setup");
	b43_radio_2069_channel_setup(dev, e2069);
	B43_AC_BLOCK(dev, "phy_channel_setup");
	b43_phy_ac_channel_setup(dev, e2069, channel);

	B43_AC_BLOCK(dev, "chan_tables");
	b43_phy_ac_chan_tables(dev);

	B43_AC_BLOCK(dev, "noise_shaping");
	/*
	 * Noise-shaping table init: per-core noise variance (table 0x15), gain
	 * limit (0x0b) and noise-shaping coefficients (0x44/0x45 per core,
	 * table ID stride 0x20), from the d6220 ch36 trace.
	 *
	 * On 2.4 GHz the MacBookAir6,1 and the archer-t5e (6.30 hybrid) write
	 * the nshp runs below, the same on both boards and every core. The
	 * first cell of each run is not taken from them: 6.30 and 7.14 already
	 * differ there on 5 GHz, so 2.4 GHz keeps 7.14's 5 GHz value and warns.
	 */
	{
		static const u16 nvar_data[5] = { 0x0020, 0x0021, 0x0022, 0x0023, 0x0024 };
		static const u16 nvar_off[3]  = { 0x0008, 0x0020, 0x0038 };
		static const u16 glim_a[6]    = { 0x000b, 0x000c, 0x000e, 0x0020, 0x0024, 0x0028 };
		static const u16 glim_b[7]    = { 0x0000, 0x0000, 0x0000, 0x0003, 0x0003, 0x0003, 0x0003 };
		static const u16 nshp_a8[6]   = { 0x00f9, 0x00fe, 0x0004, 0x000a, 0x0010, 0x0017 };
		static const u16 nshp_b8[6]   = { 0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005 };
		static const u16 nshp_a10[7]  = { 0x00f5, 0x00f8, 0x00fb, 0x00fe, 0x0002, 0x0005, 0x0009 };
		static const u16 nshp_b10[7]  = { 0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006 };
		static const u16 nshp_a8_2g[6]  = { 0x00f9, 0x00ff, 0x0006, 0x000c, 0x0012, 0x0019 };
		static const u16 nshp_a10_2g[7] = { 0x00f5, 0x00f8, 0x00fc, 0x00ff, 0x0002, 0x0002, 0x0002 };
		static const u16 nshp_b10_2g[7] = { 0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0004, 0x0004 };
		bool band_2g = b43_current_band(dev->wl) == NL80211_BAND_2GHZ;
		const u16 *a8 = band_2g ? nshp_a8_2g : nshp_a8;
		const u16 *a10 = band_2g ? nshp_a10_2g : nshp_a10;
		const u16 *b10 = band_2g ? nshp_b10_2g : nshp_b10;
		u16 saved, inner;
		unsigned int core;

		if (band_2g)
			b43_phy_ac_todo(dev,
				"2.4 GHz noise shaping: the first cell of "
				"each run is the 5 GHz value, not measured "
				"on 2.4 GHz\n");

		/*
		 * Phase 1: gate cycle, enable, shared tables, under the
		 * noise-shaping tables' own lock of the 0x019e gate.
		 */
		saved = b43_phy_ac_tbl_write_lock(dev);

		b43_phy_read_log(dev, 0x016c);                        /* peek */
		b43_phy_maskset(dev, 0x016c, (u16)~0x0040, 0x0040);

		/*
		 * Per-core nvar, three writes to table 0x15 before table 0x0b,
		 * on the d6220 and the tg789vac alike; the agcombo's older
		 * driver has no op on table 0x15.
		 */
		for (core = 0; core < dev->phy.ac->num_cores; core++)
			b43_actab_write_bulk(dev, 0x15, nvar_off[core], 16, 5, nvar_data);

		/* Gain limit, two writes to table 0x0b. */
		b43_actab_write_bulk(dev, 0x0b, 0x0008, 16, 6, glim_a);
		b43_actab_write_bulk(dev, 0x0b, 0x0010, 16, 7, glim_b);

		/* Phases 2 and 2b: each a lock nested in phase 1's. */
		inner = b43_phy_ac_tbl_write_lock(dev);

		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 ta = 0x44 + core * 0x20;	/* core 0=0x44, 1=0x64, 2=0x84 */
			u16 tb = 0x45 + core * 0x20;

			b43_actab_read_log(dev, 0x15, nvar_off[core], 6);
			b43_actab_write_bulk(dev, ta, 0x0008, 16, 6, a8);
			b43_actab_write_bulk(dev, tb, 0x0008, 16, 6, nshp_b8);
		}

		b43_phy_ac_tbl_write_unlock(dev, inner);
		inner = b43_phy_ac_tbl_write_lock(dev);

		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 ta = 0x44 + core * 0x20;
			u16 tb = 0x45 + core * 0x20;

			b43_actab_write_bulk(dev, ta, 0x0010, 16, 7, a10);
			b43_actab_write_bulk(dev, tb, 0x0010, 16, 7, b10);
		}

		b43_phy_ac_tbl_write_unlock(dev, inner);

		/*
		 * Phase 3: per-core commit and read-back, gate still locked, on
		 * every silicon core with no coremask filter.
		 */
		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 ta = (u16)(0x44 + core * 0x20);
			u16 tb = (u16)(0x45 + core * 0x20);

			b43_phy_ac_rxgain_init(dev, core);

			b43_actab_read_log(dev, tb, 0x0000, 1);
			b43_actab_read_log(dev, tb, 0x0020, 10);
			b43_actab_read_log(dev, ta, 0x0060, 8);
			b43_actab_read_log(dev, ta, 0x0070, 8);
			b43_actab_read_log(dev, tb, 0x0060, 8);
			b43_actab_read_log(dev, tb, 0x0070, 8);
		}

		/* Close phase 1: the 0x016c enable, then its lock. */
		b43_phy_write(dev, 0x016c, 0x0000);
		b43_phy_ac_tbl_write_unlock(dev, saved);
	}

	B43_AC_BLOCK(dev, "rx_regprog");
	b43_phy_ac_post_noise_shaping_rx_regprog(dev);

	/*
	 * Right after the post-noise-shaping block: two diagnostic peeks and
	 * 0x3600 under mask 0xff00 into the eight CRS registers (the high byte;
	 * chanspec_tail() and block E write the low one). Transcribed.
	 */
	B43_AC_BLOCK(dev, "crs_thresholds");
	b43_phy_read_log(dev, 0x06dc);
	b43_phy_read_log(dev, 0x06dd);
	b43_phy_maskset(dev, 0x0324, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x0330, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x0321, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x032d, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x032a, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x0336, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x0327, (u16)~0xff00, 0x3600);
	b43_phy_maskset(dev, 0x0333, (u16)~0xff00, 0x3600);

	/* BW select per core, before reset_cca/afecal. */
	b43_phy_write(dev, 0x0304, 0x4e51);
	b43_phy_write(dev, 0x0307, 0x4e51);
	b43_phy_write(dev, 0x030a, 0x4e51);
	b43_phy_write(dev, 0x030d, 0x4e51);

	/*
	 * Two writes on 2.4 GHz only, on all 41 2.4 GHz segments of the
	 * MacBookAir6,1 and the archer-t5e and on no 5 GHz one. Meaning not
	 * identified.
	 */
	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ) {
		b43_phy_write(dev, 0x0299, 0x4477);
		b43_phy_write(dev, 0x03c1, 0x0010);
	}

	B43_AC_BLOCK(dev, "reset_cca");
	b43_phy_ac_reset_cca(dev);
	udelay(1);

	B43_AC_BLOCK(dev, "afecal");
	b43_radio_2069_afecal(dev);

	/*
	 * ADC reset (cal time), after afecal and before the idle-TSSI capture,
	 * then the 0x70[15:13] TX power control enable.
	 */
	B43_AC_BLOCK(dev, "adc_reset_txpwrctrl");
	b43_phy_ac_adc_reset(dev);
	b43_phy_ac_txpwrctrl_enable(dev);

	/*
	 * Idle-TSSI measurement, iteration 1. The REQUIRE preconditions are per
	 * call: iterations 2 and 3 run with different MAC states.
	 */
	B43_AC_BLOCK(dev, "idle_tssi");
	if (b43_phy_ac_may_calibrate_tx(dev)) {
		B43_PHY_AC_REQUIRE_RET(dev,
				       B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
				       B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
				       B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN,
				       -EINVAL);
		b43_phy_ac_idle_tssi_meas(dev);

		/*
		 * Settle before txpwrctrl reads the idle-TSSI result. The trace
		 * gap is about 35 ms, read as the stock driver's scheduling
		 * since idle_tssi_meas() polls its own busy bit. If the base
		 * index comes out wrong on hardware, mdelay(35) is the first
		 * thing to try.
		 */
		udelay(35);

		/*
		 * The close of the bracket txpwrctrl_enable() opened: 0x0339
		 * back to 0x0fff, with the measurement for the same reason as
		 * the arm (cold05: two accesses, both from the channel setup).
		 */
		b43_phy_write(dev, 0x0339, 0x0fff);
	}

	/*
	 * idle_tssi to txpwrctrl transition: the shared-memory block, SPUWKUP,
	 * MHF slot 4 set 0x0008, RFATT, the direct-map scan, the key table
	 * site, then the MAC config block. No RX gate arm here:
	 * txpwrctrl_setup() runs with the gate released from the idle_tssi
	 * phase.
	 */
	B43_AC_BLOCK(dev, "shm_readback");
	b43_phy_ac_shm_readback_block(dev);
	/*
	 * SPUWKUP, the synthesizer pre-wakeup in microseconds (M_SYNTHPU_DLY,
	 * 0x4a*2). The constant is per PHY type: brcmsmac has 3700 for A-PHY,
	 * 1050 for B-PHY, 2048 for N-PHY rev >= 3 and 300 for LCN, chosen in
	 * brcms_b_upd_synthpu(); 512 is the AC member of that family.
	 *
	 * b43_set_synth_pu_delay() in main.c rewrites the cell with the same
	 * value. Its 500 for adhoc and idle is not in brcmsmac but is in the
	 * stock driver: 7.14.43 writes it at the start of the bring-up,
	 * 6.30.223 alternates 500 and 512 on every hop of a scan.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0094, 512);
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x0008, 0x0008);
	/*
	 * RFATT, right after the HOSTF5 write-through the call above causes:
	 * 0x0480 on all 26 d6220 cold segments, once each, and the same on the
	 * DSL at the same position. Constant over channel and width; its
	 * purpose on an AC-PHY is not known.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_RFATT, 0x0480);
	/*
	 * Read-only scan of the two direct-map tables, sixteen entries each,
	 * closed by the read of 0x0056. The OFDM pointers are kept for
	 * b43_phy_ac_basic_rate_map(); the rest is read because the stock
	 * driver reads it.
	 */
	for (off = B43_AC_RT_DIRMAP_A; off <= B43_AC_RT_DIRMAP_A + 0x1e; off += 2) {
		u16 v = b43_shm_read16(dev, B43_SHM_SHARED, off);
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(b43_phy_ac_ofdm_dirmap); k++)
			if (B43_AC_RT_DIRMAP_A +
			    b43_phy_ac_ofdm_dirmap[k] * 2 == off)
				dev->phy.ac->rate_ptr[k] = v;
	}
	for (off = B43_AC_RT_DIRMAP_B; off <= B43_AC_RT_DIRMAP_B + 0x1e; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
	b43_shm_read16(dev, B43_SHM_SHARED, 0x0056);
	/*
	 * 0x0056 is the key table pointer, B43_SHM_SH_KTP: 0x087a, the key
	 * material at 0x10f4. Here the stock driver zeroes it (480 words up to
	 * 0x14b2), then the corerev 42 key index block at 0x05e0-0x0666
	 * (b43_shm_sh_keyidxblock(), 68 words), then the key rows of the
	 * address match table: the key table init of the stock up (cold01
	 * #12311-#12858). In b43 it is b43_security_init()'s, at core init.
	 */
	B43_AC_CORE_SITE(dev, KEYS_CLEAR);
	/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
	 *   12859-13592]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   8546-9281]
	 */
	B43_AC_BLOCK(dev, "shm_mac_config");
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);
	b43_phy_ac_shm_mac_config_block(dev);
	/* Second half of the poll with a single pass: no head and no sweep. */
	b43_phy_ac_wd_stats_poll_opt(dev, false, 1, true);
	/*
	 * Invariant across all 26 segments; purpose unknown. Between these and
	 * the poll below the capture has a TPL.RAMW, the core's template RAM.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x018a, 0xffce);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x018c, 0xffba);
	B43_AC_CORE_SITE(dev, MACFILTER_FIRST);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0, true);
	/* After the sweep and the four CCK blocks not understood yet. */
	b43_phy_ac_chainmask_block(dev, B43_PHY_AC_CHAIN_SETUP);
	b43_phy_ac_prb_rsp_rate_po(dev, B43_PHY_AC_CHAIN_SETUP);
	b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	B43_AC_BLOCK(dev, "txpwrctrl_setup");
	b43_phy_ac_txpwrctrl_setup(dev, channel->center_freq);

	/*
	 * Transition between the first and second txpwrctrl_setup(): release
	 * the gate, enable TX power control, restore the per-core current
	 * index, wake the MAC so that the firmware picks up the new
	 * configuration, then suspend it again.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);           /* unlock */
	/*
	 * TX power control enable. The gain word is absent on a first bring-up,
	 * here and in adc_reset(), but always present in the tail of
	 * rxiqcal_finalize().
	 */
	B43_AC_BLOCK(dev, "afe_gain");
	b43_phy_ac_afe_gain_regs(dev, !(dev->phy.ac->status_mask &
				       B43_PHY_AC_STATE_FIRST_BRINGUP));
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);    /* MHF3 set bit 6 */
	b43_phy_ac_ofdm_pctl1_readback(dev);
	/*
	 * PRMAXTIME = 0, i.e. no timeout for the firmware's probe response. Not
	 * transcribed: it is what b43_chip_init() writes. b43 sets it back to 1
	 * in b43_wireless_core_init() to disable the offload, a write the
	 * capture does not have; see "Probe-response offload" in
	 * docs/retrace-todo.md.
	 */
	B43_AC_BLOCK(dev, "shm_tail");
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0074, 0x0000);
	/*
	 * Invariant across all 26 segments, purpose unknown. Only the first of
	 * the map's two passes carries them.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0082, 0x2710);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ba, 0xffff);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x003c, 0x000a);
	b43_phy_ac_chainmask_block(dev, B43_PHY_AC_CHAIN_SETUP);
	b43_phy_ac_basic_rate_map(dev);
	B43_AC_BLOCK(dev, "mac_toggles");
	B43_AC_CORE_SITE(dev, OPMODE_FILTERS);
	b43_mac_enable(dev);
	/*
	 * The four PWRIND_BLKS cells preceding the 0x0308 that crs_note_noise()
	 * samples, read right after the MAC is re-enabled; nothing consumes
	 * them.
	 */
	for (off = 0x0300; off <= 0x0306; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0, true);
	/* MHF4 bit 15: set on a first bring-up, cleared on a later one. */
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x8000,
			       (dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP) ? 0x8000 : 0);
	/* MHF1 bit 0: the opposite polarity. */
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0001,
			       (dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP) ? 0 : 0x0001);
	b43_mac_suspend(dev);
	/* Invariant across all 26 cold segments; purpose unknown. */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x007c, 0x0320);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0020, 0x0020);    /* MHF3 set bit 5 */
	b43_mac_enable(dev);
	/*
	 * Here the stock driver stops forcing the HT clock, which it holds
	 * since the core reset, and writes two IFS registers of the IHR around
	 * shm 0x005a: 0x06c6 takes the shm value, as the 928 ucode copies it
	 * there itself. Neither the 928 nor the 784 ucode touches 0x06c8, so
	 * its value is the driver's: 0x000c under wl 7.14 (agcombo), 0x0003
	 * under wl 6.30.223 (Archer T5E and MacBookAir6,1, on 5 GHz only); what
	 * it holds is not known.
	 *
	 * shm 0x005a depends on the width alone, 0x0f0f at 20 and 80 MHz and
	 * 0x0303 at 40, the same value in both bytes. Why 40 MHz differs is not
	 * known.
	 */
	bcma_core_set_clockmode(dev->dev->bdev, BCMA_CLKMODE_DYNAMIC);
	b43_write16(dev, 0x06c8, 0x000c);
	ifs = (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40) ? 0x0303 : 0x0f0f;
	b43_shm_write16(dev, B43_SHM_SHARED, 0x005a, ifs);
	b43_write16(dev, 0x06c6, ifs);
	b43_phy_maskset(dev, 0x0042, (u16)~0x8000, 0x8000);
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0020, 0x0020);    /* MHF1 set bit 5 */
	b43_mac_suspend(dev);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0, true);
	B43_AC_CORE_SITE(dev, MACFILTER);
	b43_phy_ac_basic_rate_map(dev);

	/*
	 * The mac_enable and the post-channel calibrations the stock driver
	 * emits next are the caller's: the calibrations need the MAC active,
	 * and leaving the enable out keeps the window in which the core writes
	 * the BSS configuration with the MAC suspended.
	 */
	B43_AC_BLOCK(dev, "done");
	dev->phy.ac->txpwr_adjust_due = true;
	dev->phy.ac->tuned = true;
	return 0;
}

static u16 b43_phy_ac_op_read(struct b43_wldev *dev, u16 reg)
{
	if (B43_WARN_ON(reg == 0xFFFF))
		return 0;
	b43_write16f(dev, B43_MMIO_PHY_CONTROL, reg);
	return b43_read16(dev, B43_MMIO_PHY_DATA);
}

static void b43_phy_ac_op_write(struct b43_wldev *dev, u16 reg, u16 value)
{
	if (B43_WARN_ON(reg == 0xFFFF))
		return;
	b43_write16f(dev, B43_MMIO_PHY_CONTROL, reg);
	b43_write16(dev, B43_MMIO_PHY_DATA, value);
}

const struct b43_phy_operations b43_phyops_ac = {
	.allocate		= b43_phy_ac_op_allocate,
	.free			= b43_phy_ac_op_free,
	.prepare_structs	= b43_phy_ac_op_prepare_structs,
	.init			= b43_phy_ac_op_init,
	.phy_read		= b43_phy_ac_op_read,
	.phy_write		= b43_phy_ac_op_write,
	.phy_maskset		= b43_phy_ac_op_maskset,
	.radio_read		= b43_phy_ac_op_radio_read,
	.radio_write		= b43_phy_ac_op_radio_write,
	.software_rfkill	= b43_phy_ac_op_software_rfkill,
	.switch_analog		= b43_phy_ac_op_switch_analog,
	.switch_channel		= b43_phy_ac_op_switch_channel,
	.get_default_chan	= b43_phy_ac_op_get_default_chan,
	.recalc_txpower		= b43_phy_ac_op_recalc_txpower,
	.adjust_txpower		= b43_phy_ac_op_adjust_txpower,
	.pwork_1sec		= b43_phy_ac_op_pwork_1sec,
	.pwork_60sec		= b43_phy_ac_op_pwork_60sec,
	.channel_calibrate	= b43_phy_ac_op_channel_calibrate,
	.noise_sample_done	= b43_phy_ac_noise_sample_done,
	.radar_poll		= b43_phy_ac_radar_poll,
	.cac_done		= b43_phy_ac_bss_up,
};

/* ==========================================================================
 * Farrow resampler setup
 * ==========================================================================
 */

static const u16 b43_phy_ac_farrow_vals[196] = {
	0x096c, 0x0971, 0x0976, 0x097b, 0x0980, 0x0985, 0x098a, 0x098f,
	0x0994, 0x0999, 0x099e, 0x09a3, 0x09a8, 0x09b4, 0x143c, 0x1450,
	0x1464, 0x1478, 0x148c, 0x14a0, 0x14b4, 0x14c8, 0x157c, 0x1590,
	0x15a4, 0x15b8, 0x15cc, 0x15e0, 0x15f4, 0x1608, 0x161c, 0x1630,
	0x1644, 0x1671, 0x1685, 0x1699, 0x16ad, 0x16c1, 0x1446, 0x146e,
	0x1496, 0x14be, 0x1586, 0x15ae, 0x15d6, 0x15fe, 0x1626, 0x167b,
	0x16a3, 0xf1fe, 0xd703, 0xbc25, 0xa163, 0x86bd, 0x6c33, 0x51c5,
	0x3773, 0x1d3c, 0x0321, 0xe920, 0xcf3b, 0xb570, 0x77f7, 0x71ab,
	0x42f5, 0x149a, 0xe699, 0xb8f2, 0x8ba3, 0x5eac, 0x320c, 0x44e4,
	0x1643, 0xe7f9, 0xba04, 0x8c64, 0x5f16, 0x321c, 0x0573, 0xd91b,
	0xad13, 0x8159, 0x2016, 0xf558, 0xcae6, 0xa0bf, 0x76e2, 0x5a45,
	0xfd8e, 0xa240, 0x4851, 0x2d89, 0xd0f4, 0x75b3, 0x1bbd, 0xc30d,
	0x0aae, 0xb5c9, 0x0072, 0x0072, 0x0072, 0x0072, 0x0072, 0x0072,
	0x0072, 0x0072, 0x0072, 0x0072, 0x0071, 0x0071, 0x0071, 0x0071,
	0x00ef, 0x00ef, 0x00ef, 0x00ee, 0x00ee, 0x00ee, 0x00ee, 0x00ee,
	0x00f2, 0x00f2, 0x00f1, 0x00f1, 0x00f1, 0x00f1, 0x00f1, 0x00f1,
	0x00f0, 0x00f0, 0x00f0, 0x00f0, 0x00ef, 0x00ef, 0x00ef, 0x00ef,
	0x00ef, 0x00ee, 0x00ee, 0x00ee, 0x00f2, 0x00f1, 0x00f1, 0x00f1,
	0x00f0, 0x00f0, 0x00ef, 0x20cd, 0x2122, 0x2177, 0x21cd, 0x2222,
	0x2277, 0x22cd, 0x2322, 0x2377, 0x23cd, 0x2422, 0x2477, 0x24cd,
	0x259a, 0x2cab, 0x2d55, 0x2e00, 0x2eab, 0x2f55, 0x3000, 0x30ab,
	0x3155, 0x22f7, 0x238e, 0x2426, 0x24be, 0x2555, 0x25ed, 0x2685,
	0x271c, 0x27b4, 0x284c, 0x28e4, 0x2a39, 0x2ad1, 0x2b68, 0x2c00,
	0x2c98, 0x2d00, 0x2e55, 0x2fab, 0x3100, 0x2342, 0x2472, 0x25a1,
	0x26d1, 0x2800, 0x2a85, 0x2bb4,
};

static const u16 b43_phy_ac_farrow_vals_432x_media_a1[98] = {
	0x096c, 0x0971, 0x0976, 0x097b, 0x0980, 0x0985, 0x098a, 0x098f,
	0x0994, 0x0999, 0x099e, 0x09a3, 0x09a8, 0x09b4, 0x143c, 0x1450,
	0x1464, 0x1478, 0x148c, 0x14a0, 0x14b4, 0x14c8, 0x157c, 0x1590,
	0x15a4, 0x15b8, 0x15cc, 0x15e0, 0x15f4, 0x1608, 0x161c, 0x1630,
	0x1644, 0x1671, 0x1685, 0x1699, 0x16ad, 0x16c1, 0x1446, 0x146e,
	0x1496, 0x14be, 0x1586, 0x15ae, 0x15d6, 0x15fe, 0x1626, 0x167b,
	0x16a3, 0x0008, 0x0009, 0x0009, 0x0009, 0x0009, 0x0009, 0x0009,
	0x0009, 0x0009, 0x0009, 0x0009, 0x000a, 0x0009, 0x0009, 0x0008,
	0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0009,
	0x0009, 0x0009, 0x000a, 0x0009, 0x0009, 0x0009, 0x0009, 0x0009,
	0x0009, 0x0009, 0x0009, 0x0009, 0x0009, 0x000a, 0x0009, 0x0008,
	0x0008, 0x0008, 0x0009, 0x0009, 0x000a, 0x0009, 0x0009, 0x0009,
	0x000a, 0x0009,
};

/*
 * Farrow resampler ratio and deltaphase, both functions of the centre
 * frequency and the bandwidth mode:
 *
 *   ratio  = round(f_MHz * 2^18 * M / D)
 *   dphase = round(K / (M * f_MHz))
 *
 * with (D, M, K) = (60, 1, 0x7_8000_0000) at 20 and 40 MHz and
 * (45, 1, 0xB_4000_0000) at 80. The ratio is 26 bits: 0x019a takes 15:0,
 * 0x019b 23:16, and 25:24 sit at 8:7 of 0x0199, above the mode's own field.
 * Bit 24 is set on every 5 GHz channel but the top one at 80 MHz, 5775 MHz,
 * where the ratio crosses 2^25 and 0x0199 goes from 0x0084 to 0x0104.
 *
 * The 80 MHz mode also swaps the low field of 0x0199/0x01a0 and
 * 0x019c/0x01a3, so it reads as a sample-rate mode rather than a
 * per-bandwidth scaling.
 *
 * On 2.4 GHz the block is the same with D/M = 80/3, K = D * 2^29, and
 * 0x1400 in 0x019c/0x01a3: exact on ch1-13 of the MacBookAir6,1 and ch1-11
 * of the archer-t5e. Both are 20 MHz scans; there is no 2.4 GHz 40 MHz
 * point.
 *
 * Exact on every configuration of the d6220 and tg789vac cold sweeps (25
 * channels at 20 MHz, 12 at 40, 6 at 80), both registers and both cores.
 */

struct b43_phy_ac_farrow_mode {
	u16 div;		/* D in the ratio */
	u16 mul;		/* M in the ratio and the deltaphase */
	u64 dphase_num;		/* K in the deltaphase */
	u16 mu;			/* 0x0199 / 0x01a0, bits 6:0 */
	u16 cfg;		/* 0x019c / 0x01a3 */
};

static const struct b43_phy_ac_farrow_mode b43_phy_ac_farrow_mode_20_40 = {
	.div = 60, .mul = 1, .dphase_num = 0x780000000ull, .mu = 0x0027,
	.cfg = 0x0f00,
};

static const struct b43_phy_ac_farrow_mode b43_phy_ac_farrow_mode_80 = {
	.div = 45, .mul = 1, .dphase_num = 0xb40000000ull, .mu = 0x0004,
	.cfg = 0x0b40,
};

static const struct b43_phy_ac_farrow_mode b43_phy_ac_farrow_mode_2g_20 = {
	.div = 80, .mul = 3, .dphase_num = 0xa00000000ull, .mu = 0x0027,
	.cfg = 0x1400,
};

/* [capture-ref: router-data/d6220/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   7444-7458]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   3114-3128]
 */
static void b43_phy_ac_farrow_setup(struct b43_wldev *dev,
				    struct ieee80211_channel *channel)
{
	/* Core 0's 0x0601, propagated to every core through the broadcast alias. */
	u16 core0;
	B43_AC_FN();
	const struct b43_phy_ac_farrow_mode *m = &b43_phy_ac_farrow_mode_20_40;
	const struct cfg80211_chan_def *chandef = &dev->wl->hw->conf.chandef;
	enum nl80211_chan_width width = chandef->width;
	unsigned int freq;
	u32 ratio, dphase;
	u16 mu;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET);

	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ) {
		if (width != NL80211_CHAN_WIDTH_20 &&
		    width != NL80211_CHAN_WIDTH_20_NOHT) {
			b43_phy_ac_todo(dev,
				"Farrow resampler not programmed at "
				"2.4 GHz with width %d: not measured\n",
				width);
			return;
		}
		m = &b43_phy_ac_farrow_mode_2g_20;
	}

	/*
	 * The operating channel's centre frequency, and the mode that goes
	 * with the width. A 20 MHz channel is centred on itself; a bonded one
	 * is centred on chandef.center_freq1, which is the only place the
	 * bonded centre exists.
	 */
	if (width == NL80211_CHAN_WIDTH_80) {
		m = &b43_phy_ac_farrow_mode_80;
		freq = chandef->center_freq1;
	} else if (width == NL80211_CHAN_WIDTH_40) {
		freq = chandef->center_freq1;
	} else {
		freq = channel->center_freq;
	}

	ratio = (u32)DIV_ROUND_CLOSEST(freq * m->mul * (1u << 18), m->div);
	dphase = (u32)DIV_ROUND_CLOSEST_ULL(m->dphase_num, m->mul * freq);
	mu = (u16)(m->mu | ((ratio >> 24) & 0x3) << 7);

	/*
	 * The stock order: the low half of each 32-bit value before its high
	 * half, core 0 then core 1, then core 0's 0x0601 propagated through the
	 * broadcast alias. 14 ops.
	 */
	b43_phy_write(dev, 0x019a, ratio & 0xffff);
	b43_phy_write(dev, 0x019b, (ratio >> 16) & 0xff);
	b43_phy_write(dev, 0x019c, m->cfg);
	b43_phy_write(dev, 0x0199, mu);

	b43_phy_write(dev, 0x01a1, ratio & 0xffff);
	b43_phy_write(dev, 0x01a2, (ratio >> 16) & 0xff);
	b43_phy_write(dev, 0x01a3, m->cfg);
	b43_phy_write(dev, 0x01a0, mu);

	b43_phy_write(dev, 0x1603, dphase & 0xffff);
	b43_phy_write(dev, 0x1602, dphase >> 16);
	b43_phy_write(dev, 0x1607, dphase & 0xffff);
	b43_phy_write(dev, 0x1606, dphase >> 16);

	core0 = b43_phy_read_log(dev, 0x0601);

	b43_phy_write(dev, 0x1601, core0);

	/* Raw tap tables, not consumed by this block. */
	(void)b43_phy_ac_farrow_vals;
	(void)b43_phy_ac_farrow_vals_432x_media_a1;
}
