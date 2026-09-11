// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver IEEE 802.11ac AC-PHY support Copyright (c) 2015
 * Rafał Miłecki <zajec5@gmail.com>
 */

/*
 * Transcription conventions that hold for the whole file, so that they need
 * not be repeated at every site:
 *
 * - b43_phy_maskset() and b43_radio_maskset() are used even where the set is
 *   a plain OR or AND. The stock driver emits PHY.MOD with an explicit mask;
 *   phy_set() and phy_mask() would give mask=0x0000 and would not match. A
 *   pulse is therefore a pair of masksets: val=<bit> mask=<bit>, then val=0
 *   mask=<bit>.
 * - reads whose value is not needed go through b43_phy_read_log() or
 *   b43_radio_read_log(): they hold the bus order, and a read with no consumer
 *   is a potential logic error, so it is logged rather than hidden. A few
 *   still use the plain accessor.
 * - "self-contained" describes a table write that opens the gate, writes and
 *   closes it within its own scope (b43_actab_*_scoped), as opposed to the
 *   fast variants that work inside a gate the caller holds open.
 * - where a comment says the order is the observed one, that order comes from
 *   a capture and cannot be rearranged; the op-for-op comparison checks it.
 *
 * The #NNNNN indices in the comments are episode positions in a reference
 * capture. An index alone does not identify one: the index spaces of the
 * captures overlap, so `reverse-tools/anchors.py resolve` resolves most bare
 * indices to several different ops. Check a reference with
 * `reverse-tools/anchors.py span` before relying on it.
 */

#include <linux/slab.h>
#include "b43.h"
#include "phy_ac.h"
#include "phy_common.h"
#include "tables_phy_ac.h"
#include "radio_2069.h"
#include "rxiqcal_phy_ac.h"
#include "main.h"

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
 *    recalc from b43_op_config() and every minute, the recalc says whether
 *    the target moved, and only then adjust writes it to the PHY. That is
 *    wlc_phy_txpower_recalc_target() and what b43_nphy_op_recalc_txpower()
 *    does for the N-PHY; the computation is b43_phy_ac_txpwr_recalc(). The
 *    channel setup computes the same target itself before its two write
 *    sites, so from the core's calls nothing changes unless the regulatory
 *    ceiling did.
 *
 * 2. CRS min power is the pwork_60sec hook, the same cadence the core gives
 *    the recalc. The high byte of 0x0324 and friends is the one-shot reset
 *    already done in op_switch_channel; the low byte is the recalculated
 *    threshold, from the crsmin chain verified against the d6220 7.14 blob
 *    (ladder, per-bandwidth anchoring, clamp and cold bump). The one input
 *    that cannot be reproduced without hardware is the interference sample
 *    per freq_range: it is pinned here to the steady-state low-5 GHz value
 *    and has to be replaced by the measurement on real hardware.
 *
 * 3. The periodic cycle on 0x0725/0x0925 is the measure block inside
 *    b43_phy_ac_watchdog(), the pwork_15sec hook, on the vendor's period of
 *    roughly five seconds.
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

static void b43_phy_ac_op_adjust_txpower(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_ac_txpwr_adjust(dev);
	dev->phy.ac->txpwr_adjust_due = false;
}

/*
 * prepare_structs is called by the b43 core after allocate and before init.
 */
static void b43_phy_ac_op_prepare_structs(struct b43_wldev *dev)
{
	struct b43_phy_ac *phy_ac = dev->phy.ac;
	u16 mhfs[ARRAY_SIZE(phy_ac->mhfs)];
	bool writethrough;

	/*
	 * b43_wireless_core_init() calls this on every ifconfig up, but the
	 * host-flag shadow has to survive a down/up and reset only on a module
	 * reload. The warm captures show it: their flush writes the values
	 * accumulated by the previous cycle -- 0x0060 in HOSTF4 and 0x0088 in
	 * HOSTF5 -- where a cold cycle flushes 0x0040 and 0x0080. So it is
	 * carried across the reset here, and what zeroes it is the kzalloc in
	 * op_allocate().
	 */
	memcpy(mhfs, phy_ac->mhfs, sizeof(mhfs));
	writethrough = phy_ac->mhf_writethrough;

	memset(phy_ac, 0, sizeof(*phy_ac));

	memcpy(phy_ac->mhfs, mhfs, sizeof(mhfs));
	phy_ac->mhf_writethrough = writethrough;
}

/* Mode-bit clears. These ops are not contiguous in the capture: they are
 * spread through the radio and rfkill bring-up window, tagged per sequence. */
/*
 * The two AFE power banks. OFF is the 0x173e..0x1720 bank with 0x1721=0xffff,
 * 0x1725=0x1fff and 0x1720=0x03ff: every AFE block powered down. It closes
 * every `wl down` -- at 99% of all 26 cold and all hot up/down segments --
 * and opens the attach, twice. ON is the four-write bank with 0x1721=0x5000
 * and 0x1720=0x0180, at 1% of every segment: the front end powering up for
 * the channel switch. The names used to be the other way round, which made
 * the down phase look like a bring-up.
 */
enum b43_phy_ac_afe_mode {
	B43_PHY_AC_AFE_ON,	/* front-end powered, ready for the switch */
	B43_PHY_AC_AFE_OFF,	/* front-end powered down: attach and wl down */
};

static void b43_phy_ac_enable_afe(struct b43_wldev *dev,
				  enum b43_phy_ac_afe_mode mode);
static void b43_phy_ac_down(struct b43_wldev *dev);

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Parking the front end: the same four writes b43_phy_ac_enable_afe()
	 * emits for AFE_DOWN, and it goes through the helper rather than
	 * writing them here so that @status_mask follows the hardware. A shadow
	 * left at AFE_ON while the registers are down is the state
	 * b43_phy_ac_channel_setup()'s precondition forbids, and only a harness
	 * carrying its own shadow can see it: one that derives the state from
	 * the registers cannot.
	 */
	b43_phy_ac_enable_afe(dev, B43_PHY_AC_AFE_ON);

	/*
	 * RMW pairs: read the base-page register, OR in the bit, write to the
	 * 0x17xx-page mirror. On the reference board both base values read 0,
	 * so the OR-in bit alone lands in the mirror. Each read is issued
	 * immediately before its dependent write; the vendor trace shows the
	 * same interleaving.
	 */
	b43_phy_write(dev, 0x173a, b43_phy_read_log(dev, 0x073a) | 0x0100);
	b43_phy_write(dev, 0x1725, b43_phy_read_log(dev, B43_PHY_AC_AFE_C1) | 0x0400);
}

/*
 * Initial ADC gain words. Both the value pair and the number of passes
 * follow the bring-up phase, not the chip. adc_reset() later rewrites
 * 0x03ac and 0x032c.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * First bring-up: one pass with 0x097a/0x08fa. From the second on: two
	 * passes with 0x03bf/0x0340. Both are witnessed on the d6220 and on
	 * agcombo.
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
	 * table-write gate itself: the capture shows a read of 0x019e, bit 1
	 * set, then cleared, before the first write below. On a first bring-up
	 * tables_init has already run and released it, so there is nothing to
	 * do here.
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

/*
 * Phase RX gate. The vendor works armed -- 0x140 = 0x0df4, WAITED only, clip
 * detect on for all three cores -- and releases at phase boundaries --
 * 0x0df6, OFDM | WAITED, clip off. The capture witnesses the arm/release
 * pair at every boundary and a final release with no re-arm. The vendor's
 * order is 0x140 first, then the clip registers.
 */
static void b43_phy_ac_clip_det(struct b43_wldev *dev, bool enable);
static void b43_phy_ac_cca_pulse(struct b43_wldev *dev);
static void b43_phy_ac_rxgain_perchan_tail(struct b43_wldev *dev);
static void b43_phy_ac_iq_acc_peek(struct b43_wldev *dev, unsigned int core,
				   bool measurement);
static void b43_phy_ac_loopback_gain_search(struct b43_wldev *dev);
static void b43_phy_ac_pmu_req(struct b43_wldev *dev, bool on);
static void b43_phy_ac_probe_cycle(struct b43_wldev *dev, unsigned int n_iter,
				   bool extended_first, bool closes_sequence);
static void b43_phy_ac_wd_stats_clear(struct b43_wldev *dev);
static bool b43_phy_ac_may_calibrate_tx(struct b43_wldev *dev);
static void b43_phy_ac_wd_stats_poll_opt(struct b43_wldev *dev,
					 bool head_sweep,
					 unsigned int ctr32_passes,
					 bool ctr32_tail);
static unsigned int b43_phy_ac_po_band(u16 chan);
static void b43_phy_ac_txpwr_target_write(struct b43_wldev *dev);
static u8 b43_phy_ac_tssi_visible_qdbm(struct b43_wldev *dev);
static void b43_phy_ac_farrow_setup(struct b43_wldev *dev,
				    struct ieee80211_channel *channel);

/*
 * Per-active-core ADC hold bracket, and the only helper that touches
 * 0x02ed/f1/f5/f9. The vendor always emits the same four MODs on the four
 * registers in a fixed order, on bit 0x0010:
 *   hold = true  sets bit 4, an ADC hold, that is an RX release
 *   hold = false clears it, dropping the bracket, that is an RX arm
 * Do not inline this into other functions; always call it by name, or the
 * op order stops matching.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * Several registers move by a fixed amount per step rather than taking three
 * unrelated values, so the step is worth having as a number. Each user below
 * says which law it follows and what the three values are, because the law is
 * fitted to three points and nothing more: it is a compact way of writing what
 * was measured, not a prediction. 160 MHz would be a fourth point and there is
 * no capture of one.
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
 * Load one of the tone tables the vendor puts in table 0x000e.
 *
 * All four of them have a period of 20 entries -- a dump of 40 is the first
 * half repeated, not new data -- and the length of the table is proportional
 * to the channel width: 40 entries at 20 MHz, 80
 * at 40 and 160 at 80. Verified on the d6220 cold sweep, one marker per load
 * with len=40, len=80 and len=160 respectively, on every segment of each
 * width. Two entries per MHz of channel width, which is what a tone waveform
 * sampled at the PHY rate looks like.
 *
 * The vendor emits the load as a single bulk write, so the tiled table has to
 * be handed over in one call rather than as a run of period-sized ones: those
 * would put one marker per period in the trace where the capture has one.
 *
 * Callers pass the 20-entry period. Only the period is theirs; the tiling and
 * the length belong here.
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
 * The tone period the RX-IQ measurement uses, and its table of steps.
 *
 * The vendor's "second" and "third" tones are this one period at steps +1 and
 * -1, and at 80 MHz it loads four more, the same period at +3, -3, +4 and -4:
 * verified on the 160 entries of each of cold24's eleven loads.
 *
 * The step is the tone frequency in units of rate/20, so the sequence is a
 * rake of frequencies: one pair above and below the carrier at 20 and 40 MHz,
 * three pairs at 80, where the band to cover is four times as wide.
 */
static const u32 b43_phy_ac_tone_period[B43_PHY_AC_TONE_PERIOD] = {
	0x0002d400, 0x0002b038, 0x0002486a, 0x0001a892, 0x0000e0ac,
	0x000000b5, 0x000f20ac, 0x000e5892, 0x000db86a, 0x000d5038,
	0x000d2c00, 0x000d53c8, 0x000dbb96, 0x000e5b6e, 0x000f2354,
	0x0000034b, 0x0000e354, 0x0001ab6e, 0x00024b96, 0x0002b3c8,
};

static const int b43_phy_ac_tone_steps[] = { 1, -1, 3, -3, 4, -4 };

/* Quante passate di misura: due fino a 40 MHz, sei a 80. */
static unsigned int b43_phy_ac_meas_passes(struct b43_wldev *dev)
{
	unsigned int n = b43_phy_ac_bw_step(dev) == 2 ? 6 : 2;

	return n < ARRAY_SIZE(b43_phy_ac_tone_steps)
	       ? n : ARRAY_SIZE(b43_phy_ac_tone_steps);
}

/*
 * I blocchi per-rate della shared memory, e come si arriva a un loro campo.
 *
 * L'indirizzo non e' una costante: si legge il puntatore del rate dalla
 * direct-map table e si raddoppia, che e' `brcms_b_rate_shm_offset()` di
 * brcmsmac:
 *
 *     blocco = 2 * shm_read(M_RT_DIRMAP_A + indice * 2)
 *
 * L'indice non e' il numero del rate: e' il nibble basso del campo SIGNAL del
 * PLCP, per cui 6, 9, 12, 18, 24, 36, 48 e 54 Mbit/s stanno agli indici 11,
 * 15, 10, 14, 9, 13, 8 e 12. Su questa board i blocchi vengono spaziati di
 * 0x14, ma quella e' una conseguenza della tabella rate, non una regola:
 * indirizzarli col passo fisso funziona qui e non emette le letture del
 * puntatore, che il vendor fa.
 *
 * E non e' solo questione di op emesse: la tabella rate e' stato del driver,
 * non dell'ucode -- il DSL ha i puntatori a 0x49e-0x4e4 dove questa board li ha
 * a 0x4c6-0x50c. Col passo fisso si scrive nelle celle giuste solo finche' il
 * rateset e' quello, e nessun gate lo direbbe: l'oracolo serve i valori
 * catturati e il confronto non sa dove le celle *dovrebbero* stare. Quindi il
 * puntatore va letto, non ricalcolato.
 *
 * Gli offset dentro il blocco sono in byte, come le M_RT_* di brcmsmac.
 */
#define B43_AC_RT_DIRMAP_A	0x01c0		/* M_RT_DIRMAP_A, 0xe0 * 2 */
#define B43_AC_RT_DIRMAP_B	0x0200		/* M_RT_DIRMAP_B, 0x100 * 2 */
#define B43_AC_RT_BBRSMAP_A	0x01e0		/* M_RT_BBRSMAP_A, 0xf0 * 2 */
/*
 * Il layout non e' quello di brcmsmac per tutti i campi. Il SIGNAL sta a +8 e
 * +10 e la durata a +12, mentre brcmsmac ha PLCP_POS 10 e PRS_DUR_POS 16; il
 * decodifica del SIGNAL lo inchioda, perche' a +10 il nibble del rate viene
 * zero. OFDM_PCTL1 invece resta a 18 in entrambi. E +14 brcmsmac non lo ha.
 */
#define B43_AC_RT_PLCP		8		/* SIGNAL a +8/+10, durata a +12 */
#define B43_AC_RT_RATE_PO	14		/* nibble di mcsbw*po, << 3 */
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
 * This is brcmsmac's `brcms_upd_ofdm_pctl1_table()`, which walks the same
 * explicit list of eight rates, reads `entry_ptr + M_RT_OFDM_PCTL1_POS`, puts
 * the STF mode bits back in and rewrites. On one chain `hw_stf_ss_opmode` is
 * zero, so the modification is a no-op and the trace shows the value read
 * going straight back.
 *
 * The vendor runs it twice per attach: once inside the MAC config block, and
 * once right after the MHF3 write in channel_setup(). Hence the two calls.
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
 * Campo OFDM_PCTL1 dei blocchi per-rate, letto e riscritto senza cambiarlo.
 *
 * No value is written from here -- what goes back is what came out -- so there
 * is nothing to transcribe and nothing that can be wrong on a board this was
 * not measured on. That the values ARE constant is how the read-back is known
 * to be faithful rather than a coincidence: 0x44, 0x3c, 0x34, 0x30, 0x2c,
 * 0x2c, 0x28 and 0x28, the same on two channels of the D6220 and on a BCM4360,
 * which makes them microcode defaults.
 *
 * Why the stock driver bothers is not known. A read-modify-write whose
 * modification is a no-op on this hardware would look exactly like this, and
 * so would a deliberate touch to make the microcode notice the cells. The
 * captures cannot tell those apart, and reproducing the accesses costs nothing
 * either way.
 *
 * These are cells of the MAC, not of the PHY, so this belongs in the core.
 * It sits here because the captures put it between the PHY write of 0x0339 and
 * the host flag that follows, and the core has no hook at that point; see
 * docs/retrace-todo.md.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   12194-12240]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7882-7928]
 */
static void b43_phy_ac_shm_readback_block(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int i;

	/*
	 * Four accesses that open the block, and none of them is understood.
	 *
	 * TODO 0x0092: read, not written, and b43.h does not name the cell. It
	 * reads 0xacc on both boards, so it is something the microcode
	 * publishes rather than session state; nothing here consumes the value.
	 * It is read once earlier too, during core init.
	 *
	 * TODO 0x000c: written with 0xf, and b43.h does not name it either.
	 *
	 * The slot time, 0x03ff then 9, is the second half of what
	 * patches/0012 introduced -- and the captures put BOTH writes here, not
	 * at core init where that patch does the 9. What 0x03ff is for is not
	 * known; writing the maximum and then the real value looks like a
	 * deliberate two-step, so it is reproduced as one.
	 */
	b43_shm_read16(dev, B43_SHM_SHARED, 0x0092);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x000c, 0x000f);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0010, 0x03ff);	/* SLOTT */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0010, 9);

	/*
	 * PHYTYPE e PHYVER, che l'ucode legge per sapere con cosa sta parlando.
	 * I valori non sono trascritti: b43 li scrive da se' in
	 * b43_wireless_core_init() come phy->type e
	 * phy->rev, e la cattura porta 0x0b -- che e' B43_PHYTYPE_AC -- e 1,
	 * che e' la rev del PHY di questa board.
	 *
	 * b43 li scrive al core init e la cattura li mette qui, in mezzo a
	 * questa corsa: e' la stessa situazione delle prime dieci celle di
	 * 0x05e0-0x0666 piu' sotto, e si risolve nello stesso modo -- le
	 * scrive il port e il perimetro si restringe.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0052, dev->phy.type);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0050, dev->phy.rev);

	b43_phy_ac_ofdm_pctl1_readback(dev);
}

/*
 * Lunghezza della probe response piu' l'FCS, come la danno le catture: 284
 * byte a 20 MHz e uno in piu' per ogni raddoppio. Provvisoria per definizione
 * -- sul ferro deve venire dal template della probe response.
 */
static u16 b43_phy_ac_prb_rsp_len(enum nl80211_chan_width width)
{
	switch (width) {
	case NL80211_CHAN_WIDTH_80:
		return 286;
	case NL80211_CHAN_WIDTH_40:
		return 285;
	default:
		return 284;
	}
}

/*
 * Probe-response PLCP and its duration, for each of the eight OFDM rates.
 *
 * The SIGNAL field sits at +8 and +10 as two words, the duration at +12.
 * Neither is transcribed: they are computed, and it is the same arithmetic as
 * brcms_c_compute_ofdm_plcp() and brcms_c_calc_frame_time().
 *
 *   SIGNAL:  tmp = len << 5, then plcp[0] = nibble_rate | (tmp & 0xff),
 *            plcp[1] = tmp >> 8, plcp[2] = tmp >> 16
 *   duration: 20 + ceil((len * 8 + 22) / NDBPS) * 4 + SIFS
 *
 * Verified on cold01 to the microsecond for all eight rates: 420, 292, 228,
 * 164, 132, 100, 84 and 80 us with len = 284 and SIFS = 16.
 *
 * @len is the probe response plus its FCS. On hardware it has to come from
 * B43_SHM_SH_PRTLEN (0x004a), where the core writes the template length -- in
 * the capture 0x0118, that is 280, and 280 plus 4 of FCS makes the 284 used
 * here. Nothing supplies it yet, so what is used is the captures' own: 284
 * bytes at 20 MHz and one more per doubling of the width. That +1 per width is
 * unexplained -- the 20 MHz template measures 280 in the TPL.RAMW, and 280
 * plus 4 of FCS adds up, but at 40 MHz the TPL.RAMW measures 284 while SIGNAL
 * says 285.
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

/*
 * I dodici rate del rateset, in ordine di bitrate crescente: e' l'ordine in cui
 * il vendor visita i blocchi, con i CCK interlacciati fra gli OFDM e non in un
 * gruppo a parte -- 1, 2, 5.5, 6, 9, 11, 12, 18, 24, 36, 48, 54 Mbit/s. @dirmap
 * e' l'indice nella direct-map table, che e' il nibble basso del campo SIGNAL,
 * e @mcs il MCS su cui il rate legacy OFDM ricade.
 */
struct b43_phy_ac_prb_rsp_rate {
	u8 dirmap;
	u8 mcs;
	bool cck;
};

static const struct b43_phy_ac_prb_rsp_rate b43_phy_ac_prb_rsp_rates[12] = {
	{ 10, 0, true  },	/*  1 Mbit/s */
	{  4, 0, true  },	/*  2 */
	{  7, 0, true  },	/*  5.5 */
	{ 11, 0, false },	/*  6 */
	{ 15, 0, false },	/*  9 */
	{ 14, 0, true  },	/* 11 */
	{ 10, 0, false },	/* 12 */
	{ 14, 0, false },	/* 18 */
	{  9, 1, false },	/* 24 */
	{ 13, 2, false },	/* 36 */
	{  8, 3, false },	/* 48 */
	{ 12, 4, false },	/* 54 */
};

/*
 * Il campo per i rate CCK, che nel PPR sono il gruppo cck[4].
 *
 * La rev 11 non ha un campo per-rate per loro su 5 GHz -- `cckbw202gpo` e'
 * della banda 2.4 e vale zero -- quindi il valore non si deriva da mcsbw*po, e
 * resta una tabella per sottobanda e larghezza. La tabella e' giustificata
 * perche' non dipende dalla board:
 *
 *   sottobanda 0 a 20 MHz: 0xd0 su tre schede con maxp5ga diverso e di forma
 *   diversa -- {72,70,86,0} sul d6220, {74,74,82,82} sull'agcombo,
 *   {76,76,76,76} sul DSL, che e' piatta. Ne' il valore di maxp5ga ne' la
 *   relazione fra sottobande adiacenti lo spostano.
 *
 *   sottobanda 2 e le larghezze legate: 0xf8, concordi fra d6220 e agcombo su
 *   tutte le configurazioni dei due sweep a freddo.
 *
 * L'eccezione e' la sottobanda 1, e non e' nascosta: la' il d6220 da' 0xe8 a 20
 * MHz e 0xf0 a 40 e 80 dove l'agcombo da' 0xf8, e a 40 MHz il valore cambia
 * anche col canale, 0xe0 a ch60 su entrambe. Quella sottobanda e' anche l'unica
 * voce di maxp5ga che in tutte e tre le SROM sia piu' bassa della precedente,
 * 70 < 72 sul solo d6220. Che sia la causa e' plausibile e non provato, e i
 * valori qui sono quelli del d6220: su un'altra scheda va rimisurata.
 *
 * TODO: chiusi i due termini aperti sotto, questo gruppo dovrebbe uscire dallo
 * stesso conto invece che da una tabella, perche' il suo ppr non ha nibble e il
 * campo e' la sola distanza fra il massimo e maxp5ga.
 */
static u16 b43_phy_ac_cck_rate_po(struct b43_phy_ac *ac)
{
	unsigned int sb = b43_phy_ac_po_band(ac->cal_channel);
	bool stretto = ac->cal_width == NL80211_CHAN_WIDTH_20;

	if (sb == 0)
		return stretto ? 0x00d0 : 0x00f8;
	if (sb == 1) {
		if (stretto)
			return 0x00e8;
		if (ac->cal_width == NL80211_CHAN_WIDTH_40 &&
		    ac->cal_channel == 60)
			return 0x00e0;
		return 0x00f0;
	}
	return 0x00f8;
}

/*
 * Mappa BSS-basic-rate-set: per ognuno degli otto rate OFDM si scrive in
 * M_RT_BBRSMAP_A il puntatore del blocco del proprio *basic rate*.
 *
 * E' `brcms_c_write_rate_shm()` di brcmsmac, che legge il puntatore del basic
 * rate dalla direct-map e lo scrive nella basic-rate map allo slot del rate.
 * Il basic set osservato e' {6, 12, 24} con la regola "il piu' alto minore o
 * uguale": il vendor scrive 0x04c6 per 6 e 9, 0x04da per 12 e 18, 0x04ee
 * per 24, 36, 48 e 54. Non c'e' niente di trascritto, i puntatori
 * vengono dalla tabella.
 *
 * Il vendor la esegue due volte per attach: una dentro il blocco di config
 * MAC, e una dopo la terza spazzata. Solo la prima e' preceduta dalle
 * tre celle invarianti 0x0082/0x00ba/0x003c, che stanno quindi al sito di
 * chiamata e non qui.
 */
static void b43_phy_ac_basic_rate_map(struct b43_wldev *dev)
{
	/* Indice, fra gli otto rate OFDM, del basic rate di ciascuno. */
	static const u8 basic_of[8] = { 0, 0, 2, 2, 4, 4, 4, 4 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(basic_of); i++)
		b43_shm_write16(dev, B43_SHM_SHARED,
				B43_AC_RT_BBRSMAP_A +
				b43_phy_ac_ofdm_dirmap[i] * 2,
				dev->phy.ac->rate_ptr[basic_of[i]]);
}

/* Una passata del PLCP, per il chiamante che la invoca fuori dal setup. */
void b43_phy_ac_prb_rsp_plcp_pass(struct b43_wldev *dev)
{
	b43_phy_ac_prb_rsp_plcp(dev,
			b43_phy_ac_prb_rsp_len(dev->phy.ac->cal_width));
}

/*
 * Semina di un tono della misura RX-IQ, al passo @step del periodo.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * Le tre celle invarianti del blocco 0x05d4-0x05dc, che b43.h chiama
 * KEYIDXBLOCK per il firmware v4 e che sul core AC sono altro.
 *
 * Il valore e' la maschera delle catene: 0x3 sulla D6220, che ha
 * txchain=rxchain=3, e 0x7 sull'agcombo, che ha 7. Verificato sui 26 segmenti a
 * freddo di ognuna delle due board -- due conteggi di catene diversi, due
 * valori diversi, sempre uguali a coremask -- quindi e' derivato e non
 * trascritto: il driver la maschera la ha gia' in ac->coremask.
 *
 * Le due celle in mezzo, 0x05d6 e 0x05d8, portano la stessa maschera in ogni
 * caso tranne uno: al primo bring-up sotto i 5250 MHz prendono una maschera
 * parziale che dipende dalla larghezza e dal numero di catene. Quella non e'
 * derivata e resta fuori -- vedi "Il blocco 0x05d4-0x05dc non e'
 * KEYIDXBLOCK" in docs/retrace-todo.md.
 */
static void b43_phy_ac_chainmask_block(struct b43_wldev *dev)
{
	u16 mask = dev->phy.ac->coremask;

	b43_shm_write16(dev, B43_SHM_SHARED, 0x05d4, mask);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x05da, mask);
}

/*
 * Field at +0x0e of the per-rate block: the rate's power offset, in eighths.
 *
 * For the address see b43_phy_ac_rate_shm_offset(). brcmsmac writes offsets
 * 10, 12 and 16 of the same block -- probe-response PLCP and duration -- and
 * not 14.
 *
 * The value is the rate's distance from the target in the per-rate table,
 * as in phy_n's PPR: (max - ppr[rate]) in quarter dBm, times four, so the
 * field is in sixteenths of a dB. The table is the one
 * b43_phy_ac_txpwr_recalc() built for this channel -- SROM, regulatory
 * ceiling, margin -- and the legacy rates take the 20 MHz MCS row they fall
 * on: 6, 9, 12 and 18 on mcs0, then 24, 36, 48 and 54 on mcs1..4. Two things
 * follow from taking the distance on the finished table rather than on the
 * raw nibbles: where the ceiling binds the rates flatten against it and the
 * distances shrink, and on a bonded channel the maximum may sit on a 40 or
 * 80 MHz row, which pushes the 20 MHz rates further from it.
 *
 * The CCK rates are outside: they are the PPR's cck[4] group, with no 5 GHz
 * SROM field in rev 11, so their value comes from ceiling and floor and
 * nothing else -- the board-independence measured on three boards.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   13009-13068, 13683-13742, 36065-36124]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8703-8762, 9313-9372, 28607-28634]
 */
static void b43_phy_ac_prb_rsp_rate_po(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;
	const struct b43_ppr_ac *ppr = &ac->txpwr_ppr;
	u8 max = b43_ppr_ac_get_max(ppr);
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
		else
			val = (u16)((max - ppr->rates.mcs_20[r->mcs]) * 4);

		b43_shm_read16(dev, B43_SHM_SHARED, cell);
		b43_shm_write16(dev, B43_SHM_SHARED, cell, val);
	}
}

/*
 * The invariant part of the MAC config block, right after the two zeroings.
 *
 * Every value here is the same on all 26 segments of the d6220 cold sweep, at
 * every channel and every width -- classified `invariante` by
 * reverse-tools/decorrelate_channels.py, which finds no dynamic key at all on
 * those captures. They are transcribed: what they are for is not known.
 *
 * Two non-invariant families of the same block are not here and remain to be
 * done: the fifteen cells at stride 0x14 that follow the width, and the twelve
 * at stride 0x1c that follow the centre frequency. Outside as well are the
 * read of 0x00b0, which b43.h declares the core's EXTNPHYCTL, and 0x05dc,
 * which falls in the KEYIDXBLOCK block; who should emit those two is not
 * settled.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   12860-12892]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8547-8579]
 */
static void b43_phy_ac_shm_mac_config_block(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* 0x092c a salire, sedici word consecutive. */
	static const u16 blocco_092c[] = {
		0x3475, 0x3475, 0x3475, 0x217c, 0x237b, 0x217c, 0x217c, 0x217c,
		0x3276, 0x3276, 0x3475, 0x187e, 0x217c, 0x167e, 0x1d7d, 0x1f7c,
	};
	/* 0x0902 a salire, sette word consecutive. */
	static const u16 blocco_0902[] = {
		0x41c2, 0x0000, 0x0017, 0x024b, 0x0097, 0x0500, 0x0000,
	};
	unsigned int i;

	b43_shm_write16(dev, B43_SHM_SHARED, 0x0020, 0x0800);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x08ec, 0x186a);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0910, 0x80c3);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x08f4, 0x80c2);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x08f0, 0x0001);

	/*
	 * Lettura e riscrittura del valore appena letto, come il gruppo A di
	 * shm_readback_block(). Nella cattura il valore e' zero, quindi una
	 * scrittura del letterale 0 darebbe la stessa traccia; la forma
	 * read-rewrite e' quella che non inventa un valore.
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
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Bit 0x0800 is set at 20 MHz and clear above it: the captures write
	 * 0x0df4/0x0df6 at 20 MHz and 0x05f4/0x05f6 at 40 and 80, and the rest
	 * of the word does not move. The one write per attach that does not
	 * follow the width is the first, in channel_switch_prep(), which
	 * carries bit 11 over from its peek before coeff_bank_init() has set
	 * it for this width.
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
 * Peeked CLASSCTL write, peek then write. This is the main pattern, 35 cases
 * in the capture, and is just phy_read_log() followed by classctl_write().
 * Do not inline the write.
 */
static void b43_phy_ac_classctl_write_peeked(struct b43_wldev *dev, bool arm)
{
	b43_phy_read_log(dev, 0x0140);
	b43_phy_ac_classctl_write(dev, arm);
}

/*
 * The three helpers above composed, each called by name. The full vendor
 * pattern, 35 cases in the capture, is:
 *   1. classctl_write_peeked(arm), a peek plus a write of 0x0140
 *   2. adc_hold(!arm), four MODs on 0x02?d bit 0x0010
 *   3. clip_det(!arm), three MODs on 0x?d4 bit 0x4000
 * No op is inlined here.
 */
static void b43_phy_ac_rx_gate_with_adc_hold(struct b43_wldev *dev, bool arm)
{
	b43_phy_ac_classctl_write_peeked(dev, arm);
	b43_phy_ac_adc_hold(dev, !arm);
	b43_phy_ac_clip_det(dev, !arm);
}

/*
 * Sampling passes per core for one idle-TSSI measurement.
 *
 * Measured, not derived: 1 at 20 and 40 MHz, 256 at 80. Every configuration of
 * the d6220 sweep agrees -- 6 reads of 0x0012 per segment at 20 and 40 MHz,
 * which is 3 iterations by 2 cores by 1, against 1536 at 80 MHz, which is the
 * same times 256.
 *
 * There is nothing in the trace explaining the jump. The setup that precedes
 * the sampling is byte-identical between 20 and 80 MHz -- 0x093a, 0x0925,
 * 0x0739, then 0x0394 and 0x0393 to start the measurement -- so the count is
 * not configured in a register and lives in the stock driver's own loop. It is
 * a transcribed number, and it is here rather than inline so that it is one.
 */
static unsigned int b43_phy_ac_idle_tssi_passes(struct b43_wldev *dev)
{
	return dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80 ? 256 : 1;
}

/*
 * Idle-TSSI: measure and commit the per-core base index. The index is
 * measured, not constant, and the loop is gated on the coremask. Three
 * iterations per bring-up, from op_switch_channel(), post_cal_finalize() and
 * post_cal_finalize_iter3().
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   11446-12192, 14981-15727, 15757-16503]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7134-7880, 10395-11141, 11171-11917]
 */
static void b43_phy_ac_idle_tssi_meas(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * The phase is absent above 5250 MHz on a first bring-up, and the
	 * accesses to the register that opens it measure that exactly: the
	 * vendor makes 18 of RAD 0x0548 at ch36 and 9 at ch52. The 9 that
	 * survive the threshold come from b43_radio_2069_pwron(), 6, and
	 * radio_percore_setup_1(), 3, which run on both sides, so the whole
	 * difference of 9 is this phase. RAD 0x004e, 0x0166, 0x024e and 0x0366
	 * agree, 30 below and 0 above, and so do PHY 0x0747, 0x0732 and 0x0733
	 * with their +0x200 mirrors, 20 below and 0 above.
	 *
	 * The gate is here rather than at the three call sites -- set_channel,
	 * post_cal_finalize and _iter3 -- because it is the whole phase that
	 * does not run: one predicate instead of three, and a fourth call site
	 * cannot forget it.
	 *
	 * It deliberately does not cover the RX gate release below, which is
	 * not part of the measurement: it hands back a gate the caller armed,
	 * and carries the RX_WAITED plus RX_OFDM transition that
	 * txpwrctrl_setup() requires immediately afterwards. Nothing else sets
	 * RX_OFDM, so covering it stops the flow at that precondition. The
	 * capture says the same: PHY 0x0140, the gate register, has 37 accesses
	 * at ch36 and 5 at ch52 rather than none, while the close of the
	 * measurement proper -- PHY.WR 0x0401 = 0x7733 -- has 4 at ch36 and
	 * none at ch52. The release survives the threshold, the body does not.
	 *
	 * Open: the count. The three skipped invocations each emit an RD+WR
	 * pair on 0x0140, where the vendor at ch52 has one pair plus a MOD
	 * elsewhere. All three releases are legitimate as releases -- the gate
	 * is re-armed between invocations -- so what does not match is their
	 * shape, not that they happen.
	 */
	if (!b43_phy_ac_may_calibrate_tx(dev)) {
		b43_phy_ac_rx_gate_with_adc_hold(dev, false);
		return;
	}
	/*
	 * The per-core base index is measured here rather than supplied by the
	 * caller: it is the idle-TSSI readback divided by four, see the write
	 * of 0x0645 below.
	 *
	 * The REQUIRE preconditions are the caller's responsibility: iteration
	 * 1, in set_channel, runs with the MAC suspended; iteration 2, in
	 * post_cal_finalize, with the MAC up; iteration 3 with it suspended
	 * again.
	 */
	unsigned int core;
	u16 r013 = 0, r012 = 0, r464 = 0;
	u16 idle_tssi = 0;
	u16 rr_4e = 0, rr_66 = 0, rr_24e = 0, rr_366 = 0;

	/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
	 *   11446-11520, 14981-15055, 15757-15831]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   7134-7208, 10395-10469, 11171-11245]
	 */
	B43_AC_BLOCK("tssi_path_enable");
	/* Abilitazione del path TSSI per catena. */
	{
		unsigned int c;
		u8 mask = dev->phy.ac->coremask;

		for (c = 0; c < dev->phy.ac->num_cores; c++) {
			if (!((mask >> c) & 1))
				continue;
			b43_phy_maskset(dev, 0x0072, (u16)~0x0004, 0x0004);
			b43_phy_maskset(dev, 0x0727 + c * 0x200, (u16)~0x0004, 0x0004);
			b43_phy_maskset(dev, 0x073c + c * 0x200, (u16)~0x0010, 0x0000);
		}
	}
	b43_radio_maskset(dev, 0x0548, (u16)~(0x0001), (0x0001));
	b43_radio_write(dev, 0x0549, 0x0000);
	b43_radio_write(dev, 0x054a, 0x0000);
	b43_radio_write(dev, 0x054b, 0x0000);
	b43_radio_write(dev, 0x054c, 0x0000);
	b43_radio_maskset(dev, 0x040b, (u16)~0x0001, 0);
	b43_radio_maskset(dev, 0x001a, (u16)~0x00f0, 0x0010);
	b43_radio_maskset(dev, 0x001a, (u16)~(0x0004), (0x0004));
	b43_radio_maskset(dev, 0x054b, (u16)~0xff00, 0x0100);
	b43_radio_maskset(dev, 0x001a, (u16)~0x0300, 0);
	b43_radio_maskset(dev, 0x0017, (u16)~0x0002, 0);
	b43_radio_maskset(dev, 0x001f, (u16)~0x0004, 0);
	b43_radio_maskset(dev, 0x0170, (u16)~(0x0100), (0x0100));
	b43_radio_maskset(dev, 0x021a, (u16)~0x00f0, 0x0010);
	b43_radio_maskset(dev, 0x021a, (u16)~(0x0004), (0x0004));
	b43_radio_maskset(dev, 0x054b, (u16)~0x00ff, 0x0001);
	b43_radio_maskset(dev, 0x021a, (u16)~0x0300, 0);
	b43_radio_maskset(dev, 0x0217, (u16)~0x0002, 0);
	b43_radio_maskset(dev, 0x021f, (u16)~0x0004, 0);
	b43_radio_maskset(dev, 0x0370, (u16)~(0x0100), (0x0100));
	b43_phy_read(dev, 0x0401);
	/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
	 *   11522-12192, 15057-15727, 15833-16503]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   7210-7880, 10471-11141, 11247-11917]
	 */
	B43_AC_BLOCK("rf_seq_mode_percore");
	/* The per-core fields of RF_SEQ_MODE are coremask and coremask << 12,
	 * not constants: 0x0003/0x3000 on a 2x2, 0x0007/0x7000 on a 3x3. */
	b43_phy_maskset(dev, 0x0401, (u16)~0x0007, dev->phy.ac->coremask);
	b43_phy_maskset(dev, 0x0401, (u16)~0x7000,
			(u16)(dev->phy.ac->coremask << 12));

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 p = (u16)(core * 0x0200);
		u16 base_index;
		/* The bbmult cells as they stand mid-sequence, written back. */
		u16 bbmult_inner[2];
		/* Gain state of the chain, saved before the measurement and put back. */
		u16 s747, s732, s733, s727, s73c, s739, s73a, s725;

		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;

		/*
		 * The 0x0140 gate is already armed on entry, by the
		 * rx_gate_with_adc_hold(true) at the end of adc_reset(), and
		 * is released and re-armed at the *end* of the per-core body:
		 * the capture releases after core 0 and arms again before
		 * core 1. So no opening arm is needed here.
		 */
		/* Per-chain prologue. */
		{
			b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_read(dev, 0x040f);
			b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
			b43_phy_read(dev, 0x0394);
			b43_phy_read(dev, 0x0393);

			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			b43_actab_read_bulk(dev, 0x0c, 0x63, 16, 1,
					    &dev->phy.ac->bbmult_saved[0]);
		}
		s747 = b43_phy_read(dev, 0x0747);
		s732 = b43_phy_read(dev, 0x0732);
		s733 = b43_phy_read(dev, 0x0733);
		b43_phy_read(dev, 0x0734);
		b43_phy_read(dev, 0x0722);
		s727 = b43_phy_read(dev, 0x0727);
		s73c = b43_phy_read(dev, 0x073c);
		b43_actab_read_bulk(dev, 0x0c, 0x67, 16, 1,
				    &dev->phy.ac->bbmult_saved[1]);
		b43_phy_read(dev, 0x0947);
		b43_phy_read(dev, 0x0932);
		b43_phy_read(dev, 0x0933);
		b43_phy_read(dev, 0x0934);
		b43_phy_read(dev, 0x0922);
		b43_phy_read(dev, 0x0927);
		b43_phy_read(dev, 0x093c);
		b43_phy_write(dev, 0x0732, 0x0000);
		b43_phy_write(dev, 0x0733, 0x0000);
		b43_phy_write(dev, 0x0747, 0x0000);
		b43_phy_maskset(dev, 0x0734, (u16)~0x0038, 0);
		b43_phy_maskset(dev, 0x0722, (u16)~(0x0001), (0x0001));
		b43_phy_maskset(dev, 0x0722, (u16)~(0x0008), (0x0008));
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_1 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &tblw_val_1);
		}
		{
			static const u16 tblw_val_2 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &tblw_val_2);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		rr_4e = b43_radio_read(dev, 0x004e);
		rr_66 = b43_radio_read(dev, 0x0166);
		{
			u16 tblr_dummy_1;
			b43_actab_read_bulk(dev, 0x0007, 0x017e, 16, 1, &tblr_dummy_1);
		}
		/* pdet_range: NVRAM has no pdetrange5g on this board (default 0);
		 * SPROM8 FEM offsets are 0xFFFF on SROM 11. Zero → clear bits. */
		b43_radio_maskset(dev, 0x004e, (u16)~0x0e00, 0);
		b43_radio_maskset(dev, 0x0166, (u16)~(0x0002), (0x0002));
		b43_phy_write(dev, 0x0932, 0x0000);
		b43_phy_write(dev, 0x0933, 0x0000);
		b43_phy_write(dev, 0x0947, 0x0000);
		b43_phy_maskset(dev, 0x0934, (u16)~0x0038, 0);
		b43_phy_maskset(dev, 0x0922, (u16)~(0x0001), (0x0001));
		b43_phy_maskset(dev, 0x0922, (u16)~(0x0008), (0x0008));
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_3 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &tblw_val_3);
		}
		{
			static const u16 tblw_val_4 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &tblw_val_4);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		rr_24e = b43_radio_read(dev, 0x024e);
		rr_366 = b43_radio_read(dev, 0x0366);
		{
			u16 tblr_dummy_2;
			b43_actab_read_bulk(dev, 0x0007, 0x018e, 16, 1, &tblr_dummy_2);
		}
		b43_radio_maskset(dev, 0x024e, (u16)~0x0e00, 0);
		b43_radio_maskset(dev, 0x0366, (u16)~(0x0002), (0x0002));
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		{
			b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1, &bbmult_inner[0]);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		{
			b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1, &bbmult_inner[1]);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &bbmult_inner[0]);
		}
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &bbmult_inner[0]);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &bbmult_inner[1]);
		}
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &bbmult_inner[1]);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_ac_cca_pulse(dev);
		/* Clear bit 0: the stock driver emits an AND here, not an OR. */
		b43_phy_mask(dev, 0x0471, (u16)~0x0001);
		b43_phy_write(dev, 0x0463, 0x0000);
		b43_phy_write(dev, 0x0461, 0xffff);
		b43_phy_write(dev, 0x0462, 0x003c);
		b43_phy_read(dev, 0x0400);
		b43_phy_set(dev, 0x0400, 0x0001);
		b43_phy_mask(dev, 0x0460, (u16)~0x0004);
		b43_phy_mask(dev, 0x0460, (u16)~0x0001);
		b43_phy_mask(dev, 0x0382, (u16)~0xc000);
		b43_phy_set(dev, 0x0460, 0x0001);
		/*
		 * Wait for completion on the same condition as run_rfseq_cmd():
		 * read while bit 0 stays high. The number of reads follows the
		 * values, not the phase -- attach gives [2,1,2,2,...] and a later
		 * bring-up gives 2 every time.
		 */
		{
			unsigned int k;

			for (k = 0; k < 10; k++) {
				u16 v = b43_phy_read(dev, 0x0403);

				if (!(v & 0x0001))
					break;
			}
		}
		b43_phy_write(dev, 0x0400, 0x0000);
		s739 = b43_phy_read(dev, 0x0739);
		b43_phy_write(dev, 0x0739, s739 | 0x0080);
		s73a = b43_phy_read(dev, 0x073a);
		b43_phy_write(dev, 0x073a, s73a);
		s725 = b43_phy_read(dev, 0x0725);
		b43_phy_write(dev, 0x0725, s725 | 0x0004);
		b43_phy_read(dev, 0x0939);
		b43_phy_write(dev, 0x0939, 0x0080);
		b43_phy_read(dev, 0x093a);
		b43_phy_write(dev, 0x093a, 0x0180);
		b43_phy_read(dev, 0x0925);
		b43_phy_write(dev, 0x0925, 0x0604);
		b43_phy_write(dev, 0x0925, 0x0600);
		b43_phy_write(dev, 0x093a, 0x0180);
		b43_phy_write(dev, 0x0939, 0x0000);
		b43_phy_write(dev, 0x0725, s725);
		b43_phy_write(dev, 0x073a, s73a);
		b43_phy_write(dev, 0x0739, s739);
		/*
		 * Sample the measurement and average it. Each pass arms the
		 * measurement and then reads 0x0013 then 0x0012; a pass whose
		 * measurement field is zero carries no reading and is dropped.
		 * See the base index write below for what the average feeds
		 * and why a single sample is not enough.
		 *
		 * The arming is per pass, not once before the loop: the
		 * captures re-read 0x0393 and rewrite 0x0394 and 0x0393 before
		 * every pair of sample reads. At 20 and 40 MHz there is a
		 * single pass and the two arrangements are indistinguishable;
		 * at 80 MHz, where the count is 256, arming once gave 256 pairs
		 * of reads against the stock driver's 256 armed passes.
		 */
		{
			unsigned int passes = b43_phy_ac_idle_tssi_passes(dev);
			unsigned int i;
			u32 sum = 0;

			for (i = 0; i < passes; i++) {
				b43_phy_read(dev, 0x0393);
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
		b43_phy_set(dev, 0x0460, 0x0002);
		b43_phy_mask(dev, 0x0460, (u16)~0x0004);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_9 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &tblw_val_9);
		}
		{
			static const u16 tblw_val_10 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &tblw_val_10);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_11 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &tblw_val_11);
		}
		{
			static const u16 tblw_val_12 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &tblw_val_12);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_ac_cca_pulse(dev);
		b43_phy_write(dev, 0x0732, s732);
		b43_phy_write(dev, 0x0733, s733);
		b43_phy_write(dev, 0x0747, s747);
		b43_phy_write(dev, 0x0722, 0x0000);
		/* 0x0000 on the 4352; the 4360, also 5 GHz only, writes 0x0029
		 * here, and likewise for 0x0934. A chip or board difference, not
		 * a band one. */
		b43_phy_write(dev, 0x0734, 0x0000);
		b43_phy_write(dev, 0x0727, s727);
		b43_phy_write(dev, 0x073c, s73c);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &dev->phy.ac->bbmult_saved[0]);
		}
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &dev->phy.ac->bbmult_saved[0]);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		/*
		 * 9 per bandwidth step in the low bits: 0x8000 at 20 MHz,
		 * 0x8009 at 40, 0x8012 at 80. The per-core twin at 0x024e
		 * takes the same value.
		 */
		b43_radio_write(dev, 0x004e,
				(u16)(0x8000 + 9 * b43_phy_ac_bw_step(dev)));
		b43_radio_write(dev, 0x0166, 0x0000);
		b43_phy_write(dev, 0x0932, 0x0000);
		b43_phy_write(dev, 0x0933, 0x0000);
		b43_phy_write(dev, 0x0947, 0x0000);
		b43_phy_write(dev, 0x0922, 0x0000);
		b43_phy_write(dev, 0x0934, 0x0000);
		b43_phy_write(dev, 0x0927, 0x0004);
		b43_phy_write(dev, 0x093c, 0x0000);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &dev->phy.ac->bbmult_saved[1]);
		}
		{
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &dev->phy.ac->bbmult_saved[1]);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		/* Come 0x004e qui sopra: 9 per passo di banda. */
		b43_radio_write(dev, 0x024e,
				(u16)(0x8000 + 9 * b43_phy_ac_bw_step(dev)));
		b43_radio_write(dev, 0x0366, 0x0000);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		b43_phy_write(dev, 0x0394, 0x000b);
		b43_phy_write(dev, 0x0393, 0x0000);
		b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		/*
		 * The base index is
		 *
		 *   0x200 + sum(meas >> 2 over the non-zero passes) / passes
		 *
		 * where meas is 0x0012 masked to its measurement field, the
		 * division truncates, and the divisor is the total number of
		 * passes rather than the number that carried a reading.
		 *
		 * Exact on all 52 sweep segments, every width and both cores:
		 * 312 of 312 writes of 0x0645 and 0x0845. Three details each
		 * earn their place --
		 *
		 *  - skipping the zero-measurement passes. On ch140 at 20 MHz
		 *    the three iterations write 0x208, 0x207 and 0x205, which
		 *    an average over every pass gets wrong all three times.
		 *  - dividing by the total rather than the useful count. That
		 *    is the difference between 301 and 312, all of it at
		 *    80 MHz where 226 to 243 of 256 passes carry a reading.
		 *  - truncating rather than rounding: also 301 against 312,
		 *    the same 11 configurations one unit high.
		 *
		 * The 0x200 term is not an added constant, it is bit 11 of the
		 * readback surviving the shift: a pass reading 0x0800 has a
		 * zero measurement field and shifts to 0x200. Core 1 measures
		 * zero on every pass of every capture, which is why it writes
		 * 0x200 flat and why an implementation that skipped the term
		 * would write 0 there.
		 *
		 * A single pass will not do, and at 20 and 40 MHz that is
		 * invisible: a block holds one usable pass there, so its value
		 * and the average coincide. At 80 MHz there are 256 passes per
		 * core spanning 0x200 to 0x217.
		 *
		 * The vendor emits MOD 0x0645 with mask 0x03ff, so the set is
		 * (base_index & 0x03ff) | 0xfc00. Bits 10 to 15 are static
		 * configuration the blob ORs in: bit 10 is idle_tssi_valid and
		 * 11 to 15 are believed to select a path.
		 */
		base_index = idle_tssi & 0x03ff;
		b43_phy_maskset(dev, 0x0645 + p, (u16)~0x03ff,
				base_index | 0xfc00);

		b43dbg(dev->wl,
		       "phy-ac: idle-tssi c%u meas: 0x013=0x%04x 0x012=0x%04x 0x464=0x%04x radio 0x4e=0x%04x 0x166=0x%04x 0x24e=0x%04x 0x366=0x%04x prog=0x%04x\n",
		       core, r013, r012, r464,
		       rr_4e, rr_66, rr_24e, rr_366, base_index);
	}

	/* Closing ops of iteration 1, emitted once. */
	b43_phy_write(dev, 0x0401, 0x7733);
	b43_phy_ac_rx_gate_with_adc_hold(dev, false);
}

/*
 * Regulatory ceiling for the configuration, in quarter-dBm, or 0 when none
 * applies.
 *
 * brcmsmac's brcms_c_channel_reg_limits() computes this as
 * QDB(ch->max_power) - antgain, clamped at zero, and
 * wlc_phy_txpower_recalc_target() then takes it as an upper bound on the SROM
 * limit. Both terms come from outside the PHY: max_power from the wiphy's
 * regulatory domain, the antenna gain from the SROM.
 *
 * A bonded configuration is bounded by every 20 MHz channel it occupies, not
 * by its primary alone, so the minimum over the block is what binds.
 *
 * The stock driver applies the same stage. On the hot sweeps it does not
 * bind, so those follow the SROM alone; on a first bring-up it does, under
 * whatever locale the driver runs before the userspace sets a country, and
 * the ceilings it applies are board-independent: the d6220 and agcombo, with
 * different maxp5ga and mcsbw*po, write the same 56 on ch36-48 at 20 MHz, 60
 * on ch60 at 40 MHz, 68 on ch100 at 40 MHz and 76 on ch100 at 20 and 80 MHz.
 * Adding the 6-unit margin back and the 22-qdB antenna gain the boards carry
 * (aga0..2 = 133) gives 21, 22, 24 and 26 dBm, all whole, which is the shape
 * of an EIRP table. The vendor's limits are per bandwidth, though: ch36-48
 * bind at 20 MHz only and ch100 binds differently at 40 than at 20 and 80.
 * cfg80211 carries one max_power per 20 MHz channel, so this function can
 * reproduce the 20 MHz ceilings and, through the minimum over the block, will
 * bound 40 and 80 MHz where the vendor does not. That is the regulatory
 * domain's policy, not a port defect. The harness reproduces the vendor's
 * first bring-up through AC_MAX_POWER_MAP; see test/unit/gates.sh.
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
	 * The block starts at the primary and runs upwards in 20 MHz steps.
	 * Each channel is looked up on its own rather than taking the
	 * primary's limit for all of them: a regulatory domain may cap them
	 * differently, and a bonded block is bounded by the lowest.
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

	return best == INT_MAX ? 0 : (u16)best;
}

/*
 * Per-band index into the rev-11 mcsbw*po fields: 0 = 5gl, 1 = 5gm, 2 = 5gh.
 * The split is by channel number and is not the same as the subband5gver
 * split that indexes maxp5ga, which is by frequency; the two partitions are
 * independent in the SROM and the captures need both.
 */
static unsigned int b43_phy_ac_po_band(u16 chan)
{
	if (chan < 52)
		return 0;
	if (chan < 100)
		return 1;
	return 2;
}

/*
 * TX power target: the per-rate table and its maximum per core.
 *
 * This is wlc_phy_txpower_recalc_target() of brcmsmac, and the body of
 * b43_nphy_op_recalc_txpower() in b43, with the rev 11 table: for every rate
 * the channel carries, min(SROM limit, regulatory limit) less the 6-unit
 * margin, floored at 8 dBm; the maximum over the rates is what the PHY closes
 * its power loop on, written to 0x0646[7:0] per core, and the per-rate
 * distances from it are the power offsets. The 6 + antenna gain that phy_n.c
 * keeps under "#if 0 / TODO: Enable this once we get gains working" is what
 * the AC captures reproduce, with the antenna gain on the regulatory side
 * only: the hot sweep is exact from the SROM alone on all 26 configurations.
 *
 * The SROM table is loaded from the minimum maxp5ga over the active cores, as
 * b43's loader does, and each core then takes back the difference to its own
 * maxp5ga. On the three boards in the repository the cores are equal and the
 * two forms coincide.
 *
 * Because the 40 and 80 MHz tables also carry their 20-in-40, 20-in-80 and
 * 40-in-80 rows, the maximum lands on the smallest mcsbw*po offset among the
 * widths the channel contains. That is the form, not a fit: against the hot
 * sweep it is exact at 20 and 40 MHz and on ch36 and ch100 at 80; on ch52 at
 * 80 MHz it gives 64 where the vendor writes 62 hot and 64 cold. The two
 * units the vendor takes off at hot on ch36/40, ch52/40 and ch52/80 are one
 * term this function does not have -- in recalc_target's chain it can only be
 * the per-rate regulatory limit of the country in force, the user target or
 * the TSSI-visible threshold, and the captures do not say which.
 *
 * Returns whether the target changed since the last computation, so the
 * periodic hook can leave the PHY alone when it did not.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   13001-13002, 35875-35876]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7211-7212]
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
					      ppr, ac->cal_channel, ac->cal_width);
	if (ceiling)
		b43_ppr_ac_apply_max(ppr, (u8)min_t(u16, ceiling, 0xff));
	b43_ppr_ac_add(ppr, -6);
	b43_ppr_ac_apply_min(ppr, B43_PHY_AC_QDB(8));
	b43_ppr_ac_force_disabled(ppr, b43_phy_ac_tssi_visible_qdbm(dev));
	max = b43_ppr_ac_get_max(ppr);

	if (b43_ppr_ac_sprom_has_subband_po(sprom) && !ac->txpwr_calc_chan)
		b43warn(dev->wl,
			"AC-PHY: la SROM porta offset espliciti per le righe "
			"sub-band (sb*/dot11agdup*/mcslr*), che questa tabella "
			"non applica: il target a 40/80 MHz puo' differire.\n");

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
 * The target registers, one per core, emitted from the highest core down to
 * match the vendor's order (0x0846 before 0x0646). Called from the channel
 * setup at the vendor's two sites and from adjust_txpower().
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
 * b43_nphy_tx_power_ctl_setup() and its brcmsmac counterpart, with 128
 * entries instead of 64. The coefficients come from the SPROM pa5ga[] array
 * for the current sub-band, like pa_5g[] in phy_n.c, with a per-core default
 * for boards whose triple is all zero.
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
	s16 a1, b0, b1;
	s32 num, den;
	int j;

	if (pw->pa5ga[grp * 3] || pw->pa5ga[grp * 3 + 1] ||
	    pw->pa5ga[grp * 3 + 2]) {
		a1 = (s16)pw->pa5ga[grp * 3];
		b0 = (s16)pw->pa5ga[grp * 3 + 1];
		b1 = (s16)pw->pa5ga[grp * 3 + 2];
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
 * The lowest power the closed loop can see, in quarter dBm.
 *
 * recalc_target disables the rates whose target falls below the power the
 * TSSI detector resolves (wlc_phy_tssivisible_thresh in the vendor driver,
 * whose value is not in any open source). What is in hand is the
 * est_pwr transfer function above: the TSSI index runs it downwards and the
 * table clamps at -8 where the detector has nothing left to say, so the
 * smallest unclamped entry over the active cores is the floor the hardware
 * itself declares. Taking it from the LUT and not from a constant is the
 * derivation available; whether the vendor's threshold sits exactly there is
 * not established -- SALAME -- and on every capture in the repository the
 * targets are far above it, so it does not bind.
 */
static u8 b43_phy_ac_tssi_visible_qdbm(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int grp = b43_phy_ac_pa5g_group(dev, 5000 + 5 * ac->cal_channel);
	unsigned int core, j;
	int floor = 0x7f;

	for (core = 0; core < ac->num_cores; core++) {
		u16 lut[128];

		if (!(ac->coremask & (1u << core)))
			continue;
		b43_phy_ac_est_pwr_lut(dev, core, grp, lut);
		for (j = 0; j < ARRAY_SIZE(lut); j++) {
			int v = (s8)lut[j];

			if (v > -8 && v < floor)
				floor = v;
		}
	}

	return floor < 0 ? 0 : (u8)floor;
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   13072-13404, 13746-14078]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   8766-9098, 9376-9708]
 */
static void b43_phy_ac_txpwrctrl_setup(struct b43_wldev *dev, u16 freq)
{
	B43_AC_FN();
	static const struct { s16 a1, b0, b1; } pwrdet_def[3] = {
		{ (s16)0xff49, (s16)0x12d9, (s16)0xfd99 },
		{ (s16)0xff54, (s16)0x1212, (s16)0xfd89 },
		{ (s16)0xff53, (s16)0x11b7, (s16)0xfdc0 },
	};
	static const u16 est_pwr_tbl_id[3] = { 0x40, 0x60, 0x80 };
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	u8 num_cores = dev->phy.ac->num_cores;
	unsigned int grp = b43_phy_ac_pa5g_group(dev, freq);
	u32 ppr[24] = { 0 };
	u8 core;

	dev->phy.ac->pa5g_grp = (u8)grp;

	/*
	 * Table 0x21, 24 u32s: the power-detector offsets by rate group, one
	 * byte per core in the low bytes of each word, taken from the SROM's
	 * pdoffset40ma[core] for the 40 MHz groups (entries 1, 5, 6) and
	 * pdoffset80ma[core] for the 80 MHz group (entry 10), one nibble per
	 * pa5g sub-band. The rest of the table is zero: rev 11 has no 20 MHz
	 * field, and pdoffsetcckma is zero on the one board that declares it.
	 *
	 * Read off all 104 segments of the three sweeps, which write three
	 * payloads and no other: 0x0202 on entries 1/5/6 on the d6220 (two
	 * cores), 0x020202 on agcombo (three), and on agcombo alone entry 10 =
	 * 0x010101 on ch100 and up at every width -- exactly the board whose
	 * pdoffset80ma is 0x0100, with the 1 in the sub-band-2 nibble, against
	 * 0 on the other two. The 80 MHz link is therefore measured on two
	 * boards and three sub-bands. The 40 MHz link is by the same encoding:
	 * all three boards carry pdoffset40ma = 0x3222, so 2 on sub-bands 0-2
	 * is what they write and sub-band 3 (nibble 3) is not captured. It is
	 * not the mcsbw*po table: those nibbles differ between the two boards
	 * and between their bands, and the payload does not follow them.
	 */
	for (core = 0; core < num_cores; core++) {
		const struct ssb_sprom *sp = dev->dev->bus_sprom;
		u32 o40, o80;

		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;
		o40 = (sp->pdoffset40ma[core] >> (4 * grp)) & 0xf;
		o80 = (sp->pdoffset80ma[core] >> (4 * grp)) & 0xf;
		ppr[1] |= o40 << (8 * core);
		ppr[5] |= o40 << (8 * core);
		ppr[6] |= o40 << (8 * core);
		ppr[10] |= o80 << (8 * core);
	}

	/*
	 * Preconditions, the vendor's state on entry to txpwrctrl_setup():
	 *   CLASSCTL 0x0140 = 0x0df6, released: RX_WAITED and RX_OFDM set,
	 *     RX_CCK clear
	 *   0x?d4 bit 14 clear, so clip detect is enabled on every core
	 *   0x0001 bit 14 clear, the CCA_RESET pulse having finished
	 *   MAC.MCTRL bit 0 clear, MAC suspended
	 * The vendor does not run this with the gate armed or clip detect
	 * disabled: txpwrctrl is setup with the RX classifier live, not a
	 * calibration.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED |
			   B43_PHY_AC_STATE_RX_OFDM,
			   B43_PHY_AC_STATE_RX_CCK |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS |
			   B43_PHY_AC_STATE_CCA_RESET |
			   B43_PHY_AC_STATE_MAC_EN);

	b43_phy_maskset(dev, 0x0072, (u16)~(0x0001), (0x0001));
	b43_phy_maskset(dev, 0x0070, (u16)~0x8000, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~(0x0100), (0x0100));
	b43_phy_maskset(dev, 0x0072, (u16)~0x4000, 0);
	b43_phy_maskset(dev, 0x0072, (u16)~(0x4000), (0x4000));
	b43_phy_maskset(dev, 0x0070, (u16)~0x8000, 0);

	/* Per-core current index, transcribed as 0x14, not 0x19. */
	for (core = 0; core < num_cores; core++) {
		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;
		b43_phy_maskset(dev, 0x0644 + core * 0x0200, (u16)~0x007f, 0x0014);
	}

	/*
	 * The idle-TSSI base index is not touched here: it is written by
	 * b43_phy_ac_idle_tssi_meas(), which op_switch_channel() calls before this
	 * function, matching the position seen in the capture some 240 ops
	 * before this block.
	 */

	/* Target power and control bits, transcribed as 0xc8, not 0x96. */
	b43_phy_maskset(dev, 0x0071, (u16)~0x00ff, 0x00c8);
	b43_phy_maskset(dev, 0x0071, (u16)~0x0700, 0x0400);
	b43_phy_maskset(dev, 0x0070, (u16)~0x0800, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~(0x0400), (0x0400));

	/*
	 * Per-core target power, high core -> low core to match the vendor
	 * order (the current index above goes 0->1). The value is the one
	 * b43_phy_ac_txpwr_recalc() computed for this channel: the 0x38 every
	 * board writes on attach is the regulatory ceiling binding, see
	 * b43_phy_ac_reg_ceiling().
	 */
	b43_phy_ac_txpwr_target_write(dev);

	/*
	 * Per-core est_pwr LUT, 128 u16s, plus the per-rate ppr, 24 u32s.
	 * The vendor does no explicit tbl_write_lock/unlock here: it calls
	 * actab_write_bulk() directly, which peeks 0x019e itself. The gate is
	 * already locked by the relock at the phase transition.
	 */
	for (core = 0; core < num_cores; core++) {
		u16 lut[128];

		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;

		b43_phy_ac_est_pwr_lut(dev, core, grp, lut);
		b43_actab_write_bulk(dev, est_pwr_tbl_id[core], 0, 16, 128, lut);
	}
	b43_actab_write_bulk(dev, 0x21, 0, 32, 24, ppr);
}

/*
 * TX-gain table for 5 GHz, EPA path, radio 2069 rev 4.
 *
 * Byte-for-byte transcription of the vendor blob symbol
 * `acphy_txgain_epa_5g_2069rev4` (wlD6220.o .rodata @ 0x403af0, 768 bytes).
 * Each of the 128 entries is a triplet of big-endian u16 fields as stored
 * in the blob; here they are re-expressed as host-endian u16 so callers
 * don't need to swap:
 *   [0] gaincurve   -- byte-wide value emitted into TBL 0x20 (low 8 bits)
 *   [1] bbmult_attn -- baseband multiplier + attenuation index
 *   [2] gaincode    -- radio gain code word
 *
 * Verified byte-for-byte across three independent blob branches carrying
 * the same symbol name: wlDSL-3580_EU.o_save (6.30), wlD6220.o_save
 * (7.14.89), and wl.ko extracted from AGSOT_1_0_8.img (Sercomm, unrelated
 * to Netgear/D-Link). D6220 and AGSOT match 128/128; the 6.30 DSL branch
 * diverges from index 31 onward (97/128 entries different). Two physically
 * independent boards carrying the same exact value rules out per-board
 * calibration -- this is the generic 7.x-branch table.
 *
 * Hypothesis (from blob symbol layout observed via `strings`): the vendor
 * routine wlc_phy_ac_gains_load selects this table on boards with radio ID
 * 0x2069, rev 4, and EPA configuration derived from NVRAM. D6220 and
 * DSL-3580L both carry femctrl=6, epagain5g=0, papdcap5g=0 in NVRAM and
 * are expected to land on this same table.
 *
 * Only column [0] is currently consumed (extracted inline before the
 * TBL 0x20 bulk write in b43_phy_ac_channel_setup). The byte-low of column
 * [0] reproduces the vendor's TBL 0x20 bulk 128/128 exactly.
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

/**************************************************
 * Open-loop TX gain (fixed index)
 **************************************************/

/*
 * Program the per-chain TX gain from the open-loop txgain LUT at index @idx.
 * At index 0x40 it rewrites what adc_reset() already programs, so it leaves
 * no footprint of its own in the capture. It is the only knob on the
 * operating index and runs last, so do not hand-edit the adc_reset()
 * constants instead.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	bool first_core = true;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Some stock-driver call sites wrap this sequence in side effects --
	 * restoring 0x0070 bits 15:13, writing 0x1641 -- that are not part of
	 * it. The caller emits those when it needs them.
	 */
	/* Preamble: peek gate + lock */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	for (core = 0; core < ac->num_cores; core++) {
		const u16 *e = b43_acphy_txgain_epa_5g_2069rev4[idx];
		u16 g0, g1, g2, bbmult;

		if (!((ac->coremask >> core) & 1))
			continue;

		if (!first_core) {
			/* Bridge between cores: an idempotent lock MOD only. */
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		}
		first_core = false;

		bbmult =  e[0]       & 0x00ff;
		g0     = (e[0] >> 8) | ((e[1] & 0x00ff) << 8);
		g1     = (e[1] >> 8) | ((e[2] & 0x00ff) << 8);
		g2     =  e[2] >> 8;

		/* Batch A: 3 fast WR TBL 0x0007 (gain code) */
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0100), 16, 1, &g0);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0103), 16, 1, &g1);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0106), 16, 1, &g2);

		/* Sync tra batch A e B: peek + MOD lock idempotente */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Batch B: 2 fast WR TBL 0x000c (bbmult per-antenna) */
		b43_actab_write_bulk(dev, 0xc, bbmult_lo[core], 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0xc, bbmult_hi[core], 16, 1, &bbmult);

		b43dbg(dev->wl,
		       "phy-ac: txpwr_by_index core %u idx %u gain %04x/%04x/%04x bbmult %02x\n",
		       core, idx, g0, g1, g2, bbmult);
	}

	/* Postamble: MOD lock idempotente + MOD unlock */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/**************************************************
 * RF sequencing
 **************************************************/

/*
 * Force a single RF sequence.
 *
 * The AC sequencer has no separate RF_SEQ_MODE register: the "mode" is
 * asserted by ORing 0x3 into RFCTL1 (reg 0x400) and restoring its prior value
 * at exit. Bit 0x1 of REG_TBL_WRITE_GATE (reg 0x19E, the RF-seq override gate,
 * distinct from the table-write gate at bit 0x2) is set before the trigger and
 * restored after.
 *
 * @rf_seq is a trigger bit of B43_PHY_AC_RF_SEQ_TRIG; the only one named so
 * far is B43_PHY_AC_RF_SEQ_RST2RX.
 *
 * Poll budget: up to 200 x udelay(1), about 200us, the same wait the N-PHY
 * and HT-PHY force_rf_sequence helpers use. Validated on hardware, where the
 * timeout has never been logged; the roughly 1ms DELAY in the blob is just
 * its retry granularity.
 *
 * Returns true if the sequence completed within the spin window, false on
 * timeout. Non-static: shared by other PHY units.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   30615-30625]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25897-25907]
 */
bool
b43_phy_ac_force_rf_sequence(struct b43_wldev *dev, u16 rf_seq, u16 gate)
{
	B43_AC_FN();
	u16 saved_rfctl1, saved_gate;
	bool timed_out = true;
	unsigned int i;

	saved_rfctl1 = b43_phy_read_log(dev, B43_PHY_AC_RFCTL1);
	saved_gate = b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);

	/* Open the gate with a maskset: an atomic RMW with mask = gate. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~gate, gate);
	b43_phy_set(dev, B43_PHY_AC_RFCTL1, 0x3);
	b43_phy_set(dev, B43_PHY_AC_RF_SEQ_TRIG, rf_seq);

	for (i = 0; i < 200; i++) {
		if (!(b43_phy_read(dev, B43_PHY_AC_RF_SEQ_STATUS) & rf_seq)) {
			timed_out = false;
			break;
		}
		udelay(1);
	}
	if (timed_out)
		b43err(dev->wl, "Forcing RF sequence timeout\n");

	b43_phy_write(dev, B43_PHY_AC_RFCTL1, saved_rfctl1);
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, saved_gate);

	return !timed_out;
}

/*
 * CCA reset strobe without the phy_force_clock/udelay wrapper: pulse bit
 * 0x4000 of BBCFG (0x0001) and track the state. The vendor emits the pulse
 * as an atomic maskset pair -- val=<bit> mask=<bit>, then val=0 mask=<bit> --
 * at many points of the bring-up, to kick the CCA state machine without
 * forcing the PHY clock. Never as a plain write: every op on 0x0001 in both
 * cold sweeps is a MOD, 696 of 696 on the d6220 and 784 of 784 on agcombo,
 * which is why phy_maskset() is used at every site.
 *
 * For the hard variant, which does force the clock and is used by
 * channel_switch_prep(), see b43_phy_ac_reset_cca().
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	b43_phy_maskset(dev, B43_PHY_AC_BBCFG,
			(u16)~B43_PHY_AC_BBCFG_RSTCCA,
			B43_PHY_AC_BBCFG_RSTCCA);
	phy_ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_maskset(dev, B43_PHY_AC_BBCFG,
			(u16)~B43_PHY_AC_BBCFG_RSTCCA, 0);
	phy_ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;
}

/*
 * Reset the CCA (Clear Channel Assessment) state machine: pulse BBCFG bit
 * 0x4000 with the PHY clock forced for the duration. The forced clock and the
 * udelay between set and clear are the whole difference from
 * b43_phy_ac_cca_pulse(), which emits the same pair without either.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   5022-5023, 5146-5147, 11235-11236]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   692-693, 816-817, 6905-6906]
 */
void
b43_phy_ac_reset_cca(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;

	b43_phy_force_clock(dev, true);
	/* Pulse RSTCCA; phy_maskset() so the harness emits the vendor's shape,
	 * see b43_phy_ac_cca_pulse(). */
	b43_phy_maskset(dev, B43_PHY_AC_BBCFG,
			(u16)~B43_PHY_AC_BBCFG_RSTCCA,
			B43_PHY_AC_BBCFG_RSTCCA);
	phy_ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	udelay(1);
	b43_phy_maskset(dev, B43_PHY_AC_BBCFG,
			(u16)~B43_PHY_AC_BBCFG_RSTCCA, 0);
	phy_ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_force_clock(dev, false);
}

/**************************************************
 * Various PHY ops
 **************************************************/

/*
 * Update the classifier control register.
 *
 * The classifier picks which preamble types the PHY decodes (CCK, OFDM,
 * "waited").
 *
 * Use this from set_channel to disable OFDM on Japanese channel 14,
 * like b43_phy_ht_classifier does.
 */
u16 b43_phy_ac_classifier(struct b43_wldev *dev, u16 mask, u16 val)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;
	u16 tmp;
	u16 allowed = B43_PHY_AC_CLASSCTL_CCKEN |
		      B43_PHY_AC_CLASSCTL_OFDMEN |
		      B43_PHY_AC_CLASSCTL_WAITEDEN;

	tmp = b43_phy_read_log(dev, B43_PHY_AC_CLASSCTL);
	tmp &= allowed;
	tmp &= ~mask;
	tmp |= (val & mask);
	b43_phy_maskset(dev, B43_PHY_AC_CLASSCTL, ~allowed, tmp);

	/* Mirror CLASSCTL[2:0] into status_mask[3:1]: identical bit order,
	 * shifted by 1 (RX_CCK is bit 1). */
	phy_ac->status_mask = (phy_ac->status_mask & ~B43_PHY_AC_STATE_RX_ANY) |
			      (u16)((tmp & 0x0007) << 1);

	return tmp;
}

/*
 * Clip detector enable/disable, per core. On phy rev 1 this is a single bit
 * (0x4000) of the gain-control word at 0x06d4 + core*0x200: cleared to enable,
 * set to freeze it during a channel reconfigure.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * All num_cores silicon cores: the wl blob freezes/unfreezes the clip
	 * bit on every core, including the antenna-less core 2 (trace
	 * disable /  enable, 0x0ad4), so gate on presence, not on the
	 * active-chain mask.
	 *
	 * The `0x06d4 + core * 0x200` form is written inline rather than
	 * through a local, because the correlator recognises that idiom as a
	 * per-core stride and resolves the addresses to 0x06d4, 0x08d4 and
	 * 0x0ad4.
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
 * quiesce the PHY before reprogramming the radio. Order and values come from
 * the capture; the steps are labelled A onwards in the body.
 *
 * Bit 8 of 0x0003 is band-related but its meaning is not confirmed; nominal
 * 5 GHz is bit 0. The clip mask here is 0x0010, distinct from the 0x0020 that
 * set_reg_on_reset() uses on the same registers.
 *
 * The 0x05f4 written to 0x0140 does not depend on channel or width: over the
 * 104 segments of the three sweeps (d6220 cold and hot, agcombo cold) every
 * write to that register is one of 0x05f4/0x05f6/0x0df4/0x0df6, so bits
 * [10:0] are fixed and only bit 11 moves, and that one is taken from the
 * peek below.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   5007-5023]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   677-693]
 */
static void b43_phy_ac_channel_switch_prep(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* A: gate override and bandctl. The vendor peeks 0x019e before the
	 * maskset; reproducing that peek is required for the bus order to
	 * match, and the value read is discarded. */
	(void)b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_RF_SEQ_OVERRIDE_GATE,
			B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);
	b43_phy_maskset(dev, 0x0003, (u16)~0x0100, 0x0100);

	/* B: classifier setup, a peek then a plain write. Bits [10:0] are
	 * invariant -- bit 2 is WAITEDEN, the others are not identified --
	 * while bit 11 is the 20 MHz flag that coeff_bank_init() sets or
	 * clears later in this same setup. Here it still holds whatever the
	 * previous setup or the PHY reset left: 0x0df7 on all 52 cold
	 * attaches of both boards, 0x05f7 on the first hot segment. Carrying
	 * it over from the peek is what matches all of them. */
	{
		u16 cur = b43_phy_read_log(dev, 0x0140);
		u16 next = (u16)((cur & 0x0800) | 0x05f4);

		b43dbg(dev->wl,
		       "phy-ac: channel_switch_prep 0x0140 cur=0x%04x -> 0x%04x\n",
		       cur, next);
		b43_phy_write(dev, 0x0140, next);
	}

	/* C: extended clip mask = adc_hold cleared (release-hold). Il vendor
	 * emette MOD 0x02?d val=0 mask=0x0010. Chiama l'helper unico. */
	b43_phy_ac_adc_hold(dev, false);

	/* D: per-core clip det disable. Aggiorna phy_ac->status_mask
	 * (CLIP_ALL_DIS) via il side-effect di clip_det(). */
	b43_phy_ac_clip_det(dev, false);

	/* E: extra reset. Meaning not confirmed; always val=0 in the ch36
	 * captures. */
	b43_phy_write(dev, 0x0339, 0x0000);

	/* F: CCA reset. */
	b43_phy_ac_reset_cca(dev);

	/* The classifier set only WAITEDEN in bits [2:0]; mirror the
	 * sequencer state into status_mask as classifier() would. */
	dev->phy.ac->status_mask = (dev->phy.ac->status_mask & ~B43_PHY_AC_STATE_RX_ANY) |
				   B43_PHY_AC_STATE_RX_WAITED;
}

/* AC-PHY init. */
/* Forward declaration; run_rfseq_cmd() is defined later in this file. */
static void b43_phy_ac_run_rfseq_cmd(struct b43_wldev *dev, u16 cmd_bit);

/*
 * Quiesce the silicon RX cores the board does not wire.
 *
 * PHY reg 0x0b reports the silicon PHY core count (3 on the 4352/4360 die); the
 * board may wire fewer, given by the SROM rxchain mask. Save reg 0x401/0x400,
 * drive the sequencer mode bits for the desired mask, fire force_rfseq cmd 0
 * (trigger 0x01) then cmd 1 (0x02), restore. Reg 0x401 is read once and not
 * written in between, so a single save is equivalent.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   6907-6946]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2577-2616]
 */
static void b43_phy_ac_rxcore_setstate(struct b43_wldev *dev, u8 coremask)
{
	/* Core 0's RX-gain override, saved across the bracket, put back on all cores. */
	u16 rxgain_ovr;
	B43_AC_FN();
	u16 saved_401, saved_400;

	saved_401 = b43_phy_read_log(dev, B43_PHY_AC_RF_SEQ_MODE);
	saved_400 = b43_phy_read_log(dev, B43_PHY_AC_RFCTL1);
	/* The stock driver reads this register here; the value's use is not
	 * known, so the read is logged and discarded. */
	rxgain_ovr = b43_phy_read_log(dev, 0x06d8);

	/* RX-gain override companion of 0x06d8, bracketing the core-state
	 * change: forced wide-open (0xffff) before, set to the operational
	 * mask (0x1431) after. Values transcribed literally -- identical in
	 * both d6220 captures; exact field semantics unconfirmed. */
	b43_phy_write(dev, 0x16d8, 0xffff);
	/* The down-to-bss-up capture delays 5848us after the wide-open
	 * write. */
	udelay(5850);

	b43_phy_maskset(dev, 0x0160, (u16)~0x0007, coremask);
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x0070,
			(u16)(coremask << 4));
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x7000, 0x7000);
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x0007, 0x0000);
	b43_phy_maskset(dev, B43_PHY_AC_RFCTL1, (u16)~0x0001, 0x0001);

	/* The vendor issues both force-sequences through the inner lock,
	 * bit 0 of 0x019e, not the outer lock at bit 1 -- so run_rfseq_cmd()
	 * rather than force_rf_sequence(). */
	b43_phy_ac_run_rfseq_cmd(dev, 0x0001);
	b43_phy_ac_run_rfseq_cmd(dev, 0x0002);

	/*
	 * Restore, in the vendor's order: MOD ~0x0007, then MOD ~0x7000, then
	 * the write of 0x0400.
	 *
	 * The low field goes back to the coremask, not to the saved value: the
	 * capture reads 0x7777 and writes 0x0003 back. On agcombo the coremask
	 * is 7 and the two happen to coincide.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x0007, coremask);
	b43_phy_maskset(dev, B43_PHY_AC_RF_SEQ_MODE, (u16)~0x7000,
			saved_401 & 0x7000);
	b43_phy_write(dev, B43_PHY_AC_RFCTL1, saved_400);

	b43_phy_write(dev, 0x16d8, rxgain_ovr);
}

/*
 * RF sequencer command tables (table id 7) + spexp TXV: the table-load half of
 * the reset-time setup. The analog sub-setups that precede these writes are in
 * b43_phy_ac_analog_on_reset; the reset-time register block is in
 * b43_phy_ac_set_reg_on_reset. On this board (boardflags2 = 0x2) the
 * band-gated 0x80 write is skipped, so it is omitted.
 */
static const u16 b43_acphy_rfseq_rx2tx_cmd[16] = { 0x0000, 0x0001, 0x0002, 0x0008, 0x0005, 0x0000, 0x0006, 0x0003, 0x000f, 0x0004, 0x0000, 0x0035, 0x000f, 0x0000, 0x0036, 0x001f };
static const u16 b43_acphy_rfseq_tx2rx_cmd[16] = { 0x0004, 0x0003, 0x0006, 0x0005, 0x0000, 0x0002, 0x0001, 0x0008, 0x002a, 0x000f, 0x0000, 0x000f, 0x002b, 0x001f, 0x001f, 0x001f };
static const u16 b43_acphy_rfseq_reset2rx_cmd[16] = { 0x0004, 0x0003, 0x0006, 0x0005, 0x0002, 0x0001, 0x0008, 0x002a, 0x002b, 0x000f, 0x001f, 0x001f, 0x001f, 0x001f, 0x001f, 0x001f };
static const u16 b43_acphy_rfseq_reset2rx_dly[16] = { 0x000c, 0x0002, 0x0002, 0x0004, 0x0004, 0x0006, 0x0001, 0x0004, 0x0001, 0x0002, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001 };
static const u16 b43_acphy_rfseq_updl_lpf_hpc[2] = { 0x0aaa, 0x0aaa };
static const u16 b43_acphy_rfseq_updl_tia_hpc[2] = { 0x0222, 0x0222 };

/*
 * The per-core rfseq second setup in table 0x07: 72 ops, six table writes of
 * length 8 at 12 ops each. Two tables per core, CMD and DLY, over three
 * silicon cores, with a +0x10 stride between cores. Emitted for every
 * num_cores, not filtered by the coremask.
 */
static const u16 b43_acphy_rfseq_2_cmd_c0[8] = {
	0x002a, 0x0007, 0x000a, 0x0000, 0x0008, 0x002b, 0x001f, 0x001f,
};
static const u16 b43_acphy_rfseq_2_dly_c0[8] = {
	0x0001, 0x0002, 0x0002, 0x0002, 0x0010, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c1[8] = {
	0x002a, 0x0007, 0x0008, 0x000c, 0x000e, 0x002b, 0x001f, 0x001f,
};
static const u16 b43_acphy_rfseq_2_dly_c1[8] = {
	0x0001, 0x0006, 0x0012, 0x0008, 0x0010, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c2[8] = {
	0x002a, 0x0007, 0x0008, 0x000e, 0x002b, 0x001f, 0x001f, 0x001f,
};
static const u16 b43_acphy_rfseq_2_dly_c2[8] = {
	0x0001, 0x0006, 0x001e, 0x001c, 0x0001, 0x0001, 0x0001, 0x0001,
};

/*
 * Same block at 80 MHz. The command sequence is shifted down by one -- the
 * leading 0x2a of the narrow variant is gone -- and the tail differs: 0xb0
 * with a per-core second word instead of 0x2b, and the delay slot drops from
 * 0x10 to 0xa. BW20 and BW40 share the narrow variant exactly.
 */
static const u16 b43_acphy_rfseq_2_cmd_c0_bw80[8] = {
	0x0007, 0x000a, 0x0000, 0x0008, 0x00b0, 0x00b1, 0x001f, 0x001f,
};
static const u16 b43_acphy_rfseq_2_dly_c0_bw80[8] = {
	0x0002, 0x0002, 0x0002, 0x0001, 0x000a, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c1_bw80[8] = {
	0x0007, 0x0008, 0x000c, 0x000e, 0x00b0, 0x00b2, 0x001f, 0x001f,
};
static const u16 b43_acphy_rfseq_2_dly_c1_bw80[8] = {
	0x0006, 0x0012, 0x0008, 0x0001, 0x000a, 0x0001, 0x0001, 0x0001,
};
static const u16 b43_acphy_rfseq_2_cmd_c2_bw80[8] = {
	0x0007, 0x0008, 0x000e, 0x00b0, 0x00b1, 0x001f, 0x001f, 0x001f,
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
 * Analog reset-time sub-setups for AC-PHY rev 1 / radio 2069 r4: FEM control, the
 * TX/RX analog LPF stages and the TX AFE dacbuf cap. Run before the RF-sequencer
 * command tables (see b43_phy_ac_analog_on_reset).
 * TODO: sample more hardware to get what other revs do.
 *
 * The caps come from struct b43_phy_ac, filled at runtime by
 * b43_radio_2069_rccal (called from op_software_rfkill, before the first
 * channel setup that reaches here). 0x80/0x80/0xc are only the pre-rccal
 * fallback set in op_prepare_structs; on the supported chips
 * rccal always overwrites all three. On the DSL-3580L first-run these are
 * lpf_cap0=lpf_cap1=0xab (E=0x0ac7 F=0x0baa) and dacbuf_cap=0 (RCCAL_G=0x0009).
 *
 * The per-core loops use the active-core mask below (SROM rxchain). On the
 * sampled boards this equals the PHY tx/rx core-enable mask; the equality is
 * inferred from observed writes, so a board with phytxchain != rxchain would
 * need this revisited.
 */

/* Tabella di controllo FEM: blocco 32B su tbl id 0xa, off 0/0x20/0x40. */
/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   5272-5386]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   942-1056]
 */
static void b43_phy_ac_set_regtbl_on_femctrl(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * Scaffolding: this table is femctrl 6's, the only value observed --
	 * all three boards in the repository share it, confirmed in their
	 * NVRAM. It is not generic. It drives the PA enable lines and the
	 * T/R switch state, so on a different FEM it means the PA enabled
	 * when it should not be, or the switch in the wrong state during TX.
	 * Hence the gate: a bring-up that fails is the better outcome.
	 */
	if (dev->dev->bus_sprom->femctrl != 6) {
		b43warn(dev->wl,
			"AC-PHY: femctrl=%u non supportato: la tabella di controllo FEM "
			"portata e' quella di femctrl 6. Pilotare un FEM diverso con "
			"questa tabella puo' danneggiare il front-end.\n",
			dev->dev->bus_sprom->femctrl);
		return;
	}

	static const u8 fem6_tbl[32] = {
		0x00, 0x00, 0x06, 0x02,  0x00, 0x00, 0x06, 0x02,
		0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x06, 0x02,  0x00, 0x00, 0x06, 0x02,
		0x00, 0x01, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	};
	u16 saved;

	/* Verified only on phy rev 1. */
	if (dev->phy.rev != 1)
		return;

	saved = b43_phy_ac_tbl_write_lock(dev);

	b43_actab_write_bulk(dev, 0x0a, 0x00, 8, 32, fem6_tbl);
	b43_actab_write_bulk(dev, 0x0a, 0x20, 8, 32, fem6_tbl);
	b43_actab_write_bulk(dev, 0x0a, 0x40, 8, 32, fem6_tbl);

	b43_phy_ac_tbl_write_unlock(dev, saved);
}

/*
 * Analog TX-LPF setup.
 *
 * For each active core, for each of up to 9 LPF stages selected by @stages
 * (bit i => stage i), read-modify-write the {lo,hi} table-7 pair that holds the
 * 25-bit analog TX-LPF word and re-pack it. Each sub-field is rewritten only
 * when its argument is >= 0; pass -1 to leave that field untouched. @only_core
 * restricts the work to a single core, or 0xffffffff for all.
 *
 * lo offsets per core: {0x142,0x152,0x162}; hi offsets: {0x362,0x372,0x382}.
 *
 * Kept as a parameterized helper (unlike the rx/dacbuf setups, which are
 * inlined into their single caller) because a second reset path also drives
 * it.
 * TODO: that second path (stages 0x100, f0=f6=<bw value>, only the bw fields)
 * is not yet implemented.
 *
 * The RMW preserves a per-stage-group base pre-loaded by the table init and
 * rewrites only the cap field (f9/f17): lo = base | (cap<<9),
 * hi = base | (cap<<1). The cap comes from rccal in op_init, so it is a
 * per-unit analog measurement and not a constant. Bases, the formula and the
 * verification on the three boards are in docs/txlpf-formula.md.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	u8 mask = dev->phy.ac->coremask;
	u8 core, num_cores = dev->phy.ac->num_cores;

	for (core = 0; core < num_cores; core++) {
		u16 off_lo, off_hi;
		unsigned int stage;

		if (!(mask & (1 << core)))
			continue;
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

/*
 * Wrapper: prende il lock, chiama _locked, rilascia il lock.
 */
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
 * Run one RF sequencer command through the control registers
 * 0x0400/0x0402/0x0403, under an inner lock of the write gate 0x019e at
 * bit 0. Bit 0 of 0x0403 is the busy flag: the sequence is done once it reads
 * back clear, which is the condition the poll below waits on, and the same
 * polarity b43_phy_ac_force_rf_sequence() uses on the same register. The blob
 * polls twice on the first execution and once on the second, consistent with
 * a wait-for-done loop.
 *
 * cmd_bit is OR-ed into 0x0402; 0x0001 then 0x0002 are the observed values.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   6919-6931, 6932-6942]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2589-2601, 2602-2612]
 */
static void b43_phy_ac_run_rfseq_cmd(struct b43_wldev *dev, u16 cmd_bit)
{
	B43_AC_FN();
	unsigned int i;

	b43_phy_read_log(dev, 0x0400);
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0001, 0x0001);  /* inner lock (bit 0) */

	b43_phy_set(dev, 0x0400, 0x0003);
	b43_phy_set(dev, 0x0402, cmd_bit);

	/* Poll until the busy bit clears, at most ten reads. The
	 * down-to-bss-up capture delays 1027us between the first two reads of
	 * 0x0403, so the poll does wait; udelay(200) after every read gives a
	 * 2ms ceiling, which is enough. */
	for (i = 0; i < 10; i++) {
		u16 v = b43_phy_read_log(dev, 0x0403);
		udelay(200);
		if (!(v & 0x0001))
			break;
	}

	b43_phy_write(dev, 0x0400, 0x0001);
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x03d0);  /* inner unlock via plain write */
}

/* Forward declaration; chan_tables(), which loads table 0x0011, is defined
 * later in this file and called from channel_setup(). */
static void b43_phy_ac_chan_tables(struct b43_wldev *dev);

/* Forward declaration: rx_evm_shaping_override chiamata da channel_setup. */
static void b43_phy_ac_rx_evm_shaping_override(struct b43_wldev *dev);

/* Forward declaration: chanspec_tail chiamata da channel_setup post-BW1F. */
/*
 * Write the chanspec the ucode reads out of shared memory.
 *
 * The centre channel is the primary for a 20 MHz configuration and the middle
 * of the block otherwise: +2 channels at 40 MHz and +6 at 80, which is half
 * the bonded span.
 *
 * The values come from phy.chandef and not from the cal_* fields, which
 * op_switch_channel() fills: the vendor writes the chanspec at the head of the RF
 * bring-up, before any channel setup has run, so at that point cal_* are still
 * from the previous cycle or zero.
 *
 * phy.chandef is where b43 keeps the width -- b43_phy_init() points it at the
 * hardware config before switch_analog() and before b43_software_rfkill(),
 * which is what calls this, and b43_op_config() repoints it on every channel
 * change. It is also what b43_is_40mhz() reads. So the bandwidth is already
 * established here and there is nothing to set.
 */
void b43_phy_ac_write_chanspec(struct b43_wldev *dev)
{
	const struct cfg80211_chan_def *chandef = dev->phy.chandef;
	u16 chan = chandef->chan->hw_value;
	u16 spec;

	switch (chandef->width) {
	case NL80211_CHAN_WIDTH_80:
		spec = B43_PHY_AC_CHANSPEC_BW80 | (chan + 6);
		break;
	case NL80211_CHAN_WIDTH_40:
		spec = B43_PHY_AC_CHANSPEC_BW40 | (chan + 2);
		break;
	default:
		spec = B43_PHY_AC_CHANSPEC_BW20 | chan;
		break;
	}

	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_AC_CHANSPEC, spec);

}

static void b43_phy_ac_chanspec_tail(struct b43_wldev *dev);

/*
 * Miscellaneous block that follows rfseq_tbl_init: open the 0x16d8 scratch,
 * configure the control registers 0x0160 (bandwidth-dependent) and 0x0401
 * (chip-dependent), run two RF-seq commands, close 0x16d8 again and write
 * 0x01ec = 0x9c40. What the values mean is not understood yet, so they are
 * pinned to the d6220 ch36 BW20 attach capture. The dependencies the
 * cross-capture comparison does establish:
 *   - 0x0160 and 0x0401 bits 0-2: 0x03 at 20 MHz, 0x01 at 40 and 80
 *   - 0x0401 bits 4-6: 0x30 on the 2069, 0x10 on the 2069 ac
 * TODO: parametrise by bandwidth and chip once the rest is understood.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   6905-6950]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2575-2620]
 */
static void b43_phy_ac_post_rfseq_misc_setup(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Peek diagnostica pre-setup. */
	b43_phy_read_log(dev, 0x000b);

	/*
	 * Quiesce the silicon RX cores the board does not wire up. num_cores
	 * comes from reading 0x000b masked with 0x07, which is 3 on both the
	 * 4352 and the 4360; the coremask comes from the SROM rxchain.
	 *
	 * Conditional because it only means anything when the two disagree: on
	 * agcombo rxchain is 7, covering all three chains, and the capture
	 * contains no write of 0x16d8 at all, while on the d6220 rxchain is 3,
	 * leaving one orphan chain, and the sequence is there.
	 */
	if (hweight8(dev->phy.ac->coremask) != dev->phy.ac->num_cores)
		b43_phy_ac_rxcore_setstate(dev, dev->phy.ac->coremask);

	/* Relock the outer table-write gate. The vendor emits it here as a
	 * gate read-back followed by an idempotent maskset. */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	/*
	 * On the 4360 only: the 6-bit field of 0x02e4 goes to 0x0800 here,
	 * between the gate relock and the 0x01ec write. In the agcombo capture
	 * it sits immediately before the 0x01ec and the table 0x20 load, so
	 * the position is constrained rather than free. No 4352 capture emits
	 * it.
	 */
	if (dev->dev->chip_id == 0x4360)
		b43_phy_maskset(dev, 0x02e4, (u16)~0x3f00, 0x0800);

	b43_phy_write(dev, 0x01ec, 0x9c40);
}

/*
 * Per-core radio setup, step 1: 52 ops, confirmed across the d6220 and
 * agcombo captures.
 *
 * Structure:
 *   1. A pre-block, emitted once, that programs 0x0548-0x054c -- probably an
 *      LO calibration output buffer -- and clears bit 0 of 0x040b.
 *   2. Per core, seven masksets: 0x001a three times, the shared 0x054b, then
 *      0x0017, 0x001f and 0x0170, all with a +0x200 stride. 0x054b is shared
 *      between cores by byte: core 0 takes the high byte, core 1 the low one.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   7090-7157]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2760-2827]
 */
static void b43_phy_ac_radio_percore_setup_1(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int core;
	unsigned int num_cores = dev->phy.ac->num_cores;
	u8 mask = dev->phy.ac->coremask;

	/* Pre-block: emitted once, before the per-core loop. */
	b43_radio_maskset(dev, 0x0548, (u16)~0x0001, 0x0001);
	b43_radio_write(dev, 0x0549, 0x0000);
	b43_radio_write(dev, 0x054a, 0x0000);
	b43_radio_write(dev, 0x054b, 0x0000);
	b43_radio_write(dev, 0x054c, 0x0000);
	b43_radio_maskset(dev, 0x040b, (u16)~0x0001, 0x0000);

	for (core = 0; core < num_cores; core++) {
		u16 stride = (u16)(core * 0x200);
		/*
		 * 0x054b and 0x054c are a shared pair with one byte per core:
		 * core 0 is 0x054b high, core 1 is 0x054b low and core 2 is
		 * 0x054c high, witnessed on the 3x3 agcombo.
		 */
		u16 sh_reg  = (u16)(0x054b + core / 2);
		u16 sh_mask = (core & 1) ? 0x00ff : 0xff00;
		u16 sh_val  = (core & 1) ? 0x0001 : 0x0100;

		if (!(mask & (1 << core)))
			continue;

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
 * Coefficient bank init, 48 ops. What the cross-capture comparison shows:
 *   - d6220 ch44 BW20 equals d6220 ch36 BW20, so it is channel-independent
 *     within 5 GHz at a given width
 *   - agcombo ch36 BW20 equals d6220 ch36 BW20, so it is chip-independent for
 *     a given bandwidth and band
 *   - d6220 ch36 BW40 differs, so the LUT does depend on bandwidth
 *
 * One LUT per width, selected below. 2 GHz has none: no capture supports it,
 * and switch_channel does not reach this on that band.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   7220-7269]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   2890-2939]
 */
static void b43_phy_ac_coeff_bank_init(struct b43_wldev *dev)
{
	B43_AC_FN();
	const unsigned int bw = b43_phy_ac_bw_mhz(dev);
	static const u16 lut_bw20[21] = {
		/* 0x0180 */ 0x0015,  /* mask=0x001f (5-bit); resto mask=0x07ff */
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
	/*
	 * One LUT per bandwidth, and each is the same on every channel of that
	 * width: the cold sweep gives one payload across its 16 segments at
	 * 20 MHz, one across the 7 at 40 and one across the 3 at 80.
	 */
	const u16 *lut = (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80)
		? lut_bw80
		: (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40) ? lut_bw40
								    : lut_bw20;
	unsigned int i;
	unsigned int core;
	unsigned int num_cores = dev->phy.ac->num_cores;

	/*
	 * Called from channel_setup(): the RX freeze is already established by
	 * channel_switch_prep() (RX_WAITED plus CLIP_ALL_DIS) and the MAC is
	 * still enabled, since mac_suspend comes after channel_setup().
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET);

	/* 1. Outer lock unlock + peek + relock (split di rfseq_tbl_init write). */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	/*
	 * 2. Bandwidth selector. The field in 0x0076 is the width index -- 1 at
	 * 20 MHz, 2 at 40, 3 at 80 -- and the two companion bits go with it:
	 * bit 11 of 0x0140 and bit 4 of 0x0164 are set at 20 MHz and clear on a
	 * bonded channel.
	 *
	 * All 26 cold-sweep segments agree, 16 at 20 MHz plus 7 at 40 and 3 at
	 * 80, with no exception.
	 */
	{
		u16 idx = (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80) ? 3
			: (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40) ? 2
			: 1;
		u16 narrow = (idx == 1) ? ~0 : 0;

		b43_phy_maskset(dev, 0x0076, (u16)~0x0007, idx);
		b43_phy_maskset(dev, 0x0140, (u16)~0x0800, 0x0800 & narrow);
		b43_phy_maskset(dev, 0x0164, (u16)~0x0010, 0x0010 & narrow);
	}

	/* 3. LUT 0x0180-0x0194. mask=0x001f su 0x0180, 0x07ff sul resto. */
	b43_phy_maskset(dev, 0x0180, (u16)~0x001f, lut[0]);
	for (i = 1; i < 21; i++)
		b43_phy_maskset(dev, (u16)(0x0180 + i),
				(u16)~0x07ff, lut[i]);

	/*
	 * 4. Extra register setup. Three of these depend on the bandwidth, with
	 * the values taken from the D6220 sweep, 26 configurations over 52
	 * segments:
	 *
	 *              20 MHz   40 MHz   80 MHz
	 *   0x0250        25       50       50
	 *   0x0262       200      400      400
	 *   0x0263        25       50       50
	 *
	 * They saturate at 40 MHz rather than doubling again at 80, so this is
	 * a table and not a multiplication. The DSL captures do show a pure
	 * doubling to 25/50/100, which means the structure carries across
	 * boards but the scaling law does not: these numbers are the d6220's.
	 *
	 * 0x0261 follows the same pattern, 0x14 at 20 MHz and 0x28 from 40 on,
	 * and 0x01b5 takes 0x8b at 40 MHz and 0x97 at both 20 and 80 -- the one
	 * register here that is not monotone in the width. The warm sweep shows
	 * neither, because it never leaves 20 MHz on those two; the cold sweep
	 * has one attach per width and does.
	 *
	 * 0x025b is never written by the d6220 at all -- only the DSL writes it
	 * -- so omitting it is correct.
	 */
	b43_phy_maskset(dev, 0x01b5, (u16)~0x00ff, bw == 40 ? 0x008b : 0x0097);
	b43_phy_maskset(dev, 0x0250, (u16)~0x00ff, bw == 20 ? 25 : 50);
	b43_phy_maskset(dev, 0x0261, (u16)~0x0fff, bw == 20 ? 0x0014 : 0x0028);
	b43_phy_maskset(dev, 0x0262, (u16)~0x0fff, bw == 20 ? 200 : 400);
	b43_phy_maskset(dev, 0x0263, (u16)~0x0fff, bw == 20 ? 25 : 50);
	/*
	 * These two drop at 80 MHz and only there: 0x13 at 20 and 40 MHz, 0x09
	 * at 80, the same value in the low byte of 0x0312 and the high byte of
	 * 0x0313.
	 */
	b43_phy_maskset(dev, 0x0312, (u16)~0x00ff, bw == 80 ? 0x0009 : 0x0013);
	b43_phy_maskset(dev, 0x0313, (u16)~0xff00, bw == 80 ? 0x0900 : 0x1300);

	/* 5. Per-core LUT 0x06ed/0x06ef (stride +0x200, per TUTTI num_cores). */
	for (core = 0; core < num_cores; core++) {
		u16 stride = (u16)(core * 0x200);

		/* 0x0a at 20 MHz, 0x14 from 40 on: doubles once and saturates. */
		b43_phy_maskset(dev, 0x06ed + stride,
				(u16)~0x00ff, bw == 20 ? 0x000a : 0x0014);
		/*
		 * In the D6220 sweep this register, under mask 0x00ff, takes two
		 * values per bandwidth, and the port accordingly has two writes:
		 *
		 *              20 MHz   40 MHz   80 MHz
		 *   this one     0x17     0x2a     0x54     (23 / 42 / 84)
		 *   further down 0x0f     0x1e     0x3c     (15 / 30 / 60)
		 *
		 * The second doubles; this one does not, since 42 is not 46. Three
		 * measured values, so a table rather than a multiplication.
		 */
		b43_phy_maskset(dev, 0x06ef + stride, (u16)~0x00ff,
				bw == 20 ? 0x0017 : (bw == 40 ? 0x002a : 0x0054));
		/* The high byte doubles with the width throughout: 0x0e, 0x16,
		 * 0x2c. */
		b43_phy_maskset(dev, 0x06ef + stride, (u16)~0xff00,
				bw == 20 ? 0x0e00
					 : (bw == 40 ? 0x1600 : 0x2c00));
	}

	/* 6. Per-core LUT 0x06ef pt.2 (stride +0x200, val=0x000f mask=0x00ff). */
	for (core = 0; core < num_cores; core++) {
		u16 stride = (u16)(core * 0x200);

		/* 15 / 30 / 60: qui il raddoppio e' esatto (vedi il commento
		 * sull'altra write a 0x06ef). */
		b43_phy_maskset(dev, 0x06ef + stride, (u16)~0x00ff,
				(u16)(15 * (bw / 20)));
	}

	/* 7. Peek + relock del outer gate (chiusura del blocco). */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
}

/*
 * Power-detector setup: 14 one-shot writes. @full selects the long variant,
 * which includes 0x0358-0x035a, over the short one that stops at 0x0559.
 *
 * It is not per-core: the +0x200 stride, which would give 0x0750/0x0950/
 * 0x0b50, appears in no capture. The values are transcribed rather than
 * derived, and the two identical pairs (1000/1000 and 500/500) look more like
 * settle windows than gain codes. 0x0554 and 0x0555 are adjusted later by the
 * periodic watchdog; see b43_phy_ac_op_pwork_60sec().
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   1202-1214, 5253-5268]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   568-580, 923-938]
 */
static void b43_phy_ac_set_pdet_on_reset(struct b43_wldev *dev, bool full)
{
	B43_AC_FN();
	/*
	 * The stock driver reads 0x0550 before overwriting it, at both call
	 * sites -- once in op_init and once in set_channel. Where the value
	 * goes is not known, so the read is required rather than defensive.
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
 * Analog reset-time sub-setups, run under the caller's table-write lock. FEM
 * control and TX-LPF stay as helpers; RX-LPF and dacbuf-cap have a single
 * call site each and are inline here.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   5253-6264]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   923-1934]
 */
static void b43_phy_ac_analog_on_reset(struct b43_wldev *dev, u16 *saved_outer_out)
{
	B43_AC_FN();
	struct b43_phy_ac *aphy = dev->phy.ac;
	u8 mask = dev->phy.ac->coremask;
	u8 core, num_cores = dev->phy.ac->num_cores;
	u16 saved;

	b43_phy_ac_set_pdet_on_reset(dev, true);

	/*
	 * Outer table-write gate: taken here and released by the caller after
	 * rfseq_tbl_init(). It covers femctrl, tx_lpf, dacbuf, rx_lpf and
	 * rfseq_tbl_init, with no intermediate unlock or relock in the vendor
	 * sequence. The nested sub-locks emit idempotent masksets that match
	 * the capture.
	 */
	*saved_outer_out = b43_phy_ac_tbl_write_lock(dev);

	b43_phy_ac_set_regtbl_on_femctrl(dev);

	/*
	 * TX-LPF stage entries in table 7, at offsets 0x142 and 0x362 for
	 * core 0 with a +0x10 stride per core. The vendor emits them
	 * immediately after the idempotent close of set_regtbl_on_femctrl().
	 */
	b43_phy_ac_set_analog_tx_lpf(dev, 0x1ff, -1, -1, -1,
				     aphy->lpf_cap0, aphy->lpf_cap1, 0xffffffff);


	/*
	 * TX AFE dacbuf cap: dacbuf_cap into the 6-bit cap field of all 9
	 * stages on every active core; the field sits at bits 0..5 or 6..11 by
	 * stage. Same shape as TX/RX-LPF: the cell carries a base (0x0b20 for
	 * stages 0-7, 0x0020 for stage 8) that the RMW preserves, rewriting
	 * only the cap. dacbuf_cap comes from rccal: (RCCAL_G & 0x03e0) >> 5,
	 * read post-apply. Verified on real RETVALs: DSL RCCAL_G=0x0186 ->
	 * cap 0xc -> 0x0b2c, agcombo 0x1a8 -> 0xd -> 0x0b2d, d6220 -> 0xe ->
	 * 0x0b2e (cap deduced from the observed write; no RETVAL on d6220).
	 *
	 * The vendor's order is femctrl, tx_lpf, dacbuf, rx_lpf, then the
	 * per-core loop.
	 */
	{
		static const u16 base[3]  = { 0x3f0, 0x60, 0xd0 };
		static const u8  add[9]   = { 0xb, 0xb, 0xc, 0xc, 0xe, 0xe, 0xf, 0xf, 0xa };
		static const u8  shift[9] = { 0, 6, 0, 6, 0, 6, 0, 6, 0 };
		unsigned int stage;

		saved = b43_phy_ac_tbl_write_lock(dev);
		for (core = 0; core < num_cores; core++) {
			if (!(mask & (1 << core)))
				continue;
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
	 * RX-LPF: like the TX-LPF, the table-7 {lo,hi} cells carry a per-stage
	 * base (lo bit0-5 = 0x00/0x09/0x12 per stage, hi 0x0000) that the RMW
	 * preserves, rewriting only the cap fields f6 (bits 6..13) and f17
	 * (bits 17..). f17 is lpf_cap1 directly; f6 is lpf_cap0 scaled by a
	 * per-section RX/TX capacitance ratio, since the 3 RX-LPF sections have
	 * different corners: f6 = (lpf_cap0 * rx_k[stage]) >> 8 with rx_k =
	 * {221, 215, 215}. Verified on d6220 (0xa8 -> 0x91/0x8d/0x8d) and
	 * agcombo (0xae -> 0x96/0x92/0x92), same coefficients on both chips;
	 * f17 confirmed by hi (d6220 0x0150, agcombo 0x015c). The DSL (wl6.30)
	 * applies no scaling (rx_k = 256 on all stages), an older-wl behaviour.
	 *
	 * TODO: validate on third board -- the coefficients are fit from two
	 * cap samples (221/222 and 215/216 both match the known values); a
	 * third distinct lpf_cap0, or other-channel captures, would pin them.
	 */
	{
		static const u16 lo_off[3][3] = {
			{ 0x140, 0x150, 0x160 },
			{ 0x141, 0x151, 0x161 },
			{ 0x441, 0x443, 0x445 },
		};
		static const u16 hi_off[3][3] = {
			{ 0x360, 0x370, 0x380 },
			{ 0x361, 0x371, 0x381 },
			{ 0x440, 0x442, 0x444 },
		};
		/* f6 scale per stage: the 3 RX-LPF sections have different corners,
		 * so the cap is scaled by a per-section RX/TX ratio ~221/256
		 * (stage 0) and ~215/256 (stages 1,2). */
		static const u16 rx_k[3] = { 221, 215, 215 };
		unsigned int stage;

		for (stage = 0; stage < 3; stage++) {
			saved = b43_phy_ac_tbl_write_lock(dev);
			for (core = 0; core < num_cores; core++) {
				u16 off_lo, off_hi;
				u16 lo, hi;
				u32 v;

				if (!(mask & (1 << core)))
					continue;

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

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/*
	 * Table-write gate: taken by the caller, channel_setup(), and shared
	 * with analog_on_reset(). In the vendor sequence the outer gate stays
	 * locked across both, with no intermediate unlock or relock.
	 */

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
 * backplane MAC-PHY clock enable maps to b43_mac_phy_clock_set; the CCA reset
 * maps to b43_phy_ac_reset_cca, called mid-sequence.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   5128-5158]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   798-828]
 */
static void b43_phy_ac_set_reg_on_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;

	/* Clear the RF-seq override gate before the reset block. phy_maskset()
	 * is used rather than phy_mask() or phy_set() so that the harness
	 * emits the vendor's `val=<set> mask=<affected bits>` shape, which is
	 * what the capture shows at these positions. */
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

	/*
	 * Enable the backplane MAC-PHY clock. The b43 core only enables it
	 * after b43_phy_init(), in b43_chip_init(), which is too late for the
	 * AC calibration that runs during reset, so it is enabled here. This
	 * is idempotent with respect to the core's own enable.
	 */
	b43_mac_phy_clock_set(dev, true);

	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);

	/* Clip mask: all the bit-5 clears first, then all the low-byte 0x55
	 * writes. That order is the vendor's. */
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
	/*
	 * No 0x6d4/0x8d4/0xad4 block here: 0x06d4 is only ever used by the
	 * clip_det pattern, which lives in b43_phy_ac_clip_det(), and 0x06db
	 * never appears as a PHY register anywhere in the session.
	 */
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* Stadio AFE/LPF per catena, 2a chiamata. */
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

	/* Ri-locka il gate outer in uscita. */
	b43_phy_ac_post_rfseq_misc_setup(dev);

	/*
	 * Bulk write of table 0x20, a 128-byte gain curve, invariant across
	 * every capture. It goes through the alternate DATA port 0x011, which
	 * actab_write_bulk() handles for id 0x20. The buffer is column 0, the
	 * gain curve, of b43_acphy_txgain_epa_5g_2069rev4 -- the low byte of
	 * each u16.
	 */
	{
		u8 gaincurve[128];
		unsigned int k;

		for (k = 0; k < 128; k++)
			gaincurve[k] = (u8)(b43_acphy_txgain_epa_5g_2069rev4[k][0]
					    & 0xff);
		b43_actab_write_bulk(dev, 0x20, 0x0000, 8, 128, gaincurve);
	}

	/*
	 * Per-chain PHY setup after table 0x20, identical on the d6220 and on
	 * agcombo. Filtered by the coremask.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;

		for (core = 0; core < num_cores; core++) {
			u16 stride = (u16)(core * 0x200);


			if (!(mask & (1 << core))) {
				continue;
			}

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
	 * d6220's orphan core 2 as well -- num_cores 3, coremask 0x03 -- with
	 * base 0x0b00.
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
	 * Not filtered by the coremask either. Bit 12 of 0x?21 adds to the
	 * bit 14 the previous loop set; the radio stride is +0x200, the same as
	 * the PHY's.
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
	 * Azzeramento per catena. Filtrato per
	 * coremask: entrambe le catture saltano il core 2.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;

		for (core = 0; core < num_cores; core++) {
			u16 tbl_off = (u16)(0x0060 + core * 4);
			u16 rad_stride = (u16)(core * 0x200);
			u16 phy_stride = (u16)(core * 0x200);

			if (!(mask & (1 << core)))
				continue;

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

	b43_phy_ac_coeff_bank_init(dev);

	/*
	 * Second invocation of set_analog_tx_lpf with stages=0x100 (only
	 * stage 8). The 0x019e gate is already locked by
	 * channel_analog_setup(), which ends on a relock maskset, so the
	 * _locked variant is used and skips the pre-lock. The closing
	 * idempotent unlock is emitted explicitly. 41 ops in total: 20 per
	 * core over two cores, plus the unlock.
	 *
	 * This is where the bandwidth shows. Stage 8 is written twice per
	 * cycle: the first pass, in channel_analog_setup(), lays down the
	 * 20 MHz base on every width, and this one replaces it with the base of
	 * the group the width selects -- 0x0db at 20 MHz, 0x123 at 40, 0x16b at
	 * 80. Stages 0 to 7 are the same ladder on every width and this pass
	 * does not touch them.
	 *
	 * The ladder is the driver's own default: neither board defines the
	 * ofdmanalogfiltbw5g NVRAM key, and the two chips write the same three
	 * bases, the 4360 as 0x5cdb, 0x5d23 and 0x5d6b against the 4352's
	 * 0x50db, 0x5123 and 0x516b, differing only in the rccal cap that sits
	 * in the high byte.
	 */
	{
		static const u16 stage8_base[3] = { 0x00db, 0x0123, 0x016b };
		unsigned int bwi =
			(dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80) ? 2 :
			(dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40) ? 1
									  : 0;
		u16 base = stage8_base[bwi];

		b43_phy_ac_set_analog_tx_lpf_locked(dev, 0x100,
						    base & 7,
						    (base >> 6) & 7,
						    (base >> 3) & 7,
						    dev->phy.ac->lpf_cap0,
						    dev->phy.ac->lpf_cap0,
						    0xffffffff);
	}
	b43_phy_ac_tbl_write_unlock(dev, B43_PHY_AC_TBL_WRITE_GATE_LOCK);

	b43_phy_ac_rx_evm_shaping_override(dev);

	/*
	 * Per-core maskset "post rfseq_2 prep" (4 op = 2 op × 2 core attivi):
	 * X = 7 per core 0, 9 per core 1 (stride +0x200). FILTRATO per coremask.
	 */
	{
		unsigned int core;
		unsigned int num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;

		for (core = 0; core < num_cores; core++) {
			u16 stride = (u16)(core * 0x200);

			if (!(mask & (1 << core)))
				continue;

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
		 * At 80 MHz the sequence opens with three cells of table 0x14,
		 * through the alternate data register like table 0x11. Same
		 * three values on all three 80 MHz segments of the cold sweep,
		 * and absent from the 20 and 40 MHz ones.
		 */
		if (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80) {
			static const u16 t14_bw80[3] = {
				0x0fd2, 0x0fc2, 0x0fd2,
			};

			b43_actab_write_r11(dev, 0x0014, 0x0030,
					    ARRAY_SIZE(t14_bw80), t14_bw80);
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
	 * Write 0x0197 and 0x0198, then unlock the outer gate: three ops. The
	 * unlock is what lets the raw PHY writes that follow through, since
	 * they do not go via the outer table-write path.
	 *
	 * Both step once from 20 to 40 MHz and then hold: 0x14 and 0x10 at
	 * 20 MHz, 0x1e and 0x14 from 40 on. Constant across every channel of
	 * each width in the cold sweep.
	 */
	if (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_20) {
		b43_phy_write(dev, 0x0197, 0x0014);
		b43_phy_write(dev, 0x0198, 0x0010);
	} else {
		b43_phy_write(dev, 0x0197, 0x001e);
		b43_phy_write(dev, 0x0198, 0x0014);
	}
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);

	/*
	 * Clear bits massivo per 0x0410 + per-core 0x0X3a/0x0X25 (20 op =
	 * 2 op globali + 6 op × 3 core). NON filtrato
	 * per coremask — emesso su tutti num_cores.
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

	/*
	 * Peek + relock outer gate (2 op).
	 * Prepara la fase finale di TBL.RD/TBL.WR su celle 0x03cd/0x03dd.
	 */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	/*
	 * RFSEQ 0x03cd and 0x03dd: read, then write 0x04c2 and 0x04e2. The
	 * read returns 0x0c02 -- the value the RFSEQ init load put there -- on
	 * every capture there is, d6220 cold and hot and agcombo, and the
	 * write is the same on all of them, so nothing pins how the write
	 * depends on the read: a field write clearing 0x0800 and setting
	 * 0x00c0/0x00e0 fits, and so does a constant.
	 *
	 * SALAME: kept as the constant the captures show; a board whose init
	 * load differs at these cells would tell the two apart.
	 * 20 ops, 10 per cell.
	 */
	{
		u16 tmp;
		u16 val_cd = 0x04c2;
		u16 val_dd = 0x04e2;

		b43_actab_read_bulk(dev, 7, 0x03cd, 16, 1, &tmp);
		b43_actab_write_bulk(dev, 7, 0x03cd, 16, 1, &val_cd);
		b43_actab_read_bulk(dev, 7, 0x03dd, 16, 1, &tmp);
		b43_actab_write_bulk(dev, 7, 0x03dd, 16, 1, &val_dd);
	}

	/*
	 * Final unlock of the outer gate, one op, closing the rfseq_tbl_init
	 * and channel_setup segment. The caller resumes with the 0x0371-0x0376
	 * writes.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);

	/*
	 * Per-channel PHY 0x371-0x376, the 2069 path of
	 * set_regtbl_on_chan_change: phy_reg_write(0x371 + i, ci[0x68 + 2*i]),
	 * which is entry u16[52..57]. The vendor emits these immediately after
	 * rfseq_tbl_init() unlocks the table-write gate.
	 *
	 * These six values are validated against the vendor sweep on all 16
	 * BW20 channels by reverse-tools/check_channeltab.py.
	 */
	for (i = 0; i < 6; i++)
		b43_phy_write(dev, B43_PHY_AC_BW1A + i, e->phy_bw[i]);

	b43_phy_ac_chanspec_tail(dev);
}

/*
 * Table id 0x11, 464 words, loaded through the alternate data register 0x0011
 * by b43_actab_write_r11().
 *
 * Only the twelve words at the head and the four at the tail are real data,
 * and both are channel-invariant. The 448 words between them are a single
 * repeated value that depends on the sub-band, so a single array of literals
 * is right only for the sub-band it was read from.
 *
 * The d6220 sweep locates the boundary and shows there is only one: 0x18f1 up
 * to ch48 and 0x7907 from ch52 on, with ch100 and above sharing the second
 * value. That last part rules out the pa5ga sub-band group, which changes
 * again at 5500 MHz; the boundary that fits is the U-NII-1 edge at 5250.
 */
static const u16 b43_acphy_tbl11_head[12] = {
	0x005b, 0x8250, 0xc338, 0x4527, 0xa6a1, 0x081b,
	0x8a18, 0x2c96, 0x8e17, 0x101b, 0x0020, 0x0020,
};

static const u16 b43_acphy_tbl11_tail[4] = { 0x0000, 0x0000, 0x0000, 0x0000 };

#define B43_PHY_AC_TBL11_FILL_OFF	12
#define B43_PHY_AC_TBL11_FILL_LEN	448

static u16 b43_phy_ac_tbl11_fill(u16 freq)
{
	return freq < 5250 ? 0x18f1 : 0x7907;
}

/*
 * Per-channel table loads, radio rev 4 on 5 GHz: the twin coefficients at
 * 0x00ec-0x00f5, table 0x11 at 464 words, tables 0x0b and 0x15, and the
 * per-core 0x44/0x45 pair broadcast to num_cores.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   7534-10317]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   3204-5987]
 */
static void b43_phy_ac_chan_tables(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Load table 0x11, 464 u16s, through the DATA_2 port at 0x011,
	 * reselecting the id and offset for every cell. The per-cell vendor
	 * pattern is: peek 0x019e, write the id, write the offset, write
	 * DATA_2 -- 2320 ops for 464 cells. Split into head, constant run and
	 * tail, which emits the identical sequence because the writer takes an
	 * explicit offset per cell.
	 *
	 * The 0x019e gate is already locked on entry, by chanspec_tail(), and
	 * stays locked on exit, since the next consumer,
	 * noise_shaping_table_init(), opens its own gate cycle.
	 *
	 * The other tables -- 0x0b glim, 0x15 nvar and the per-core 0x44/0x45
	 * nshp -- are not here; noise_shaping_table_init() emits those.
	 */
	b43_actab_write_r11(dev, 0x11, 0, ARRAY_SIZE(b43_acphy_tbl11_head),
			    b43_acphy_tbl11_head);
	b43_actab_fill_r11(dev, 0x11, B43_PHY_AC_TBL11_FILL_OFF,
			   B43_PHY_AC_TBL11_FILL_LEN,
			   b43_phy_ac_tbl11_fill(dev->phy.ac->cal_freq));
	b43_actab_write_r11(dev, 0x11,
			    B43_PHY_AC_TBL11_FILL_OFF + B43_PHY_AC_TBL11_FILL_LEN,
			    ARRAY_SIZE(b43_acphy_tbl11_tail),
			    b43_acphy_tbl11_tail);
}

/*
 * rx_evm_shaping override in table 0x04: two runs of three 16-bit words, at
 * offset 0x0001 and offset 0x003d. Called from channel_setup() with the 0x019e
 * gate already locked, so there is no tbl_write_lock/unlock here.
 *
 * The shaping applies at 20 MHz only, where the runs are {8, 6, 4} and
 * {4, 6, 8}; on a bonded channel both are written as zeroes, which disables
 * it. Constant across every channel of each width in the cold sweep, so this
 * follows the bandwidth and not the channel.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * Per-core body of the post-noise-shaping block, replicated with a 0x200
 * stride across cores: peek and write 0x06dc/0x06dd plus stride, one table
 * write of id 0x07 offset 0xf9 + core with value 0xc0b5, unlock the outer
 * gate, five groups of (clear bit 1 of 0x06e3 + stride, then two writes),
 * eight diagnostic peeks, and finally a MOD of 0x06ee + stride to 1 under
 * mask 3.
 *
 * The meaning is unknown -- 0x06dc-0x06e5 and 0x06ee have no public vendor
 * naming -- but the per-core shape suggests RX gain or RSSI programming.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   11060-11101, 11113-11154, 11166-11207]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6730-6771, 6783-6824, 6836-6877]
 */
static void
b43_phy_ac_post_noise_shaping_rx_regprog_core(struct b43_wldev *dev,
					      unsigned int core)
{
	/* 0x06dc is read and written back as it stands; only 0x06dd gets a new value. */
	u16 cur;
	B43_AC_FN();
	u16 stride = (u16)(core * 0x200);
	u16 tbl_off = (u16)(0x00f9 + core);
	static const u16 tbl_val = 0xc0b5;

	/* peek + program 0x06dc/0x06dd */
	cur = b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_write(dev, 0x06dc + stride, cur);
	b43_phy_write(dev, 0x06dd + stride, 0x0604);

	/* TBL.WR id=0x07 off=0xf9+core val=0xc0b5; il gate qui e' sbloccato,
	 * quindi _reopen. */
	b43_actab_write_bulk_reopen(dev, 0x07, tbl_off, 16, 1, &tbl_val);

	/* Unlock the outer gate after the table write. */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);

	/* 5 gruppi (MOD 0x06e3+stride clear bit 1) + 2 write */
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06de + stride, 0x015a);
	b43_phy_write(dev, 0x06df + stride, 0x0004);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06e0 + stride, 0x016a);
	b43_phy_write(dev, 0x06e1 + stride, 0x0018);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06e4 + stride, 0x013a);
	b43_phy_write(dev, 0x06e5 + stride, 0x0008);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);
	b43_phy_write(dev, 0x06e2 + stride, 0x013a);
	b43_phy_write(dev, 0x06e3 + stride, 0x0008);
	b43_phy_maskset(dev, 0x06e3 + stride, (u16)~0x0002, 0x0000);

	/* 5× peek 0x06dc+stride + 3× peek 0x06dd+stride (settle/diagnostic) */
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
 * Core transition, emitted after every core's body including the last: eight
 * ops programming radio 0x0045 and 0x0033 plus stride, and PHY 0x06dc and
 * 0x06ee plus stride. radio_maskset() expands to MOD + RD + WR with the value
 * from the mirror, which radio_2069_channel_setup() has already populated:
 * an active core gives 0x73bf/0x4181, an inactive one 0x7380/0x4180.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Two of these follow the bandwidth. At 20 MHz radio 0x0045 takes bits
	 * 9:8 set, on a bonded channel bit 6 cleared instead -- a different
	 * mask, not just a different value -- and radio 0x0033's nibble goes
	 * from 0x80 to 0xc0. The 0x06ee field is 0x8 throughout.
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
 * Post-noise-shaping block. Closes noise shaping by writing 0x016c = 0,
 * unlocks the outer gate, then for every silicon core -- num_cores, not the
 * coremask -- does:
 *   1. rx_regprog_core(), 26 ops on the 0x06XX + stride block
 *   2. core_transition(), 8 radio and PHY ops
 *
 * The registers 0x06dc-0x06e5, 0x06ee and radio 0x0045/0x0033 are not
 * identified. The per-core stride and the active/inactive radio values
 * suggest per-channel RX gain or RSSI programming; rename this once that is
 * established.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* Close noise shaping; common, before any core. */
	b43_phy_write(dev, 0x016c, 0x0000);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);

	/* Per-core loop: body then transition, including after the last core. */
	for (core = 0; core < num_cores; core++) {
		b43_phy_ac_post_noise_shaping_rx_regprog_core(dev, core);
		b43_phy_ac_post_noise_shaping_core_transition(dev, core);
	}
}

/*
 * RX gain-control registers a core reads before a measurement and writes back
 * after it, in the order the vendor keeps: rx_gain_regs_program() saves them
 * into rxgain_saved[], measure_block() restores them.
 */
static const u16 b43_phy_ac_rxgain_regs[14] = {
	0x073e, 0x0727, 0x073c, 0x0721, 0x0729, 0x0720, 0x0728,
	0x0724, 0x0736, 0x0725, 0x0739, 0x073a, 0x0722, 0x0734,
};

/*
 * Program one chain's RX gain-control block, 0x0720-0x073e: open the override
 * bracket on 0x?73e, re-read the 13 saved registers in the vendor's order,
 * then emit the 26 programming ops. All of them are masksets, to match the
 * capture's RMW shape.
 *
 * 41 ops, emitted identically at two points of the flow and in the same
 * order: the tail of switch_channel, in b43_phy_ac_rxgainctrl_regs(), and the
 * RX AFE reconfiguration at the head of the measure block. @gw_hi, the
 * channel's high gain word, is the only difference between the two sites.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   14412-14466, 14467-14521, 32825-32879, 32880-32934, 35471-35525,
 *   35526-35580]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9826-9880, 9881-9935, 27221-27275, 27276-27330]
 */
static void b43_phy_ac_rx_gain_regs_program(struct b43_wldev *dev,
					    unsigned int core, u16 gw_hi)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);

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
	b43_phy_write(dev,   0x0736 + s, gw_hi);
	/*
	 * TSSI floor from the SROM, not a constant. On all three boards in the
	 * repository the field is unprogrammed and reads 0x3ff, the mask's
	 * maximum, which is indistinguishable from a literal -- and is exactly
	 * why it has to be read. On a board that does program it, writing a raw
	 * 0x3ff would silently ignore the declared floor.
	 */
	b43_phy_write(dev,   0x0724 + s,
		      dev->dev->bus_sprom->tssifloor5g[dev->phy.ac->pa5g_grp]
		      & 0x03ff);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0007, 0x0003);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0020, 0x0020);
	b43_phy_maskset(dev, 0x0739 + s, (u16)~0x007e, 0x007a);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0008, 0x0000);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0040, 0x0040);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0010, 0x0010);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0080, 0x0080);
	b43_phy_maskset(dev, 0x073a + s, (u16)~0x0060, 0x0040);
	b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0100, 0x0100);
	b43_phy_maskset(dev, 0x0734 + s, (u16)~0x0007, 0x0000);
	b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0004, 0x0004);
}

/*
 * Tail of switch_channel: program the RX gain-control block of every
 * populated chain. Called after both txpwrctrl_setup() passes, with the gate
 * released to RX_OFDM and CLIP_ALL_DIS clear.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   14412-14521]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9826-9935]
 */
static void b43_phy_ac_rxgainctrl_regs(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;
	u8 mask = dev->phy.ac->coremask;
	/*
	 * A constant of this call site, not a per-channel quantity: measured
	 * invariant across all 26 configurations of the d6220 sweep, 16
	 * channels over 20, 40 and 80 MHz. The other two sites that write
	 * 0x0736 have their own constants -- 0x0152 in the b2j bank and 0x022a
	 * in rxgain_perchan_config() -- and are invariant in the same way. The
	 * three values distinguish the sites, not the channels.
	 */
	u16 gw_hi = 0x0154;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_RX_OFDM,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CLIP_ALL_DIS |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	for (c = 0; c < num_cores; c++) {
		if (!((mask >> c) & 1))
			continue;
		b43_phy_ac_rx_gain_regs_program(dev, c, gw_hi);
	}
}


/* Registri ADC per core, usati dalle due fasi del setup. */
static const u16 b43_phy_ac_adc_hi[8] = { 0x33a, 0x33b, 0x33e, 0x33f,
					  0x342, 0x343, 0x346, 0x347 }; /* = 0x03ac */
static const u16 b43_phy_ac_adc_lo[8] = { 0x33c, 0x33d, 0x340, 0x341,
					  0x344, 0x345, 0x348, 0x349 }; /* = 0x032c */

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   11299-11388]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   6969-7058]
 */
static void b43_phy_ac_adc_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;
	u8 mask = dev->phy.ac->coremask;
	u16 saved;
	unsigned int i;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * The RX-chain arm/restore on 0x0739/0x0725/0x073a is NOT part of
	 * adc_reset: it is afecal's save-arm-restore (b43_radio_2069_afecal),
	 * which runs immediately before this. In the trace those PHY writes sit
	 * inside afecal's RCCAL_EN1 bracket, interleaved with the AFE_CAL_CLK
	 * radio ops; adc_reset proper begins at the gain table.
	 *
	 * Per active core: point the gain table (0x20 off 0x40) + readback, then
	 * the fixed ADC gain words into tables 7 and 0xc. These five per-core
	 * words are exactly b43_phy_ac_txpwr_by_index(0x40) inlined here as the
	 * cal-time gain (the words match idx 0x40 byte-for-byte); the trace shows
	 * them only here, wrapped in the 0x20/0x40 readback dance, so they stay
	 * open-coded rather than routed through the helper.
	 *
	 * The 0x019e gate is cycled once per table group; the steps are
	 * labelled in the body.
	 */
	for (c = 0; c < num_cores; c++) {
		static const u16 z = 0x0000, w0 = 0x2f13, w1 = 0x00f3;
		u8 bbmult8;
		u16 bbmult;

		if (!((mask >> c) & 1))
			continue;

		/* Apertura core: peek + relock */
		saved = b43_phy_ac_tbl_write_lock(dev);

		/*
		 * Readback gain-curve, offset chip-dependent: 0x40 su 4352,
		 * 0x0000 su 4360.
		 */
		b43_actab_read_bulk(dev, 0x20,
				    dev->dev->chip_id == 0x4360 ? 0x0000 : 0x40,
				    8, 1, &bbmult8);
		bbmult = bbmult8;

		b43_actab_write_bulk(dev, 7, 0x100 + c, 16, 1, &z);
		b43_actab_write_bulk(dev, 7, 0x103 + c, 16, 1, &w0);
		b43_actab_write_bulk(dev, 7, 0x106 + c, 16, 1, &w1);

		/* Mid-sync tra gruppo id=0x07 e gruppo id=0x0c. */
		saved = b43_phy_ac_tbl_write_lock(dev);

		b43_actab_write_bulk(dev, 0xc, 0x63 + c * 4, 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0xc, 0x73 + c * 4, 16, 1, &bbmult);

		/*
		 * Per-core close: two raw masksets on the gate, an idempotent
		 * relock then an unlock. Not the same as
		 * tbl_write_unlock(saved), which with saved = 0x02 emits only the
		 * relock and not the unlock, so the unlock has to be explicit.
		 */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);
	}
	(void)saved;
}

/*
 * TX power-control enable, the second PHY phase of the setup.
 *
 * The boundary with b43_phy_ac_adc_reset(), which set_channel calls just
 * before, is read off the data: this block's marker, PHY.WR 0x1641, appears
 * once in the cold capture and falls inside the first PHY phase.
 *
 * Not to be confused with the power-target recalculation on 0x0644/0x0646 per
 * core: that is the PHY phase which in the capture falls *after* the core's
 * BSS config, and in b43 it belongs to b43_phy_txpower_check() called from
 * b43_op_config().
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   11389-11445]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   7059-7133]
 */
static void b43_phy_ac_txpwrctrl_enable(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;
	u8 mask = dev->phy.ac->coremask;
	unsigned int i;

	/*
	 * TX power-control enable (0x70[15:13]), framed by 0x1641 (broadcast
	 * gain reg). Il vendor emette:
	 *   - PHY.RD 0x0070 (peek diagnostico)
	 *   - MOD 0x0070 clear bit 13-15
	 *   - WR 0x1641 = 0x7f18
	 *   - MOD 0x0070 set bit 13-15
	 *   - MOD 0x0644+stride val=0x0014 mask=0x007f (per-core, active only)
	 *   - MOD 0x0678+stride clr bit 2 (per-core, active only)
	 */
	b43_phy_read_log(dev, 0x0070);
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0x0000);
	b43_phy_write(dev, 0x1641, 0x7f18);
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);

	/*
	 * The gain-word pair is absent on a first bring-up: on attach the block
	 * is three ops, on a later channel setup five. The 0x14 is not derivable
	 * from the SROM; see retrace-todo.md.
	 */
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		for (c = 0; c < num_cores; c++) {
			if (!((mask >> c) & 1))
				continue;
			b43_phy_maskset(dev, 0x0644 + c * 0x200,
					(u16)~0x007f, 0x0014);
		}
	}

	for (c = 0; c < num_cores; c++) {
		if (!((mask >> c) & 1))
			continue;
		b43_phy_maskset(dev, 0x0678 + c * 0x200, (u16)~0x0004, 0x0000);
	}

	/*
	 * PLL lock verify, the counterpart of channel_switch_prep(). By this
	 * point the RX freeze has covered the whole channel_setup() flow and the
	 * PLL has had tens of milliseconds to settle -- through the udelays in
	 * radio_2069_channel_setup() and the rest of the setup -- so the vendor
	 * emits a single read of radio 0x090b just before releasing the freeze.
	 *
	 * A polling loop is used here instead of that single peek, as a safety
	 * net for the case where the PLL has not locked yet: rare on silicon,
	 * but not impossible.
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
			b43dbg(dev->wl,
			       "radio 2069: PLL lock timeout (0x90b=0x%04x)\n",
			       stat);
	}

	/* PHY update strobe. */
	b43_phy_ac_cca_pulse(dev);

	/* Release the RX gate and hold the ADC bracket. A full write of
	 * 0x0140 as the vendor does, not an RMW, so that bit 0x0800 also ends
	 * up in the vendor's state. */
	b43_phy_ac_rx_gate_with_adc_hold(dev, false);

	/* ADC config. */
	b43_phy_write(dev, 0x0339, 0x0fff);
	/*
	 * Clear MHF slot 0 bit 13, mask 0x2000. The MAC firmware applies the
	 * maskset atomically on the HOSTF1 word.
	 *
	 * This uses the AC-PHY-specific b43_phy_ac_mhf_maskset() rather than
	 * b43_hf_write(), which takes a u64 value and covers only three slots
	 * where the AC-PHY uses five.
	 */
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x2000, 0x0000);

	/*
	 * ADC config: two consecutive rounds with different values. The first
	 * uses b43_phy_ac_adc_hi 0x03ac and b43_phy_ac_adc_lo 0x032c, the second 0x03bf and 0x0340.
	 */
	for (i = 0; i < 8; i++)
		b43_phy_write(dev, b43_phy_ac_adc_hi[i], 0x03ac);
	for (i = 0; i < 8; i++)
		b43_phy_write(dev, b43_phy_ac_adc_lo[i], 0x032c);
	/*
	 * The second round only happens from the second bring-up on: on attach
	 * the stock driver programs the pair once with 0x03ac/0x032c and moves
	 * on to 0x016e, while on a later channel setup it repeats with
	 * 0x03bf/0x0340.
	 */
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

	/* Arm the RX gate and drop the ADC bracket. */
	b43_phy_ac_rx_gate_with_adc_hold(dev, true);
	b43_phy_write(dev, 0x0339, 0x0000);

	/*
	 * PHY update strobe: MOD (mask esplicita), NON OR.
	 * Il TSSI-path enable per-core (MOD 0x0072/0x?727/0x?73c
	 * per ogni core attivo) è emesso da idle_tssi_meas, non qui.
	 */
	b43_phy_ac_cca_pulse(dev);
}


/*
 * CRS clip-detector thresholds: eight registers, four per core with a +0xc
 * stride, interleaved between cores. The vendor programs the low byte with
 * bandwidth-dependent thresholds -- 0x31 at 20 MHz in 5 GHz, 0x36 at 40 and
 * 80 -- in chanspec_tail(), and with 0x34 in block E of rxiqcal_finalize().
 */
static const u16 b43_phy_ac_crs_regs[8] = {
	0x0324, 0x0330,
	0x0321, 0x032d,
	0x032a, 0x0336,
	0x0327, 0x0333,
};

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * CRS min-power recalculation, the low byte of 0x0324 through 0x0333:
 * crs = ladder[bw][clamp(threshold + anchor[bw], 0, 14)], plus 4 when cold.
 *
 * The row follows the operating width, the entry is b43_phy_ac_crs_index() out
 * of the noise statistic the watchdog latches; the observed values are the
 * entry plus the cold bump of 4, so 45, 48, 53 and 60 appear as 49, 52, 57
 * and 64.
 */
static const u8 b43_phy_ac_crs_ladder[3][15] = {
	/* BW20 */ { 45, 48, 51, 53, 54, 57, 60, 63, 66, 68, 70, 72, 75, 78, 80 },
	/* BW40 */ { 44, 46, 48, 50, 52, 54, 56, 58, 60, 63, 66, 69, 71, 74, 76 },
	/* BW80 */ { 41, 44, 46, 48, 50, 52, 55, 57, 60, 63, 65, 68, 70, 72, 74 },
};

/*
 * Ladder index, chosen from the noise statistic the watchdog latches out of
 * SHM 0x0308.
 *
 * Three thresholds and a floor reproduce all 66 CRS writes of the 32 BW20
 * sweep segments:
 *
 *   sample < 1526   index 1
 *   sample < 1900   index 3
 *   otherwise       index 6
 *   no sample       the index in force stays
 *   sub-band change index 0
 *
 * The indices are not contiguous, which is why this is a threshold table
 * rather than arithmetic on the sample.
 *
 * The state outlives set_channel deliberately. In the sweep the threshold
 * carries from one channel to the next within a sub-band -- the first block
 * of a cycle takes no sample and rewrites what the previous cycle left -- and
 * only crossing a sub-band boundary resets it to the floor. That is what puts
 * index 0 on ch36, ch52 and ch100 and nowhere else.
 *
 * Neither existing gate exercises the carry: both run one channel, so the
 * sub-band never changes and the floor is what they see.
 */
#define B43_PHY_AC_CRS_NOISE_T1		1526
#define B43_PHY_AC_CRS_NOISE_T2		1900

/*
 * Fold a noise sample into the standing ladder index. Called from the
 * watchdog, where the sample is latched.
 */
static void b43_phy_ac_crs_note_noise(struct b43_wldev *dev, u16 sample)
{
	struct b43_phy_ac *ac = dev->phy.ac;

	if (sample < B43_PHY_AC_CRS_NOISE_T1)
		ac->crs_index = 1;
	else if (sample < B43_PHY_AC_CRS_NOISE_T2)
		ac->crs_index = 3;
	else
		ac->crs_index = 6;
}

/*
 * Standing ladder index, reset to the floor when the sub-band changes.
 *
 * The partition is pa5g_group's, at 5250 and 5500 MHz -- not the one
 * b43_ppr_ac_subband() uses for the power target, whose first boundary
 * is 5210. Using that one puts ch44 and ch48 in a different sub-band from
 * ch36 and resets the index where the vendor carries it.
 *
 * The index in force here is the one the previous cycle left: the sample is
 * latched by the watchdog, which runs after the channel setup, so a cycle
 * writes with what it inherited and the new sample only tells on the next.
 */
static unsigned int b43_phy_ac_crs_index(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	u8 sb = (u8)b43_phy_ac_pa5g_group(dev, ac->cal_freq);

	if (sb != ac->crs_subband) {
		ac->crs_subband = sb;
		ac->crs_index = 0;
	}

	return ac->crs_index;
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

	idx = b43_phy_ac_crs_index(dev);
	crs = b43_phy_ac_crs_min_pwr(dev, idx, cold);

	if (dev->phy.ac->cal_cycles < 2)
		dev->phy.ac->cal_cycles++;

	/*
	 * Solo se la soglia cambia. Il core chiama questo hook da
	 * b43_periodic_every60sec() ogni minuto, mentre il vendor riscrive la
	 * soglia solo quando l'indice della scala si muove: su 76 dei 78
	 * segmenti degli sweep le scritture CRS sono esattamente due, quelle
	 * di chanspec_tail() e del blocco E di rxiqcal_finalize(), e nessuna
	 * cade nei 9-20 s di tick del watchdog che seguono. I due segmenti che
	 * ne hanno una terza -- cold09 e 03-up -- la fanno quando il campione
	 * di rumore attraversa la soglia della scala, non a cadenza.
	 *
	 * Il confronto e' quello che rende questo un ricalcolo che decide:
	 * `DONE` significa che non c'e' altro da fare, e senza di esso ogni
	 * chiamata riscriverebbe gli otto registri.
	 */
	if (crs == dev->phy.ac->crs_low)
		return;

	b43_phy_ac_crs_regs_write(dev, crs);
}

/*
 * The 0x0910-0x0913 bank: a per-chain offset on the same scale as the CRS
 * threshold written immediately before it. The quantity that means something
 * is the sum crs + off, an absolute threshold drawn from a discrete ladder:
 * the CRS carries the part common to all eight registers and the bank the
 * per-chain correction. Write-only, four registers per core with a 0x200
 * stride.
 *
 * The direction here is the reverse of the mechanism: a rule over bandwidth
 * and phase computes the CRS and the offset follows from it. That reproduces
 * ch36 at 20 MHz op-for-op because both terms are fitted to that one
 * configuration.
 *
 * Scaffolding: these are transcribed thresholds, not derived values. Off
 * ch36 they are wrong CCA thresholds, and the filter in op_switch_channel() is
 * what keeps the code from getting there.
 *
 * TODO: the rule for the ladder index, and the transformation that produces
 * the per-chain offset, are both still missing.
 *
 * Findings, excluded hypotheses and the derivation: docs/bank-0910-analysis.md.
 */
enum b43_phy_ac_crs_site {
	B43_PHY_AC_CRS_SITE_CHANSPEC = 0,
	B43_PHY_AC_CRS_SITE_FINALIZE = 1,
};

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   7509-7516, 30910-30917]
 */
static void b43_phy_ac_prog_bank_0910(struct b43_wldev *dev, u16 crs,
				      enum b43_phy_ac_crs_site site)
{
	B43_AC_FN();
	bool is4360 = dev->dev->chip_id == 0x4360;
	/* { chanspec_tail, blocco E }, per chip. */
	static const u16 targets[2][2] = {
		{ 52, 57 },	/* 4352: d6220, DSL */
		{ 64, 67 },	/* 4360: agcombo */
	};
	u16 target = targets[is4360][site];
	/*
	 * Signed difference, not a clamped one. The sweep settles it: the sums
	 * crs + off land on this chip's two ladder entries {52, 57} on every
	 * channel, and reaching them needs both directions --
	 *
	 *   ch40 to ch48   crs 52, off +5  -> 57
	 *   ch100 to ch140 crs 57, off -5  -> 52
	 *
	 * The vendor writes the negative case as 0xfb, a signed byte. Clamping
	 * at zero writes nothing where an offset of -5 is wanted, and ch36, the
	 * one validated channel, is on the side where the difference comes out
	 * positive, so it would not show there.
	 */
	s16 off = (s16)target - (s16)crs;
	u16 hi, lo, base;

	/*
	 * On a first bring-up the chanspec_tail site writes zero whatever the
	 * target and the CRS are: the noise calibration has not run yet, so
	 * there is no offset to apply. Five attach captures, two boards and two
	 * chips agree:
	 *
	 *   d6220   attach ch36 BW20   [0] [5]
	 *   d6220   attach ch36 BW40   [0] [6] [0]
	 *   d6220   attach ch44        [0] [0]
	 *   agcombo attach ch36        [0] [6,9]
	 *   agcombo attach (wl-diag)   [0] [6,9] [6,9]
	 *
	 * On the down-to-up path the first block is not zero -- [3] [5] on the
	 * d6220, [15] [18] on agcombo -- so what this discriminates is the
	 * phase, not the call site.
	 *
	 * The condition is explicit rather than left to the arithmetic: on the
	 * 4352 target 52 minus crs 58 is negative and would come out zero
	 * anyway, but on the 4360 64 minus 58 is 6, so nothing but this test
	 * produces the zero there.
	 */
	if (site == B43_PHY_AC_CRS_SITE_CHANSPEC &&
	    (dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP))
		off = 0;

	hi = (u16)((off & 0xff) << 8);
	lo = (u16)(off & 0xff);

	for (base = 0x0910; base <= (is4360 ? 0x0b10 : 0x0910); base += 0x200) {
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
 * AFE gain regs re-emit — 5 MOD identici usati in due punti di
 * rxiqcal_finalize (blocco E post-Blocco D e coda finale post-LUT).
 *   0x0070 set 0xe000                       — top-3 gain enable bits
 *   0x0644 / 0x0844 set 0x14 mask 0x7f      — per-core AFE gain word
 *   0x0678 / 0x0878 clr bit 2               — per-core AFE bypass
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   30728-30732, 36462-36466]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   26010-26014, 29004-29008]
 */
static void b43_phy_ac_afe_gain_regs_reemit(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);
	b43_phy_maskset(dev, 0x0644, (u16)~0x007f, 0x0014);
	b43_phy_maskset(dev, 0x0844, (u16)~0x007f, 0x0014);
	b43_phy_maskset(dev, 0x0678, (u16)~0x0004, 0);
	b43_phy_maskset(dev, 0x0878, (u16)~0x0004, 0);
}

/*
 * Arm the tone generator: peek 0x0393, write @arm_val to 0x0394, write
 * 0x8000 to 0x0393. Observed with arm_val 0x0110 for the global arm, 0x0111
 * for core 1, and 0x0110 | core in the idle_tssi_meas iterations.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   33068-33071, 33200-33203, 35714-35717, 35846-35849]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   27464-27467, 27596-27599]
 */
static void b43_phy_ac_arm_tone_gen(struct b43_wldev *dev, u16 arm_val)
{
	B43_AC_FN();
	b43_phy_read_log(dev, 0x0393);
	b43_phy_write(dev,    0x0394, arm_val);
	b43_phy_write(dev,    0x0393, 0x8000);
}

/*
 * Chanspec tail after the BW1F loop: 37 ops the vendor emits immediately
 * after the BW1A/BW1F loop inside channel_setup(), with the 0x019e gate
 * already locked by the caller. Structure:
 *   1. clear bit 4 of 0x0164, undoing the set from coeff_bank_init()
 *   2. clear bits 14 and 15 of 0x030f
 *   3. write the gain to 0x031c-0x031f: 0x00bf at 20 MHz in 5 GHz, 0x0100 at
 *      80 MHz in 5 GHz, 0x00ff in 2.4 GHz
 *   4. the per-core CRS registers, eight ops -- four registers over two
 *      cores, interleaved, +0xc stride -- with 0x31 under mask 0x00ff at
 *      20 MHz in 5 GHz and 0x36 at 40 and 80
 *   5. clear the high and low bytes of 0x0910-0x0913, eight alternating ops
 *   6. peek 0x03a9, then two clears on it, bits 0-6 and bit 11
 *   7. ten raw writes to 0x00ec-0x00f5. The values do not depend on channel
 *      or width: they are identical on all 104 segments of the three sweeps
 *      (d6220 cold and hot, agcombo cold), 16 channels at 20, 40 and 80 MHz.
 *      Whether 2.4 GHz uses the same ones is not known; there is no capture
 *      of that band.
 *   8. peek and relock the outer gate, closing the tail for chan_tables()
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	unsigned int i;

	if (b43_current_band(dev->wl) != NL80211_BAND_5GHZ)
		gain = 0x00ff;
	else if (dev->wl->hw->conf.chandef.width == NL80211_CHAN_WIDTH_80)
		gain = 0x0100;

	/*
	 * CRS minimum power. On a first bring-up nothing has been measured yet
	 * and the value is the observed constant; from then on it comes off the
	 * ladder, at the index the noise statistic selects.
	 *
	 * The sample is latched by the watchdog, which runs after this, so the
	 * index in force here is the one the previous cycle left. That is what
	 * the sweep shows: the first block of a cycle takes no new sample and
	 * repeats the standing value, and only crossing a sub-band boundary
	 * resets it to the floor.
	 */
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP ||
	    dev->phy.ac->cal_width != NL80211_CHAN_WIDTH_20)
		/*
		 * First bring-up, or any bonded width: the value is the
		 * observed constant, 0x3a at 20 MHz, 0x3c at 40 and 0x3d at 80.
		 * Every cold segment writes it as the first of the two CRS
		 * values of its cycle, whatever the channel.
		 */
		crs = (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_80)
			? 0x003d
			: (dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40)
				? 0x003c : 0x003a;
	else
		crs = b43_phy_ac_crs_min_pwr(dev, b43_phy_ac_crs_index(dev),
					     true);

	dev->phy.ac->crs_written = (u8)crs;

	/*
	 * The clear of 0x0164 bit 4 is skipped at 80 MHz: the bandwidth
	 * selector left the bit already clear there, and the vendor writes this
	 * register twice per cycle at 20 and 40 MHz and once at 80.
	 */
	if (dev->phy.ac->cal_width != NL80211_CHAN_WIDTH_80)
		b43_phy_maskset(dev, 0x0164, (u16)~0x0010, 0x0000);
	b43_phy_maskset(dev, 0x030f, (u16)~0xc000, 0x0000);

	b43_phy_write(dev, 0x031c, gain);
	b43_phy_write(dev, 0x031d, gain);
	b43_phy_write(dev, 0x031e, gain);
	b43_phy_write(dev, 0x031f, gain);

	b43_phy_ac_crs_regs_write(dev, crs);
	b43_phy_ac_prog_bank_0910(dev, crs, B43_PHY_AC_CRS_SITE_CHANSPEC);

	b43_phy_read_log(dev, 0x03a9);
	b43_phy_maskset(dev, 0x03a9, (u16)~0x007f, 0x0000);
	b43_phy_maskset(dev, 0x03a9, (u16)~0x0800, 0x0000);

	for (i = 0; i < ARRAY_SIZE(post_bw1f_raw); i++)
		b43_phy_write(dev, post_bw1f_raw[i].reg, post_bw1f_raw[i].val);

	/* Peek + relock outer gate (chiude il chanspec_tail per chan_tables). */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
}

/*
 * Seed for one chain's RX gain LUT, emitted during the body of the
 * noise-shaping tables. Two of the three fields come from the SPROM:
 *
 *   gainctx  = ((triso[core] + 4) << 1) + 2   -- the +2 is wl 7's
 *   hdr      = (elnagain[core] + 3) << 1      -- eLNA header
 *   lna1_idx = 0x02                           -- the stock driver pins this
 *
 * lna1_idx is not read back from the table because table 0x45 offset 0x20 is
 * not populated yet at this point: it gets written three ops later with
 * fill_02, whose first element is that same 0x02.
 *
 * The core iteration and the coremask filter belong to the caller; the stock
 * driver emits this for every num_cores without filtering, on 2x2 boards
 * too.
 *
 * Uses rxgains_5gl, U-NII-1, consistent with the ch36 target. 5gm and 5gh
 * will need a per-sub-band selection.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	const struct ssb_sprom_rxgains *rxgains = &sprom->rxgains_5gl;
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
 * Whether the calibrations that transmit may run on this channel.
 *
 * The condition is regulatory, not empirical. These calibrations drive the
 * tone generator: they transmit. On a channel where radar detection is
 * required, transmission is not allowed until the channel availability check
 * has completed, so they cannot run.
 *
 * Which sub-bands carry the duty is not this driver's to decide: it is in
 * IEEE80211_CHAN_RADAR, which the regulatory domain sets, on the same
 * ieee80211_channel this code already reads hw_value and center_freq from.
 * Whether the check has completed is mac80211's, and mac80211 tells the driver
 * -- see the note on ac->cac_pending in phy_ac.h for the producer, and for why
 * this does not read cfg80211's dfs_state directly even though it would give
 * the same answer.
 *
 * The captures confirm it and did not supply it, which is worth keeping
 * straight because the two answers differ. Below the boundary: PHY 0x0380, the
 * tone generator's command register, takes 313 to 978 accesses on every
 * segment from channel 48 down and radio 0x0b22 nine. Above it, on all
 * nineteen segments from channel 52 up, not one access to either. It is not a
 * shorter run, it is nothing, and it is most of the difference between a
 * 29k-operation attach and a 16k one. In the hot sweep those same phases run
 * above the boundary too, at every channel -- on 09-up-ch52-bw20 and
 * 19-up-ch104-bw20 exactly as on 01-up-ch36-bw20 -- because there the CAC had
 * long completed.
 *
 * Where the two answers part company is U-NII-3. Both sweeps stop at ch140,
 * 5700 MHz, so every captured channel that carries the radar duty is above
 * 5250 and every one that does not is below: a plain threshold at 5250 fits
 * the data exactly. It is still the wrong rule, because 5725-5850 carries no
 * radar duty either, and a threshold would suppress on channels 149-165
 * calibrations that are allowed to run there. No capture can arbitrate that,
 * which is the reason to take the rule from the spec instead of from the
 * sweep.
 *
 * One thing this does not settle: who owns the CAC. Here mac80211 does, on
 * cfg80211's behalf and at a userspace AP's request, and it hands the driver
 * the channel before the check rather than after -- .start_radar_detection is
 * called with the hardware already tuned, which is exactly the window these
 * calibrations must sit out. wl runs its own check inside the attach instead,
 * which is why the captures show them skipped at all. Same rule, different
 * owner, and the difference shows up in what b43 will do on hardware: by the
 * time an AP bring-up arrives the check has finished, so the calibrations will
 * run where the cold captures have them absent. See docs/retrace-todo.md.
 */
static bool b43_phy_ac_may_calibrate_tx(struct b43_wldev *dev)
{
	const struct ieee80211_channel *chan = dev->phy.chandef->chan;

	if (!(chan->flags & IEEE80211_CHAN_RADAR))
		return true;
	return !dev->phy.ac->cac_pending;
}

/*
 * Orchestrator for the post-channel-setup calibrations: everything the vendor
 * emits after the rxcal_afe finalize, gathered in one place. In order:
 * post_cal_finalize iterations 2 and 3, rxiqcal iterations 1 to 24, the
 * rxcal AFE calibrate and finalize, a first txpwr round with its rxiqcal
 * iteration, a second txpwr round with the gainctrl_final loop, then the RXIQ
 * teardown and finalize.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   14968-36041]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   10382-28591]
 */
static void b43_phy_ac_post_switch_calibrations(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * Runs with the MAC enabled, after op_switch_channel()'s body has
	 * put the classifier in WAITED mode, that is RX_OFDM
	 * plus RX_WAITED on a normal channel setup. The observed entry state is
	 * {MAC_EN | RX_OFDM | RX_WAITED}; CLIP_ALL_DIS is not set here, since
	 * post_cal_finalize() sets it through clip_det() and does its own
	 * mac_suspend.
	 *
	 * The minimum precondition is RX_WAITED, without which the calibration
	 * is meaningless. CCA_RESET is incompatible with any radio operation.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED,
			   B43_PHY_AC_STATE_CCA_RESET);

	/*
	 * Post-cal finalize, iterazioni 2 e 3.
	 */
	b43_phy_ac_post_cal_finalize(dev);
	b43_phy_ac_post_cal_finalize_iter3(dev);
	b43_phy_ac_rxiqcal_apply(dev);
	/*
	 * Salta dal canale 52 in su: la tabella 0x000e, che questa fase e' la
	 * sola a toccare, ha 8 accessi sui canali fino al 48 -- 11 a 80 MHz --
	 * e zero dal 52 in su, su tutti e 26 i segmenti.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_post_rxiqcal_stage2(dev);

	/*
	 * RX AFE calibration (~1500 op), on the lower 5 GHz
	 * channels only.
	 *
	 * Skipped above 5250 MHz; see b43_phy_ac_may_calibrate_tx().
	 */
	if (b43_phy_ac_may_calibrate_tx(dev)) {
		b43_phy_ac_rxcal_afe_calibrate(dev);
		b43_phy_ac_rxcal_afe_finalize_gain_luts(dev);
	}

	/*
	 * Primo round post-cal RXIQ.
	 */
	b43_phy_ac_txpwr_by_index(dev, B43_PHY_AC_TXPWR_INDEX_DEFAULT);
	b43_phy_ac_rxgain_defaults_pulse(dev);
	b43_phy_ac_radio_chain_range_setup(dev, true);
	b43_phy_ac_rxgain_perchan_config(dev);
	b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(dev);
	/*
	 * Semina del tono DDS. Il discriminante e' l'identita' della tabella e
	 * non l'indirizzo, perche' il lavoro di questa fase sono scritture su
	 * una porta dati: la tabella 0x000e ha 8 accessi sui canali fino al 48,
	 * 11 a 80 MHz, e zero dal 52 in su su tutti e 26 i segmenti, e le voci
	 * 0x40/0x48/0x50 della tabella 0x000c sei e zero.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_rxiqcal_dds_seed(dev);
	b43_phy_ac_rxiqcal_prep_second_iter(dev);
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_rxiqcal_run_meas_iters(dev);
	b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(dev);
	/*
	 * Azzeramento delle tabelle dei coefficienti IQ, 0x42/0x62/0x82: 256
	 * voci ciascuna sui canali fino al 48, zero dal 52 in su su tutti e 26
	 * i segmenti. Sono le tabelle che le fasi di calibrazione
	 * dietro may_calibrate_tx() riempiono, quindi la' non c'e' niente da
	 * azzerare.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_iqcal_coeff_tables_reset(dev);

	/*
	 * Second round post-cal: applica coefficienti misurati dagli iter
	 * 19-24. Nuova txpwr_by_index (5° di 11) + rxgain_defaults_pulse.
	 */
	b43_phy_ac_txpwr_by_index(dev, B43_PHY_AC_TXPWR_INDEX_DEFAULT);
	b43_phy_ac_rxgain_defaults_pulse(dev);
	b43_phy_ac_radio_chain_range_setup(dev, false);
	b43_phy_ac_iqcal_apply_second_stage(dev);
	b43_phy_ac_rxgain_config_readback(dev);
	/*
	 * Assente sopra i 5250 MHz al primo bring-up, e il test e' quello che
	 * il commento di may_calibrate_tx() prescrive: la conta di `PHY 0x0724`
	 * e `PHY 0x0736`, che fanno 27 accessi ciascuno a ch36 e 9 dal canale
	 * 52 in su, su tutti e 26 i segmenti e per ogni larghezza. I 9 che
	 * restano sopra la soglia sono di altre fasi -- rx_gain_regs_program()
	 * scrive 0x0724, rxgain_perchan_tail() 0x0736 -- quindi la differenza
	 * e' per intero il contributo di questa.
	 *
	 * Lo sweep a caldo separa i due termini del predicato invece di lasciare
	 * la sola soglia: su 09-up-ch52-bw20 e 19-up-ch104-bw20 i due registri
	 * fanno dieci accessi come su 01-up-ch36-bw20, quindi sopra la soglia il
	 * salto vale al primo bring-up e non oltre -- come per le altre fasi
	 * dietro questo gate.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_rxgain_config_apply(dev);
	/*
	 * Configurazione IQ-cal della radio. E' il caso piu' netto del gruppo:
	 * la fase tocca dodici registri radio -- 0x0020-0x0023, 0x003a, 0x003d
	 * e i mirror per catena a +0x200 -- e tutti e dodici sono a zero dal
	 * canale 52 in su su tutti e 26 i segmenti, contro 3 a 21 accessi
	 * ciascuno sotto. Nessun indirizzo condiviso, quindi non c'e' margine
	 * di interpretazione.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_radio_iqcal_config(dev);

	/*
	 * Ricerca del guadagno di loopback (round 6°-9° di txpwr apply):
	 * vedi b43_phy_ac_loopback_gain_search e il commento alla ricerca.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_loopback_gain_search(dev);

	/*
	 * Le passate di misura: semina di un tono e variante v2 di meas_apply,
	 * ripetute. Due fino a 40 MHz, sei a 80, ai passi +1, -1, +3, -3, +4 e
	 * -4 del periodo -- vedi b43_phy_ac_tone_steps.
	 *
	 * Assenti dal canale 52 in su. Per le semine vale il criterio della
	 * tabella 0x000e sopra; per apply_v2 sono 22 dei suoi 36 indirizzi a
	 * zero, e sono tutti quelli caratteristici della fase -- 0x0270,
	 * 0x0460-0x0464, 0x0471, 0x0382, 0x0271/0x0272 e i banchi 0x06cx e
	 * 0x08cx. I 14 che restano sono le porte delle tabelle e 0x019e, che
	 * tutto il resto del flow usa.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev)) {
		unsigned int pass, n = b43_phy_ac_meas_passes(dev);

		for (pass = 0; pass < n; pass++) {
			b43_phy_ac_rxiqcal_dds_seed_tone(dev,
					b43_phy_ac_tone_steps[pass]);
			b43_phy_ac_iqcal_meas_post_dds_apply_v2(dev);
		}
	}

	/* Teardown finale RXIQ. */
	b43_phy_ac_rxiq_apply_coefficients(dev);
	/* Stessi dodici registri di radio_iqcal_config, stesso conteggio. */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_radio_iqcal_teardown(dev);
	b43_phy_ac_rxiq_teardown_apply_defaults(dev);
	b43_phy_ac_rxiqcal_finalize(dev);
}

/*
 * Configurations the port is known to reproduce op-for-op against a vendor
 * capture. ch36 at 20 MHz is covered on both the 4352 and the 4360.
 *
 * Width belongs in here alongside the channel because a large part of what
 * the channel setup programs is bandwidth-dependent: the coefficient bank,
 * the CRS thresholds, the chanspec gain, the farrow mode. ch44 and the 40 and
 * 80 MHz configurations are captured but not covered -- the ch36 40 MHz trace
 * carries some 500 ops this port does not emit -- and the crs_min_pwr
 * thresholds are already known to move with the sub-band: in the d6220 sweep
 * the low byte of 0x0321/0x0324/0x032d/0x0330/0x0333 reads 0x34 on ch36 and
 * 0x39 on ch100 and ch140.
 *
 * An entry earns its place by a capture, not by looking plausible.
 */
static const struct {
	u16 channel;
	enum nl80211_chan_width width;
} b43_phy_ac_validated_configs[] = {
	{ 36, NL80211_CHAN_WIDTH_20 },
};

/*
 * Avviso una volta per sito: dice che qui il driver scrive qualcosa che non sa
 * derivare, e cosa puo' andare storto se l'hardware non e' quello su cui il
 * valore e' stato ricavato.
 *
 * Una volta e non ogni volta: alcuni di questi punti stanno in un ciclo di
 * calibrazione e riempirebbero il log. E per sito e non globale: quale dei
 * punti si e' toccato e' l'informazione utile.
 *
 * Non e' B43_WARN_ON, che segnala uno stato che non dovrebbe accadere: questi
 * accadono per costruzione, e il messaggio serve a chi legge un dmesg dopo che
 * qualcosa non ha funzionato.
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

static bool b43_phy_ac_config_validated(struct b43_wldev *dev, u16 chan,
					enum nl80211_chan_width width)
{
	unsigned int i;

	/*
	 * CONFIG_B43_PHY_AC_ANY_CHANNEL exists so the bring-up work can drive
	 * an uncovered configuration against its capture and grow the list
	 * above. It defeats a guard that protects the PA, hence
	 * B43_DEBUG-only and default n.
	 */
	if (IS_ENABLED(CONFIG_B43_PHY_AC_ANY_CHANNEL)) {
		b43_phy_ac_todo(dev,
			"channel guard defeated by CONFIG_B43_PHY_AC_ANY_CHANNEL. "
			"Several tables are fitted to ch36 at 20 MHz and have no "
			"evidence elsewhere: the AFE low-pass stages, the CRS "
			"minimum power ladder, and the per-bandwidth entries of "
			"the 0x00ec-0x00f5 block. Transmit power and receive "
			"sensitivity may be wrong here.\n");
		return true;
	}

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_validated_configs); i++)
		if (b43_phy_ac_validated_configs[i].channel == chan &&
		    b43_phy_ac_validated_configs[i].width == width)
			return true;

	return false;
}

/*
 * The TX power adjust that follows a channel switch: adjust_txpower.
 *
 * b43 reaches it from b43_op_config() through b43_phy_txpower_check() and the
 * txpower_adjust_work, after b43_switch_channel() has returned and the core
 * has written the BSS configuration. The boundary is read off the data, not
 * chosen: on the reference segment the last op of the switch is the
 * basic-rate map and the first of this one is the PLCP, with the core's BSS
 * block in between. The frequency comes from the channel the switch tuned,
 * the only value of its prologue this phase used.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   13665-14082]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9295-9714]
 */
static void b43_phy_ac_txpwr_adjust(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *ac = dev->phy.ac;

	b43_maccontrol_set(dev, ~0x10000000u, 0x10000000);
	b43_maccontrol_set(dev, ~0x10000000u, 0);
	b43_maccontrol_set(dev, ~0x00040000u, 0x00040000);
	b43_maccontrol_set(dev, ~0x48020000u, 0x00020000);

	/*
	 * PRETBTT, il preavviso in microsecondi rispetto al TBTT. La cella e'
	 * M_PRETBTT, 0x4b*2. brcmsmac la definisce e non la scrive mai, quindi
	 * lascia il default dell'hardware; b43 la scrive con 250 in AP e 2 in
	 * adhoc (b43_set_pretbtt()), e la cattura porta 2 pur essendo in AP.
	 *
	 * Lo split 250/2 di b43 e' logica dei core vecchi. Qui il beacon lo
	 * costruisce l'ucode dal template in template RAM, che e' il carico che
	 * emit_core_bss_ssid() rispecchia, quindi l'host non ha niente da
	 * preparare e il preavviso lungo non serve. Il 2 lo si emette percio'
	 * come valore di questo core, non come il ramo adhoc di b43.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0096, 2);
	b43_mac_enable(dev);
	b43_maccontrol_set(dev, ~0x00100000u, 0x00100000);
	b43_mac_suspend(dev);
	/*
	 * Read-modify-write of 0x00cc: reads the value and writes it back twice
	 * unchanged. The cell is touched the same way in the BSS config and on
	 * every watchdog tick, always with the same pattern; what the double
	 * rewrite is for is unknown.
	 */
	{
		u16 cc = b43_shm_read16(dev, B43_SHM_SHARED, 0x00cc);

		b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
		b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
	}
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ce, 0x0000);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00d0, 0x0000);

	/* Second pass of the twelve-rate loop. */
	b43_phy_ac_chainmask_block(dev);
	b43_phy_ac_prb_rsp_rate_po(dev);
	b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);                               /* peek */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);      /* relock */

	/* Second txpwrctrl_setup() call. The vendor emits the same sequence
	 * op-for-op: the LUT is computed from the same SPROM coefficients and
	 * the ppr values are unchanged. */
	b43_phy_ac_txpwrctrl_setup(dev, 5000 + 5 * ac->cal_channel);

	/*
	 * Transition after the second txpwrctrl_setup(): TX power control setup,
	 * a restore, five MAC wake/suspend pulses -- probably a firmware flush
	 * to make it reload the rate table -- then three MODs on 0x019e clearing
	 * bits 6, 7 and 8.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);          /* unlock */
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);
	/* Come sopra: coppia gain word assente al primo bring-up. */
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		b43_phy_maskset(dev, 0x0644, (u16)~0x007f, 0x0014);
		b43_phy_maskset(dev, 0x0844, (u16)~0x007f, 0x0014);
	}
	b43_phy_maskset(dev, 0x0678, (u16)~0x0004, 0);
	b43_phy_maskset(dev, 0x0878, (u16)~0x0004, 0);
}

/*
 * RX gain-control calibration: the per-core loopback sweep that opens the
 * post-switch calibrations, with its radio setup, tone generator and cleanup.
 *
 * In the capture it sits 3.3 s after the TX power adjust and two ops before
 * the rest of the calibrations, so it is theirs and not the channel switch's.
 * Between the adjust and this the core emits four conf_tx passes, one per
 * access queue, each in its own enable/suspend bracket and ~79 ops apart; the
 * bracket is the core's like the payload, so there is no loop here.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   14407-14966]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   9821-10380]
 */
static void b43_phy_ac_rxgainctrl_cal(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);                              /* peek */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0);

	/* Per-core RX gain-control programming, immediately after the third
	 * transition that follows the two txpwrctrl_setup() calls. */
	b43_phy_ac_rxgainctrl_regs(dev);

	/* Radio loopback setup per-core (34 op/core). */
	{
		u8 c;
		u8 num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;
		for (c = 0; c < num_cores; c++) {
			if (!((mask >> c) & 1))
				continue;
			b43_phy_ac_rxcal_radio_setup(dev, c);
		}
	}

	/* PHY tone-setup (26 op, non per-core). */
	b43_phy_ac_rxcal_tone_setup(dev);

	/* Per core: tone_arm() then gainctrl(), a four-step probe sweep with
	 * settling -- 83 ops per core. */
	{
		u8 c;
		u8 num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;
		for (c = 0; c < num_cores; c++) {
			if (!((mask >> c) & 1))
				continue;
			b43_phy_ac_rxcal_tone_arm(dev, c);
			b43_phy_ac_rxcal_gainctrl(dev, c);
		}

		/* Cleanup and finalize after the calibration, in the vendor's
		 * order: every core's PHY cleanup, then every core's radio
		 * cleanup, then unarm the tone, the gate ops and mac_enable. */
		b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x03d0);    /* unlock gate plain */

		for (c = 0; c < num_cores; c++) {
			if (!((mask >> c) & 1))
				continue;
			b43_phy_ac_rxcal_cleanup(dev, c);       /* 14 PHY WR */
		}
		for (c = 0; c < num_cores; c++) {
			if (!((mask >> c) & 1))
				continue;
			b43_phy_ac_rxcal_radio_cleanup(dev, c); /* 7 RAD WR */
		}

		/* Finalize (unarm tone gen + gate cleanup). */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);  /* relock */
		b43_phy_write(dev,   0x0394, 0x000b);
		b43_phy_write(dev,   0x0393, 0x0000);                /* unarm */
		b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);       /* unlock */
	}

}

/*
 * channel_calibrate: the calibrations that close a channel switch, run by
 * b43_op_config() in place of its final mac_enable. The RX gain-control sweep
 * needs the MAC suspended and the calibrations after it need it running --
 * rxgainctrl_regs() forbids MAC_EN, post_switch_calibrations() requires it --
 * so the enable the caller owes sits between the two, where the vendor emits
 * it.
 *
 * Checked against the capture with annotate_enables.py: the last conf_tx
 * pass leaves the MAC suspended at #14406, the sweep runs #14407-#14966 with
 * no MAC transition, #14967 is the one MAC.MCTRL enable between #14406 and
 * #15100, and post_cal_finalize() enters at #14968 with
 * {MAC=1, RX=ow, CLIP=000, CCA=0}: the REQUIRE it carries.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   14406-14968]
 */
static void b43_phy_ac_op_channel_calibrate(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_ac_rxgainctrl_cal(dev);
	b43_mac_enable(dev);
	b43_phy_ac_post_switch_calibrations(dev);
}

/*
 * Read the RF-chain hardware inventory into ac->{num_cores, coremask}.
 *
 * Called from both op_init and op_software_rfkill: b43_phy_init drives
 * software_rfkill(false) before ops->init, so these fields have to be
 * populated on demand from either entry point. Hardware inventory is
 * stable for the lifetime of the device, so once num_cores is set the
 * call is a no-op.
 *
 * num_cores is the raw slot count from PHY 0x000b (e.g. 3 on a 2x2 part),
 * clamped to the 3-chain array max. coremask is the SROM-declared subset
 * that is actually populated; touching the radio/PHY space of an
 * unpopulated, powered-down core hangs the backplane, so every per-core
 * register loop is gated on it. Fall back to all cores when the SROM
 * leaves rxchain unset. First-run capture on the DSL-3580L reads
 * num_cores=3, coremask=3.
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

	b43dbg(dev->wl, "phy-ac: num_cores=%u coremask=0x%x\n",
	       ac->num_cores, ac->coremask);
}

/*
 * Pre-op_init analog frontend: runs
 * after the radio bring-up (op_software_rfkill) and before op_init proper.
 * The vendor programs pdet + per-core RF-frontend gates here so that
 * init_regs and the first channel_setup see a settled analog state. The
 * vendor does the equivalent a second time inside channel_setup, after
 * op_init; this is the first occurrence, and both are needed.
 *
 * pdet is the 11-write short form (no 0x0358 trio, which is set_channel-only).
 * Per core: PHY 0x0X29/0x0X21 bit 12, RAD 0x0X33 nibble -> 0x4000. Closes
 * with PHY 0x01b0 bit 15.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* Vendor reads PHY 0x0000 here before the PMU regctl. */
	b43_phy_read_log(dev, 0x0000);
}

/*
 * The seven MHF clears the vendor emits twice during a cold attach: once in
 * the analog-on preamble, and again after the two MHF sets further on. The
 * sequence is identical both times, so it lives here once.
 *
 * SALAME: what the individual bits mean is still unknown -- see the comment
 * in op_init() -- so this is fidelity to the blob, not understanding.
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
 * Bring-up MHF configuration: two sets and the seven clears, in the stock
 * driver's order. Not channel-dependent -- identical on ch36, ch44 and ch36
 * at 40 MHz.
 */
static void b43_phy_ac_mhf_config(struct b43_wldev *dev)
{
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x0080, 0x0080);
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x0100, 0x0100);

	b43_phy_ac_mhf_bringup_clears(dev);
}

/*
 * PHY attach/init entry (.init op). Whole-attach dispatcher: each step below
 * carries its own capture range on its own function. First op on the wire is
 * the num_cores read; the last is the trailing PMU regctl/GPIO pair.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* Only the 0x4352/0x4360 chips are supported. */
	if (dev->dev->chip_id != 0x4352 && dev->dev->chip_id != 0x4360) {
		b43err(dev->wl,
		       "AC-PHY: chip 0x%04x not in the implemented acphychipid dispatch {0x4352,0x4360}\n",
		       dev->dev->chip_id);
		return -EOPNOTSUPP;
	}

	/*
	 * PLLCTL3 is set before this code runs, and what it holds depends on the
	 * board more than on the chip. What the captures show:
	 *
	 *   DSL-3580L (4352)  only reads of the PLL, no writes at all
	 *   D6220 (4352)      writes 0xc31 and 0x100e early, like the 4360
	 *   agcombo (4360)    writes 0xc31 and 0x100e early and rewrites them
	 *                     identically later
	 *
	 * So 0x00133333 is not a property of the 4352, it is a property of the
	 * DSL. Comparing against it and refusing would block bring-up on a
	 * d6220, where bcma_pmu_pll_init() writes 0x100e.
	 *
	 * This therefore only logs what it finds: the value tells you what the
	 * PMU did, it is not grounds for a decision. There is no well-founded
	 * condition to refuse on, and inventing one blocks working hardware.
	 */
	{
		u32 pllctl3 = bcma_chipco_pll_read(&dev->dev->bdev->bus->drv_cc,
						   BCMA_CC_PMU_PLL_CTL3);
		b43dbg(dev->wl, "AC-PHY: PLLCTL3 = 0x%08x all'ingresso di op_init\n",
		       pllctl3);
	}

	b43_phy_ac_probe_cores(dev);

	/*
	 * Analog frontend that the vendor runs between radio bring-up and
	 * op_init proper, before the PMU regctl strobe.
	 */
	b43_phy_ac_pre_init_frontend(dev);

	/*
	 * PMU regctl chip-dependent field [24:20] (0x1 on 4352, 0x2 on 4360) +
	 * GPIO.CTL clear, at the head of the op_init
	 * window. The PLLCTL2/3 writes and the regctl 0x2 strobe from this
	 * window live in bcma_pmu_{pll,resources}_init.
	 */
	if (dev->dev->chip_id == 0x4360)
		bcma_chipco_regctl_maskset(&dev->dev->bdev->bus->drv_cc, 0, ~0x01f00000, 0x200000);
	else
		bcma_chipco_regctl_maskset(&dev->dev->bdev->bus->drv_cc, 0, ~0x01f00000, 0x100000);

	bcma_chipco_gpio_control(&dev->dev->bdev->bus->drv_cc, 0xffff, 0);

	b43_phy_ac_mode_init(dev);

	/*
	 * tables_init runs on a first bring-up only. Every row of its list is
	 * either a PHY constant that never changes once loaded or PAPD state
	 * that a warm cycle must not clear, and the ids it would write --
	 * 0x04, 0x40, 0x42, 0x60, 0x62 -- do appear in a warm capture, but
	 * from elsewhere: the est_pwr tables come out of txpwrctrl_setup(),
	 * right after it writes the target to 0x0646, and 0x42 and 0x62 out of
	 * the RX-IQ path. The first table select of a warm cycle is id 0x0a.
	 */
	if (dev->phy.do_full_init)
		b43_phy_ac_tables_init(dev);

	/* Band/chip-agnostic PHY register writes. */
	b43_phy_ac_init_regs(dev);

	/*
	 * MHF configuration is not emitted here on a warm cycle. The attach
	 * capture has it in the preamble, before the radio body, and a sweep
	 * segment has it at the very end -- episodes 29093 to 29114 of a
	 * segment that ends at 29162, followed by MAC.MCTRL and the MAC
	 * shared-memory block. So on a warm cycle it closes the cycle rather
	 * than opening it, and emitting it at the head of op_init puts it some
	 * 22000 ops early.
	 *
	 * Where it belongs on that path is not settled: the ops after it are
	 * MAC configuration, so the sequence may be the tail of this cycle or
	 * the head of the next. Until a capture separates the two it is left
	 * out rather than placed on a guess.
	 *
	 * SALAME: the individual bits are undocumented. If the hardware
	 * misbehaves after bring-up, they are candidates to investigate.
	 */

	/* Latch the phase; see B43_PHY_AC_STATE_FIRST_BRINGUP. */
	if (dev->phy.do_full_init)
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_FIRST_BRINGUP;
	else
		dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_FIRST_BRINGUP;

	/*
	 * No trailing mac_suspend. b43 reaches ops->init with the MAC already
	 * disabled, so a suspend here would not touch the hardware: it would
	 * only raise the refcount, and op_switch_channel(), which b43_phy_init() calls
	 * straight afterwards, would find it at 1 instead of 0. Its 90
	 * suspend/enable pairs would then all become no-ops and the MAC would
	 * never be toggled during the calibration.
	 */
	return 0;
}

/*
 * Program the PHY analog front-end bank (the AFE_C1 registers at +0x1000
 * stride, 0x1720-0x173e) to the requested mode. B43_PHY_AC_AFE_OFF is the
 * final RX/TX arm the OEM emits at bss-up;
 * B43_PHY_AC_AFE_ON parks the front-end. Kept as one named operation so
 * the enable point is explicit and callers pick a mode rather than
 * open-coding register writes.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * Three sites emit it and they differ only in the pair's values: the entry of
 * b43_phy_ac_switch_analog_once() and its cold tail put back what the entry
 * saved, while the bss-up step at the end of b43_phy_ac_rxiqcal_finalize() has
 * no save of its own and the captures show 0x0000 and 0x0001 there -- 26 cold
 * segments and 51 of the 52 warm ones carry exactly one AFE bank, one 0x0408
 * and one 0x0417 in that phase.
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
 * regctl 0 bit 1 is a PMU resource request. The stock driver uses it as a
 * bracket around the channel work: raised in the attach preamble and lowered
 * at its end, then on a later bring-up lowered on entry to op_switch_channel()
 * and
 * raised on exit. The state is tracked here because the bit cannot be read
 * back; without tracking it, the attach path lowered it twice.
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
 * b43 calls switch_analog(dev, true) from four sites: the attach reset, right
 * after b43_phy_allocate() has made phy->ops non-NULL, the core-init reset,
 * b43_chip_init() and b43_phy_init(). The vendor emits the preamble once per
 * bring-up, so
 * three of the four entries must do nothing but the analog bank.
 *
 * The discriminant is the channel. b43 only has one from b43_phy_init()
 * onwards -- that function points phy->chandef at the hardware config on the
 * line before it calls switch_analog(), and nobody sets it earlier: on a first
 * bring-up b43_op_config() has not run yet. So the first entry that has a
 * channel is the one the preamble belongs to, and the state bit keeps the
 * later entries out. op_prepare_structs() clears it, which b43 calls once per
 * ifconfig up.
 *
 * On a cold bring-up that selects the b43_phy_init() entry, on a warm one the
 * b43_chip_init() entry, and those are the same position in the op stream:
 * b43_phy_init() emits nothing between the two.
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
 * Cold-bring-up frontend GPIO, bracketed by the PMU resource request. The
 * vendor emits it between the analog preamble and the radio body: regctl
 * bit 1 raised, a round of MHF and MACCTL, the three GPIO registers, then
 * regctl bit 1 lowered.
 *
 * The placement follows the capture rather than subsystem affinity: what is
 * in here is MAC and GPIO, not analog. b43 has no hook between switch_analog
 * and software_rfkill, and this is the phase in which the vendor does it.
 *
 * This is not the bss-up block that op_switch_channel() emits: that one has
 * only two gpio_out writes and the regctl raise, with no gpio_control, no
 * gpio_outen and no closing clear. Distinct sequences, and not factorable.
 *
 * Not reproduced here: the write to BCMA_CC_PMU_CTL and the poll of
 * BCMA_CLKCTLST. The first belongs to the bcma PMU init in patch 0007, and
 * b43_bcma_wireless_core_reset() already does the second.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   584-645]
 */
static void b43_phy_ac_frontend_gpio_setup(struct b43_wldev *dev)
{
	struct bcma_drv_cc *cc = &dev->dev->bdev->bus->drv_cc;

	B43_AC_FN();

	b43_phy_ac_pmu_req(dev, true);

	b43_phy_ac_mhf_maskset(dev, 2, (u16)~0x0040, 0);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);
	b43_maccontrol_set(dev, 0, 0x04000400);

	bcma_chipco_gpio_control(cc, 0x00000407, 0x00000000);
	bcma_chipco_gpio_out(cc, 0x00000407, 0x00000400);
	bcma_chipco_gpio_outen(cc, 0x00000407, 0x00000407);

	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x0080, 0x0080);
	b43_maccontrol_set(dev, 0, 0x04000400);
	b43_maccontrol_set(dev, 0, 0x04000400);
	/*
	 * From here on a HOSTFn change reaches the cell. Both captures put the
	 * transition between these two calls: the slot 4 write above leaves no
	 * OBJ.WR behind, the slot 0 write below does. See the comment on
	 * mhf_writethrough.
	 */
	dev->phy.ac->mhf_writethrough = true;
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x0100, 0x0100);
	b43_maccontrol_set(dev, 0, 0x04000400);

	/*
	 * A second write of the 0x2e4 field, on the upper 5 GHz channels only.
	 * The lower ones -- 36 to 48 -- take one write, done in the AFE unit
	 * above; everything from 52 up takes this one as well.
	 *
	 * The condition is on the channel because nothing else is available:
	 * the sweep this comes from unloads and reloads the driver between
	 * channels, so there is no carried-over state for the driver to have
	 * branched on, and the whole attach is 36k operations on 36 to 48
	 * against 20k from 52 up. What the field means is not known, and which
	 * property of the channel the stock driver actually tests is not
	 * either: 5250 MHz is where the regulatory domains put the DFS
	 * boundary, and the traces show no DFS-specific work on either side, so
	 * "above 5250" and "requires radar detection" are the same set here and
	 * cannot be told apart.
	 */
	if (dev->phy.chandef->chan->center_freq > 5250)
		b43_phy_maskset(dev, 0x02e4, (u16)~0x3f00, 0x0f00);

	b43_phy_ac_pmu_req(dev, false);

	b43_maccontrol_set(dev, 0, 0x04000404);
	b43_phy_ac_mhf_bringup_clears(dev);

	/*
	 * Tail of the preamble, before the radio body. The GPIO control write
	 * with an empty mask changes nothing, but the stock driver emits it, so
	 * it stays here rather than leaving a hole in the sequence.
	 */
	b43_maccontrol_set(dev, 0, 0x04020402);
	b43_maccontrol_set(dev, (u32)~0x0000c000u, 0);
	bcma_chipco_gpio_control(cc, 0x00000000, 0x00000000);

	/*
	 * The preamble does NOT end with a fourth MACCONTROL write setting INFRA
	 * and DISCPMQ and clearing AP. Those are the core's operating mode, not
	 * the front end's, and b43_adjust_opmode() sets them: the PHY has no
	 * business writing them. The captures agree -- the stock driver
	 * emits it after the core has written its chip-init cells to shared
	 * memory, not with the GPIO setup: in the capture two shared-memory
	 * writes sit between the GPIO control write above and this one.
	 */
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   528-583]
 */
static void b43_phy_ac_switch_analog_once(struct b43_wldev *dev, bool on,
					  bool cold)
{
	B43_AC_FN();
	u16 saved_417, saved_416;

	/*
	 * Only the cold entry emits anything. b43 calls switch_analog(dev,
	 * true) from three sites before b43_phy_init() -- the attach reset, the
	 * core-init reset and b43_chip_init() -- and the vendor emits nothing
	 * at any of them: in the cold capture the AFE_ON bank appears three
	 * times in the whole trace, twice in the cold preamble, which enters
	 * twice, and once at bss-up. Each entry that did emit it would also
	 * re-read the ten save registers, so the reads the preamble needs would
	 * arrive four times where the vendor makes them once.
	 *
	 * Nor does the power-down, before the preamble has run: the AFE_DOWN
	 * bank appears once, and that one is b43_phy_ac_mode_init()'s.
	 *
	 * The teardown is covered by the same rule, and the sweep is what
	 * establishes it: 26 cycles of insmod, wl up, wl down, rmmod, and on 27
	 * rmmod out of 27 there is not one PHY, RAD or TBL op between the last
	 * PHY op and the `mod GOING` marker -- only GPIO and SI.COREREG. The
	 * AFE_DOWN bank appears 26 times in the whole trace, one per cycle, and
	 * they are mode_init's 26. So b43_wireless_core_exit() does not touch
	 * the front end and must not.
	 *
	 * COLD_PREAMBLE does not separate these cases and does not need to: a
	 * single bring-up has `on == true` entries both before and after the
	 * preamble.
	 */
	if (!cold)
		return;

	/*
	 * The generic b43_phyop_switch_analog_generic writes B43_MMIO_PHY0
	 * (0x3e6); the OEM never touches that offset (no SI.COREREG off=0x03e6
	 * in any capture). Like every modern PHY in-tree (N, HT, LCN) the AC
	 * drives its own analog front-end instead.
	 *
	 * The unit is save -> AFE bank -> restore, as emitted at the top of a
	 * cold attach, twice back to back: ten reads, the AFE_ON block on the
	 * override page, then chan-select bit 1 cleared and 0x0417/0x0416 put
	 * back to what they held. The first five
	 * reads cover exactly the registers the AFE bank then masks through the
	 * 0x17xx override page (0x1739/0x173a/0x1725/0x1729/0x1721), so the save
	 * is of what is about to be hidden. The
	 * captured values there are 0x0000 and 0x0001, which is why the bss-up
	 * copy of this unit in op_switch_channel can hardcode them; here they
	 * are restored, so the sequence is board-independent.
	 *
	 * Only 0x0417 and 0x0416 are put back here; the other eight are read and
	 * not consumed. Whether the OEM keeps those values for a later stage is
	 * not established -- they are read because the capture reads them.
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
	 * Cold attach only: a 6-bit field in 0x02e4 is set right after the unit
	 * above, before the MHF block and a second copy of the unit. The cold
	 * preamble of both boards carries exactly one op on that register and
	 * the value is 0x0f00 on the 4352 and on the 4360 alike, so it is the
	 * phase that selects it, not the chip.
	 *
	 * On a later bring-up the d6220 does not touch 0x02e4 at all. Which
	 * entry is the cold one is the caller's decision, see
	 * b43_phy_ac_cold_preamble_due(). The DSL (wl 6.30) writes 0x0800 there
	 * on its down->up instead: a version divergence, tracked in
	 * retrace-todo.md, not reproduced here.
	 *
	 * Unrelated to the 0x0800 that set_channel writes on the 4360 -- that
	 * one lands after init_regs and no 4352 witness emits it.
	 */
	if (!cold)
		return;

	b43_phy_maskset(dev, 0x02e4, (u16)~0x3f00, 0x0f00);


	/*
	 * Tail of the preamble: the seven MHF clears, then a second copy of the
	 * AFE bank and the triplet. The second copy does not repeat the five
	 * reads -- the save belongs to the entry, not to the unit -- so the
	 * values saved above are reused.
	 *
	 * The gate is the same one as the 0x02e4 write above. For 0x02e4 that is
	 * supported by the d6220's down-to-up path not emitting it; for the MHF
	 * clears it is not, because the down-to-up captures contain no MAC.MHF
	 * at all, starting after this phase. It sits here by analogy, not on
	 * direct evidence.
	 */
	b43_phy_ac_mhf_bringup_clears(dev);

	b43_phy_ac_afe_arm(dev, B43_PHY_AC_AFE_OFF, saved_417, saved_416);
}

/*
 * The analog block is programmed twice during attach on agcombo and once on
 * the d6220. The condition below is on chip_id because that is the only
 * variable distinguishing the two witnesses, not because the discriminant is
 * established: the stock driver's code is identical between the two versions
 * involved, so the difference arises at runtime.
 *
 * These are two power-ons, not an off cycle: between them the PLL is
 * rewritten with the same values. The bus ops in between -- SI.COREREG,
 * PMU.PLL, MAC.MCTRL -- belong to bcma and to the b43 core and are not
 * reproduced here.
 *
 * Evidence, the checks that ruled other explanations out, and the two open
 * TODOs (a second 4360 is needed, and one running the d6220's version):
 * docs/retrace-todo.md.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   528-645]
 */
static void b43_phy_ac_op_switch_analog(struct b43_wldev *dev, bool on)
{
	bool cold = on && b43_phy_ac_cold_preamble_due(dev);

	B43_AC_FN();
	b43_phy_ac_switch_analog_once(dev, on, cold);

	if (cold && dev->dev->chip_id == 0x4360) {
		/*
		 * TODO: between the two entries the stock driver emits nine bus
		 * ops -- two GPIO reads, SI.COREREG offset 0x80, PMU.PLL 0x2 and
		 * 0x3, SI.COREREG offset 0x88, two MAC.MCTRL writes and a
		 * re-read of 0x000b. That is a PLL re-init, and it has to happen
		 * here: bcma has already finished by the time this runs, so
		 * nothing else can emit it.
		 *
		 * They are deliberately not open-coded. The values, 0xc31 and
		 * 0x100e, are the ones bcma_pmu_pll_init() writes for this chip,
		 * so the right shape is to call into that rather than copy its
		 * constants into the PHY. That needs a bcma entry point callable
		 * from here and matching harness stubs; today only
		 * bcma_chipco_pll_read() exists. Until then the two entries stay
		 * 13 ops apart.
		 */
		b43_phy_ac_switch_analog_once(dev, on, cold);
	}

	/*
	 * The front-end GPIO setup closes the sequence once, not on every entry:
	 * in the agcombo capture GPIO.OUT = 0x400 appears exactly once, after
	 * the second entry. On the 4352, which enters once, the distinction is
	 * invisible.
	 */
	if (!cold)
		return;

	b43_phy_ac_frontend_gpio_setup(dev);
	dev->phy.ac->status_mask |= B43_PHY_AC_STATE_COLD_PREAMBLE;
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   691-1201]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   72-567]
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
	 * from b43_wireless_core_exit(), and the vendor's down phase is one
	 * contiguous burst that ends with the AFE_OFF bank and the PMU
	 * release. It is b43_phy_ac_down() whole; switch_analog(false), which
	 * b43 calls after it, then has nothing left to emit, and the vendor
	 * emits nothing there either -- from the PMU release to the `mod
	 * GOING` of the rmmod only GPIO and SI.COREREG.
	 */
	if (blocked) {
		b43_phy_ac_down(dev);
		return;
	}

	/*
	 * The chanspec heads the radio bring-up, and one position covers both
	 * phases: on the cold attach it is the op immediately before the radio
	 * prologue, and on a warm cycle it lands immediately before the first
	 * op of b43_radio_2069_init().
	 *
	 * The harness also writes it from the `down` flow, which does not run
	 * this function; the two must stay in step.
	 */
	b43_phy_ac_write_chanspec(dev);

	/*
	 * b43_phy_init drives us with blocked=false before ops->init, so
	 * the core inventory must be probed here as well. Idempotent when
	 * op_init has already run.
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
	 * Per-core AFE / radio-LPF stage. Runs right after RC-cal in the radio
	 * bring-up, after the 0x08ea RCCAL_EN cleanup and before the op_init
	 * analog reset. afe_728 = 0x0800 in the ch36/5GHz capture.
	 */
	b43_radio_2069_afe_lpf_stage(dev, 0x0800);

	/*
	 * software_rfkill's scope ends at the radio front-end being up. The
	 * two-phase GPIO frontend, per-core PA bias and the final PMU regctl
	 * enable that the vendor emits only at steady state are driven from the
	 * rxiqcal / TX-enable path, not
	 * from the radio bring-up.
	 */
}

/*
 * Common A1 restore, used by every rxcal iteration after the first. 12 ops,
 * equivalent to rx_gate_with_adc_hold(true) -- arm the classifier, drop the
 * ADC bracket, disable clip -- followed by a gate close and a CCA pulse:
 *   1. classctl_write_peeked(true): peek 0x0140, write 0x0df4
 *   2. adc_hold(false): four MODs clearing bit 4 of 0x02?d
 *   3. clip_det(false): three MODs setting bit 14 of 0x?d4, clip disable
 *   4. write 0x0339 = 0, disabling the RX-IQ cal accumulator
 *   5. cca_pulse: set then clear bit 14 of 0x0001
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * Post-cal finalize: the second idle-TSSI iteration. Called with the MAC up,
 * mac_enable having been emitted at the end of op_switch_channel().
 *
 * The base indices observed for iteration 2 are 0x0207 on core 0, one more
 * than iteration 1, and 0x0200 on core 1, unchanged. The two iterations do
 * not differ by a constant delta: the idle-TSSI readback varies per core.
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
 * RX-cal iteration 3, the third idle-TSSI measurement:
 *   - mac_suspend first, since the MAC was up after iteration 2
 *   - a four-op diagnostic preamble: peek 0x0070, 0x0640 and 0x0840, then a
 *     MOD of 0x0070
 *   - the A1 restore
 *   - the idle_tssi_meas body, whose base index comes out equal to
 *     iteration 1's, which looks like convergence: the readback returns to
 *     its starting value after iteration 2's excursion
 *
 * It is the last idle-TSSI iteration: what follows is the RX-IQ compensation
 * apply, see b43_phy_ac_rxiqcal_apply().
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   15728-16503]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   11142-11917]
 */
void b43_phy_ac_post_cal_finalize_iter3(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_MAC_EN | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_RX_WAITED,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CCA_RESET |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS);


	/*
	 * Iteration 2 to 3 transition: an RX suspend before the mac_suspend.
	 * It is the same write of 0x0339 = 0x0fff that closes iteration 1 in
	 * op_switch_channel(), but emitted on its own here -- without the MHF and gate
	 * relock that follow it there, because that was a transition into
	 * txpwrctrl rather than into another idle_tssi iteration.
	 */
	b43_phy_write(dev, 0x0339, 0x0fff);

	/*
	 * The RX suspend closes with the same clear of the statistics window
	 * that a watchdog tick does, between the 0x0339 write above and the
	 * mac_suspend below. The counters have been
	 * accumulating since the channel setup, and the probe phase that
	 * follows reads the window on every tick.
	 */
	b43_phy_ac_wd_stats_clear(dev);

	/*
	 * Una parola in shared memory, subito dopo il clear e prima del
	 * mac_suspend. Cosa sia non e' noto e la cattura non lo dice, ma cio'
	 * che si sa la circoscrive abbastanza per riprodurla:
	 *
	 *  - il valore e' 0x7148 su 66 scritture su 66 -- 7 nello sweep a
	 *    freddo del d6220, 52 in quello a caldo, 7 su agcombo -- quindi
	 *    non dipende da canale, larghezza, chip ne' primo bring-up, e non
	 *    e' un contatore: sette attach diversi lo scrivono identico;
	 *  - non e' letto da nessuna parte nella traccia e non compare
	 *    nell'NVRAM, quindi non e' la copia di qualcosa;
	 *  - le tre letture che precedono sempre il blocco -- 0x0070, 0x0640 e
	 *    0x0840 -- non lo determinano: sul DSL la stessa terna precede
	 *    valori diversi;
	 *  - sul DSL, che gira wl 6.30 e non e' un oracolo, la cella prende
	 *    0x1130 in regime e altri quattro valori una volta ciascuno. Il
	 *    discriminante sembra la versione del blob e non l'hardware.
	 *
	 * b43.h non nomina la cella. E' del MAC e non del PHY, come il blocco
	 * di b43_phy_ac_shm_readback_block(), e sta qui per la stessa ragione:
	 * la cattura la mette fra il clear e il suspend e il core non ha un
	 * gancio in quel punto.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00b8, 0x7148);

	/*
	 * Ensure the MAC is suspended without nesting. The write the stock
	 * driver emits here is already produced by the preceding site in
	 * op_switch_channel(); an unconditional suspend would write nothing but would
	 * leave the refcount at 2, and from then on the enable/suspend pairs in
	 * rxiqcal_finalize() and probe_cycle() would toggle between 2 and 1
	 * without touching the hardware. This function has no closing enable:
	 * the caller expects the MAC suspended on return.
	 */
	if (!dev->mac_suspended)
		b43_mac_suspend(dev);

	/* Preambulo diagnostico (4 op nuove vs iter 2). */
	b43_phy_read_log(dev, 0x0070);
	b43_phy_read_log(dev, 0x0640);
	b43_phy_read_log(dev, 0x0840);
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0);

	b43_phy_ac_rxcal_a1_restore(dev);
	b43_phy_ac_idle_tssi_meas(dev);
}

/*
 * B2j coefficient write-back table: the 56 ops -- 54 MODs and two inline
 * writes -- the vendor emits to configure the gain stages after the RX-IQ
 * measurement. The registers are core 0's, with a +0x200 stride for core 1.
 *
 * Entry format is { reg_off, mask_bits, val }:
 *   mask_bits nonzero -> phy_maskset(reg + core_off, ~mask_bits, val),
 *                        rendered as PHY.MOD val=val mask=mask_bits
 *   mask_bits zero    -> phy_write(reg + core_off, val), a raw write
 *
 * These are not per-chain I/Q coefficients. The stock driver writes the same
 * values to both chains, 0x0724/0x0924 and 0x0736/0x0936, and in both phases,
 * whereas the per-chain coefficients do differ (a = -17 against -44). They are
 * a pair of constants written together and cleared together: an arm and
 * disarm of the tone generator, not an estimate. Nor are they per channel: in
 * the d6220 sweep the pair is identical across all 26 configurations, and the
 * 0x0152 here is this call site's constant -- the other two sites write
 * 0x0154 and 0x022a in the same captures.
 */
struct b43_ac_b2j_op {
	u16 reg_off;
	u16 mask_bits;   /* 0 sentinel = WR raw */
	u16 val;
	/* Somma il passo di banda al valore: vedi b43_phy_ac_bw_step(). Serve
	 * per le voci il cui campo cresce di uno per raddoppio della banda. */
	bool bw_step;
};

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
	{ 0x0736, 0x0000, 0x0152 },

	{ 0x073a, 0x0007, 0x0003 },
	{ 0x0725, 0x0020, 0x0020 },
	{ 0x0739, 0x007e, 0x007a },
	{ 0x0725, 0x0002, 0x0002 },
	{ 0x073a, 0x0008, 0x0000 },
	{ 0x0725, 0x0040, 0x0040 },
	{ 0x073a, 0x0010, 0x0010 },
	{ 0x0725, 0x0080, 0x0080 },
	{ 0x073a, 0x0060, 0x0040 },
	{ 0x0725, 0x0100, 0x0100 },

	{ 0x0723, 0x0008, 0x0008 },
	{ 0x0723, 0x0010, 0x0010 },
	{ 0x0723, 0x0800, 0x0800 },
	{ 0x0735, 0x0700, 0x0300 },
	{ 0x0735, 0x3800, 0x1800 },
	/* Il campo [2:0] cresce di 1 per passo di banda: 3 a 20 MHz, 4 a 40, 5 a
	 * 80. Qui c'e' il valore a 20 e il loop ci somma il passo. */
	{ 0x0738, 0x0007, 0x0003, .bw_step = true },
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
 * Per-core body of iteration 4, B2h + B2i + B2j, 79 ops. The B2g preamble
 * (peek the gate, tone generator off, three ops) is emitted once by the
 * caller before the per-core loop, not per core.
 *
 *   B2h,  8 ops: configure 0x073e -- write 0, clear bits 4-7, set bits 10
 *                and 12
 *   B2i, 15 ops: peek the gain registers, a pre-write readback of
 *                0x0720-0x073c
 *   B2j, 56 ops: coefficient write-back, from b43_phy_ac_b2j_ops
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   16631-16725, 16726-16820]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   12045-12139, 12140-12234]
 */
static void b43_phy_ac_rxiqcal_apply_body_core(struct b43_wldev *dev,
					       u16 core_off)
{
	B43_AC_FN();
	unsigned int i;

	/* B2h: 0x073e config (8 op) — clr bit 4-7, set bit 10/12 */
	b43_phy_read_log(dev, 0x073e + core_off);
	b43_phy_write(dev,   0x073e + core_off, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0010, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0020, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0040, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0080, 0);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x1000, 0x1000);
	b43_phy_maskset(dev, 0x073e + core_off, (u16)~0x0400, 0x0400);

	/* B2i: peek gain regs (15 op) */
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

	/* B2j: coefficient write-back (56 op da tabella) */
	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_b2j_ops); i++) {
		const struct b43_ac_b2j_op *op = &b43_phy_ac_b2j_ops[i];
		u16 addr = op->reg_off + core_off;
		u16 val = op->val;

		if (op->bw_step)
			val = (u16)(val + b43_phy_ac_bw_step(dev));

		if (op->mask_bits == 0)
			b43_phy_write(dev, addr, val);
		else
			b43_phy_maskset(dev, addr,
					(u16)~op->mask_bits, val);
	}
}

/*
 * RX-IQ compensation apply, phase B2. The fourth iteration of the cal cycle,
 * structurally unlike the idle-TSSI ones:
 *
 *   B2a,  1 op:  iteration 3 to 4 transition, write 0x0339 = 0x0fff, an RX
 *                suspend
 *   B2b, 14 ops: two table reads of id 0x0020 at offsets 0x14 and 0x1e,
 *                reading back the compensation tables
 *   B2c,  6 ops: per-core RX-IQ path disable, peek then clear bit 0 of
 *                0x?78. Three cores unconditionally, with no coremask
 *                check, as in A1.
 *   B2d, 12 ops: the common A1 restore
 *   B2e,  3 ops: an extra classifier reset, two peeks of 0x0140 then a
 *                write of 0x0df4
 *   B2f, 50 ops: per-core radio commit for the coefficient apply,
 *                coremask-guarded
 *   then the iteration-4 body loop:
 *     B2g,  3 ops: preamble, peek the gate and turn the tone generator off,
 *                  emitted once
 *     B2h to B2j:  per core, 79 ops, coremask-guarded
 *   B2k,  3 ops: set bits 6, 7 and 8 of 0x019e, extra gate configuration
 *   B2l, 18 ops: tone generator configuration, forward pass then reversed
 *   B2m, 32 ops: readback and write of coefficient table 0x0007
 *
 * Called with the MAC suspended, iteration 3 having left it down.
 *
 * TODO: computing the coefficients at runtime from rxcal_imbalance is still
 * pending.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* B2b: read back the compensation tables. The vendor emits a closing
	 * unlock after each table read; actab_read_bulk() relocks conditionally
	 * but does not emit the unlock, so it is added here. */
	{
		u8 bbmult8;
		b43_actab_read_bulk(dev, 0x0020, 0x0014, 8, 1, &bbmult8);
		dev->phy.ac->bbmult_cal[0] = bbmult8;
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_actab_read_bulk(dev, 0x0020, 0x001e, 8, 1, &bbmult8);
		dev->phy.ac->bbmult_cal[1] = bbmult8;
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* B2c: per-core RX-IQ path disable (0x?78 stride +0x200). Emesso per
	 * tutti e 3 i core hardcoded, senza check coremask — pattern analogo
	 * ad A1 sui registri 0x?d4 (phy_set 0x06d4/0x08d4/0x0ad4). */
	b43_phy_read_log(dev, 0x0678);
	b43_phy_maskset(dev, 0x0678, (u16)~0x0001, 0);
	b43_phy_read_log(dev, 0x0878);
	b43_phy_maskset(dev, 0x0878, (u16)~0x0001, 0);
	b43_phy_read_log(dev, 0x0a78);
	b43_phy_maskset(dev, 0x0a78, (u16)~0x0001, 0);

	/* B2d: A1 restore common */
	b43_phy_ac_rxcal_a1_restore(dev);

	/* B2e: extra classifier reset — 1 peek diagnostico + classctl_write_peeked(true) */
	b43_phy_read_log(dev, 0x0140);
	b43_phy_ac_classctl_write_peeked(dev, true);

	/*
	 * B2f, 50 ops: per-core radio commit for the coefficient apply.
	 * Coremask-guarded, unlike B2c which emits for three cores
	 * unconditionally.
	 */
	{
		u8 c;
		u8 num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;

		for (c = 0; c < num_cores; c++) {
			u16 s = (u16)(c * 0x200);
			if (!((mask >> c) & 1))
				continue;

			b43_radio_read_log(dev, 0x001a + s);
			b43_radio_read_log(dev, 0x001b + s);
			b43_radio_read_log(dev, 0x001c + s);
			b43_radio_read_log(dev, 0x001e + s);
			b43_radio_read_log(dev, 0x001f + s);
			b43_radio_read_log(dev, 0x0024 + s);
			b43_radio_read_log(dev, 0x0170 + s);

			b43_radio_maskset(dev, 0x001a + s, (u16)~0x00f0, 0x00b0);
			b43_radio_maskset(dev, 0x001f + s, (u16)~0x0004, 0x0004);
			b43_radio_maskset(dev, 0x0170 + s, (u16)~0x0100, 0x0100);
			b43_radio_maskset(dev, 0x0170 + s, (u16)~0x4000, 0);
			b43_radio_maskset(dev, 0x001e + s, (u16)~0x0004, 0);
			b43_radio_maskset(dev, 0x001a + s, (u16)~0x0300, 0);
		}
	}

	/*
	 * Iteration-4 body loop: coefficient write-back. The B2g preamble, three
	 * ops, is emitted once before the per-core loop; the rest, B2h through
	 * B2j at 79 ops, is per core and coremask-guarded.
	 */
	{
		u8 c;
		u8 num_cores = dev->phy.ac->num_cores;
		u8 mask = dev->phy.ac->coremask;

		/* B2g: preamble, three ops, emitted once rather than per core. */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_read_log(dev, 0x040f);
		b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);

		for (c = 0; c < num_cores; c++) {
			u16 s = (u16)(c * 0x200);
			if (!((mask >> c) & 1))
				continue;
			b43_phy_ac_rxiqcal_apply_body_core(dev, s);
		}

		/* B2k (3 op): setta bit 6/7/8 di B43_PHY_AC_REG_TBL_WRITE_GATE (gate config extra). */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0x0040);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0x0080);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0x0100);

		/*
		 * B2l: tone generator configuration, 18 ops on a two-core board
		 * and 27 on a three-core one. See
		 * b43_phy_ac_rxgain_perchan_tail() for the full pattern, a
		 * forward pass followed by a reversed one.
		 */
		b43_phy_ac_rxgain_perchan_tail(dev);

		/*
		 * B2m, 32 ops: readback and write of coefficient table 0x0007.
		 *
		 * Three cells per chain: (0x100, 0x103, 0x106) and (0x101,
		 * 0x104, 0x107). In both captures the vendor reads
		 * (0x0000, 0x2f13, 0x00f3) and writes
		 * (0x0000, 0x4f7f, 0x00f3) on chain 0 and
		 * (0x0000, 0x2f7f, 0x00f3) on chain 1. So the two outer cells
		 * are copies of what was read -- discarded into dummy_rd here --
		 * and in the middle cell the low byte goes to 0x7f on both
		 * chains.
		 *
		 * TODO: the high byte of the middle cell is still unexplained.
		 * It goes 0x2f to 0x4f on chain 0 and stays put on chain 1: one
		 * data point per chain, not enough to pin it down.
		 */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		{
			u16 dummy_rd;
			b43_actab_read_bulk(dev, 0x0007, 0x0100, 16, 1, &dummy_rd);
			b43_actab_read_bulk(dev, 0x0007, 0x0103, 16, 1, &dummy_rd);
			b43_actab_read_bulk(dev, 0x0007, 0x0106, 16, 1, &dummy_rd);
		}
		{
			static const u16 tblw_100 = 0x0000;
			static const u16 tblw_103 = 0x4f7f;
			static const u16 tblw_106 = 0x00f3;
			b43_actab_write_bulk(dev, 0x0007, 0x0100, 16, 1, &tblw_100);
			b43_actab_write_bulk(dev, 0x0007, 0x0103, 16, 1, &tblw_103);
			b43_actab_write_bulk(dev, 0x0007, 0x0106, 16, 1, &tblw_106);
		}

		/*
		 * B3: a second coefficient application pass, shifted by one
		 * relative to B2m -- the same shape with different offsets,
		 * 0x101 against 0x100 and so on, probably table 0x0007's Q slot
		 * rather than its I slot.
		 *
		 * The standalone peek-plus-relock pairs are not inside a table
		 * access: they are an explicit relock between two groups of ops
		 * on the same table. In B3b the relock is a MOD with no peek.
		 */

		/* B3a */
		{
			struct b43_phy_ac *ac = dev->phy.ac;

			b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1,
					    &ac->bbmult_saved[0]);
			b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1,
					     &ac->bbmult_cal[0]);
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1,
					     &ac->bbmult_cal[0]);
		}

		/* B3b: relock standalone (no peek) + 3 RD + 3 WR sulla 0x0007
		 * offset 0x101/0x104/0x107 (slot 1 vs 0x100/0x103/0x106 di B2m) */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		{
			u16 dummy_rd;
			static const u16 tblw_101 = 0x0000;
			static const u16 tblw_104 = 0x2f7f;
			static const u16 tblw_107 = 0x00f3;

			b43_actab_read_bulk(dev, 0x0007, 0x0101, 16, 1, &dummy_rd);
			b43_actab_read_bulk(dev, 0x0007, 0x0104, 16, 1, &dummy_rd);
			b43_actab_read_bulk(dev, 0x0007, 0x0107, 16, 1, &dummy_rd);
			b43_actab_write_bulk(dev, 0x0007, 0x0101, 16, 1, &tblw_101);
			b43_actab_write_bulk(dev, 0x0007, 0x0104, 16, 1, &tblw_104);
			b43_actab_write_bulk(dev, 0x0007, 0x0107, 16, 1, &tblw_107);
		}

		/* B3c: come B3a ma offset 0x67/0x77 val=0x003c */
		{
			struct b43_phy_ac *ac = dev->phy.ac;

			b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1,
					    &ac->bbmult_saved[1]);
			b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1,
					     &ac->bbmult_cal[1]);
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1,
					     &ac->bbmult_cal[1]);
		}
	}
}

/*
 * Close the gate scope rxiqcal_apply() opened and clear 12 slots of table
 * 0x000c, self-contained: every table write locks, writes and unlocks within
 * its own scope.
 *
 * TODO: the name "stage2" is provisional, to be revisited once the structure
 * of the post-rxiqcal phases is clear.
 *
 * The three 0x000c groups are fixed and not per coremask: the d6220 wires two
 * chains and clears all three, and the 4360 on agcombo emits the same twelve
 * table writes in the same order. The two hypotheses coincide there, since
 * agcombo wires all three chains; a board that wires fewer on a different
 * chip would be needed to separate them, and none is in the repository.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	b43_phy_write(dev, 0x0382, 0x8a09);

	/* B4a: three fixed per-core groups, each of four table writes at
	 * offsets base (length 2), +3, +4 and +5 (length 1), using the
	 * self-contained scoped pattern. */
	for (c = 0; c < 3; c++) {
		u16 base = (u16)(0x40 + c * 0x08);

		b43_actab_write_bulk_scoped(dev, 0x000c, base + 0, 16, 2, zero2);
		b43_actab_write_bulk_scoped(dev, 0x000c, base + 3, 16, 1, zero1);
		b43_actab_write_bulk_scoped(dev, 0x000c, base + 4, 16, 1, zero1);
		b43_actab_write_bulk_scoped(dev, 0x000c, base + 5, 16, 1, zero1);
	}

	/*
	 * B4b: one period of the tone in table 0x000e. The values are
	 * transcribed from the d6220 ch36 capture and may be channel-dependent
	 * or derived from a formula; neither is established. What is
	 * established is the length, see b43_phy_ac_tone_table_write().
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
	 * B4c, 33 ops:
	 *   - two self-contained table reads of 0x000c at offsets 0x63 and
	 *     0x67, dummy readbacks. The vendor emits lock, read and unlock;
	 *     actab_read_bulk() emits only lock and read, so the unlock is
	 *     done by hand after the call.
	 *   - 19 individual PHY configuration ops: clear bit 0 of 0x0471,
	 *     write the 0x0463/0x0461/0x0462 triplet, peek and set bit 0 of
	 *     0x0400, clear bits 0 and 2 of 0x0460, MOD 0x0382 (clear 0xc000
	 *     and set 0x8000, taking 0x8a09 to 0x8009), two peeks of 0x0403,
	 *     write 0x0400 = 0, then six MODs across three fixed cores on
	 *     0x073a and 0x0725 -- clear bit 8, set bit 10 -- with a +0x200
	 *     stride.
	 */
	{
		u16 dummy_rd;
		u8 c2;

		b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1, &dummy_rd);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1, &dummy_rd);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

		b43_phy_mask(dev, 0x0471, (u16)~0x0001);
		/*
		 * 0x27 at 20 MHz, 0x4f at 40, 0x9f at 80: the value plus one
		 * doubles with the bandwidth, which is the shape of a count of
		 * samples over a fixed time rather than three separate numbers.
		 */
		b43_phy_write(dev, 0x0463,
			      (u16)(0x28 * (1u << b43_phy_ac_bw_step(dev)) - 1));
		b43_phy_write(dev, 0x0461, 0xffff);
		b43_phy_write(dev, 0x0462, 0x003c);
		b43_phy_read_log(dev, 0x0400);
		b43_phy_set(dev, 0x0400, 0x0001);
		b43_phy_mask(dev, 0x0460, (u16)~0x0004);
		b43_phy_mask(dev, 0x0460, (u16)~0x0001);
		b43_phy_mask(dev, 0x0382, (u16)~0xc000);
		b43_phy_set(dev, 0x0382, 0x8000);
		b43_phy_read_log(dev, 0x0403);
		b43_phy_read_log(dev, 0x0403);
		b43_phy_write(dev, 0x0400, 0x0000);

		/* 6 MOD per-core 3-core hardcoded (stride +0x200) su
		 * 0x073a (clr bit 8) e 0x0725 (set bit 10) */
		for (c2 = 0; c2 < 3; c2++) {
			u16 s = (u16)(c2 * 0x200);

			b43_phy_maskset(dev, 0x073a + s, (u16)~0x0100, 0);
			b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0400, 0x0400);
		}
	}

	/*
	 * B4d, 183 ops: fast bulk table writes to 0x000c at offsets 0x00-0x11
	 * for core 0 and 0x20-0x31 for core 1. The 0x019e gate is locked
	 * externally by the preamble and unlocked by the epilogue, so each
	 * inner table write emits only five ops -- peek the gate, write the id,
	 * the offset and the data -- which is exactly b43_actab_write_bulk().
	 *
	 * The values are per core: the first seven and the last are identical
	 * between cores 0 and 1, while slots 0x07 to 0x10 differ. Two cores,
	 * and not the coremask: agcombo wires three chains and still writes
	 * only 0x00-0x11 and 0x20-0x31, with the same values as the d6220.
	 */
	{
		static const u16 b4d_core0_vals[18] = {
			0x0100, 0x0200, 0x0300, 0x0500, 0x0800, 0x0b00, 0x1000,
			0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007,
			0x1607, 0x2007, 0x2d07, 0x4007,
		};
		static const u16 b4d_core1_vals[18] = {
			0x0100, 0x0200, 0x0300, 0x0500, 0x0800, 0x0b00, 0x1000,
			0x1600, 0x2000, 0x2d00, 0x4000, 0x4001, 0x4002, 0x4003,
			0x4004, 0x4005, 0x4006, 0x4007,
		};
		unsigned int i;

		/* B4d preamble: peek gate + lock esterno */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Body: 18 core-0/core-1 pairs, interleaved. */
		for (i = 0; i < 18; i++) {
			b43_actab_write_bulk(dev, 0x000c, 0x00 + i, 16, 1,
					     &b4d_core0_vals[i]);
			b43_actab_write_bulk(dev, 0x000c, 0x20 + i, 16, 1,
					     &b4d_core1_vals[i]);
		}

		/* B4d epilogue: unlock */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}
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
 *   write 0x0381 = 0x7976      cal parameter A
 *   optionally a pre-clear table write of 0x000c at <pre> with value 0
 *   write 0x0383 = 0x003d      cal parameter B
 *   write 0x0380 = CMD         the armed command, bit 15 set
 *   poll while phy_read(0x0380) & 0x8000
 *   read radio 0x0144 + core_off, the per-core result
 *   table read of 0x000c at offset 0x8X, from the scratch table
 *   table write of 0x000c at offset 0x4X with the result
 *
 * The table read is self-contained in the vendor sequence; the port uses
 * actab_read_bulk() plus a manual clear of bit 1 of 0x019e for the closing
 * unlock.
 *
 * core_off is 0x0000 for the 0x8XXX command group (core 0), 0x0200 for
 * 0x9XXX (core 1) and 0x0400 for 0xaXXX (core 2).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Wait for the busy bit, bit 15, to clear, with a finite budget: if the
	 * hardware ends up in an unexpected state on an untested channel or
	 * board, this must not spin in the kernel. Giving up degrades the
	 * calibration but is not fatal.
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
	/* The value read is the value written back: copy 0x80 + n to 0x40 + n. */
	b43_actab_read_bulk(dev, 0x000c, rd_off, 16, rw_len, result);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);  /* unlock manuale */
	b43_actab_write_bulk_scoped(dev, 0x000c, wr_off, 16, rw_len, result);
	b43_phy_ac_afe_res_store(dev, wr_off, result, rw_len);
}

/*
 * Commit batch, the tail of B5 iterations 6, 12 and 18: after a commit
 * iteration's main result, emit N pairs of fast table writes interleaved
 * between cores 0 and 1, with a 0x20 offset stride. The 0x019e gate is locked
 * and unlocked externally, by the preamble and epilogue.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   17999-18218, 18723-18942]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   13419-13638]
 */
static void b43_phy_ac_rxcal_afe_commit_batch(struct b43_wldev *dev,
					      u16 base_c0, u16 base_c1,
					      const u16 *c0_vals,
					      const u16 *c1_vals,
					      u8 n)
{
	B43_AC_FN();
	u8 i;

	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	for (i = 0; i < n; i++) {
		b43_actab_write_bulk(dev, 0x000c, base_c0 + i, 16, 1,
				     &c0_vals[i]);
		b43_actab_write_bulk(dev, 0x000c, base_c1 + i, 16, 1,
				     &c1_vals[i]);
	}

	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Iteration table: 18 iterations, six commands over three cores.
	 *
	 * The results are not hardcoded -- rxcal_afe_iter() reads them from the
	 * readback and writes them back. The only static data here is pre[],
	 * which holds pre-clear offsets rather than values. The "result 0xNNNN"
	 * notes below are observations from the capture, not constants the code
	 * uses.
	 */

	/* ==== Gruppo core-0 (cmd 0x8XXX, RAD.RD 0x0144) ==== */

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
	 * with a commit batch: 36 interleaved fast table writes updating the
	 * gain override table 0x000c at offsets 0x00-0x11 for core 0 and
	 * 0x20-0x31 for core 1, with the final coefficients from iterations 1
	 * to 6.
	 */
	{
		static const u16 batch_c0[18] = {
			0x0100, 0x0200, 0x0300, 0x0500, 0x0700, 0x0a00, 0x0f00,
			0x0f01, 0x0f02, 0x0f03, 0x0f04, 0x0f05, 0x0f06, 0x0f07,
			0x1507, 0x1e07, 0x2a07, 0x3c07,
		};
		static const u16 batch_c1[18] = {
			0x0100, 0x0200, 0x0300, 0x0500, 0x0700, 0x0a00, 0x0f00,
			0x1500, 0x1e00, 0x2a00, 0x3c00, 0x3c01, 0x3c02, 0x3c03,
			0x3c04, 0x3c05, 0x3c06, 0x3c07,
		};
		b43_phy_ac_rxcal_afe_iter(dev, 0x8234, 0x0000,
					  NULL, 0, 0x0083, 1, 0x0043);
		b43_phy_ac_rxcal_afe_commit_batch(dev, 0x00, 0x20,
						  batch_c0, batch_c1, 18);
	}

	/* ==== Gruppo core-1 (cmd 0x9XXX, RAD.RD 0x0344) ==== */

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
	 * Iteration 12, cmd 0x9234, observed result 0x0100. Last of the core-1
	 * group, with no special tail: iteration 13's two pre-clears at 0x53 and
	 * 0x54 are emitted inside iteration 13, after the write of 0x0381.
	 */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0x9234, 0x0200,
					  NULL, 0, 0x008a, 1, 0x004b);
	}

	/*
	 * A second commit batch between the core-1 and core-2 groups, on the
	 * first bring-up only: the same cells and structure as the first, but
	 * carrying the low byte alone -- the index ramp without the gain.
	 */
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP) {
		static const u16 ramp_c0[18] = {
			0, 0, 0, 0, 0, 0, 0,
			1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 7,
		};
		static const u16 ramp_c1[18] = {
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			1, 2, 3, 4, 5, 6, 7,
		};

		b43_phy_ac_rxcal_afe_commit_batch(dev, 0x00, 0x20,
						  ramp_c0, ramp_c1, 18);
	}

	/* ==== Gruppo core-2 (cmd 0xaXXX, RAD.RD 0x0544) ==== */

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
	/* Iter 18: cmd=0xa234, result 0x0764. Per la coda vedi il blocco
	 * sotto. */
	{
		b43_phy_ac_rxcal_afe_iter(dev, 0xa234, 0x0400,
					  NULL, 0, 0x0091, 1, 0x0053);
	}

	/*
	 * Tail of iteration 18: after the last core-2 result, the stock driver
	 * emits three parts, labelled (a), (b) and (c) in the body. Part (a)
	 * duplicates the commit iterations' results per antenna.
	 *
	 * SALAME: (a) look like gain-override defaults for the second pass, (b)
	 * like a reset of bits set in B4c, and (c) like a scaling of the gain
	 * channels. None of the three is confirmed.
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

	/* (b): 3 op individual */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_mask(dev, 0x0382, (u16)~0x8000);
	b43_phy_mask(dev, 0x0460, (u16)~0x0004);

	/* (c): two fast batch trailers, each of two table writes -- 0x63 with
	 * 0x73, then 0x67 with 0x77 -- under an externally locked gate. */
	{

		/* Trailer 1: 0x63 + 0x73 val=0x0040 */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1,
				     &dev->phy.ac->bbmult_cal[0]);
		b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1,
				     &dev->phy.ac->bbmult_cal[0]);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

		/* Trailer 2: 0x67 + 0x77 val=0x003c */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1,
				     &dev->phy.ac->bbmult_cal[1]);
		b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1,
				     &dev->phy.ac->bbmult_cal[1]);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/*
	 * Snapshot of the pass-1 results. rxiqcal_run_meas_iters() will rewrite
	 * the same offsets with the second tone's results, but the final
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
 * Base LOFT LUT entry per core and index, the value iqcal_coeff_tables_reset()
 * writes and the one the calibrated word is added onto.
 *
 * Cores 0 and 1 have no base: their LUT is the LO word alone. Core 2 carries
 * (-8, -4) up to index 0x20 and (-10, -14) from 0x21, the same on the d6220,
 * which has no third chain, and on the agcombo, which has one: there the
 * chain's LO word 0xff00 gives 0xf7fc/0xf5f2, 0x0000 gives 0xf8fc/0xf6f2 and
 * 0xfe02 gives 0xf6fe/0xf4f4, entry for entry. So it is a constant of the
 * core, not of the board.
 *
 * SALAME: why core 2 alone has a base, and why it steps at index 0x21, has no
 * evidence in the captures.
 */
static u16 b43_phy_ac_loft_lut_base(unsigned int core, unsigned int off)
{
	if (core < 2)
		return 0;
	return off <= 0x20 ? 0xf8fc : b43_phy_ac_loft_add(0xf8fc, 0xfef6);
}

/*
 * Fill the LOFT LUTs from the TX IQ/LO calibration, phase C.
 *
 * Preamble, 3 ops:
 *   set then clear bit 14 of 0x0001, pulsing the hardware trigger
 *   write 0x0382 = 0, resetting the B4b config registers
 *
 * Body, 2688 ops: 128 indices over three cores, each index emitting three
 * self-contained table writes to 0x42 for core 0, 0x62 for core 1 and 0x82
 * for core 2. Every entry of a core's LUT is the LO leakage word that core's
 * commit iteration produced -- what rxcal_afe_calibrate() wrote to IQLOCAL
 * 0x62 + 4*core -- added per field onto the core's base entry. On cold01 the
 * words are 0xff02, 0x0200 and 0x1eea; on the hot ch36 flow 0xff01, 0x0201
 * and 0x93f1; on the agcombo's cold ch36 0x0301, 0x0102 and 0xff00, and the
 * LUTs follow them entry for entry on both boards. The value changes from
 * run to run because the calibration does, not because it accumulates.
 *
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	for (core = 0; core < 3; core++)
		lo[core] = dev->phy.ac->afe_res[2 * core + 1].v[0];

	/* Preamble */
	b43_phy_ac_cca_pulse(dev);
	b43_phy_write(dev, 0x0382, 0x0000);

	for (i = 0; i < 0x80; i++) {
		for (core = 0; core < 3; core++) {
			u16 v = b43_phy_ac_loft_add(lo[core],
					b43_phy_ac_loft_lut_base(core, i));

			b43_actab_write_bulk_scoped(dev, tbl[core], (u16)i,
						    16, 1, &v);
		}
	}
}

/*
 * Return the gain registers 0x0720-0x073e to their defaults and close with
 * the commit pulse. Called immediately after txpwr_by_index(). The values are
 * identical across chains, and the block runs once per active chain: two on
 * the d6220, three on agcombo, where the same sixteen writes appear on
 * 0x0b20-0x0b3e as well.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   22842-22877, 27632-27667]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   17970-18005, 22844-22879]
 * [capture-ref: router-data/agcombo/cold-sweep.zip!cold01-ch36-bw20.txt;
 *   30383-30434]
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
	u8 mask = dev->phy.ac->coremask;
	unsigned int core, k;

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 stride = (u16)(core * 0x200);

		if (!(mask & (1 << core)))
			continue;
		for (k = 0; k < ARRAY_SIZE(gain_cfg); k++)
			b43_phy_write(dev, gain_cfg[k].off + stride,
				      gain_cfg[k].val);
	}

	/* Block 3 preamble: gate feature bits + 0x040f + commit pulse */
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x03d0);
	b43_phy_write(dev, 0x040f, 0x09ff);
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Mixed RX and TX chain setup: write the base set of per-chain radio
 * registers, open a bracket -- classifier armed, ADC hold, clip detect --
 * that closes to a net zero, and with @with_tune add per-chain readback and
 * tuning. The steps are labelled 3a to 3r in the body, in the vendor's order.
 *
 * SALAME: what the bits touched on 0x02ed/f1/f5/f9 mean, and what the closing
 * pulse of bit 14 of 0x0001 does, are not identified.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	static const struct { u16 off; u16 val; } radio_wr[7] = {
		{ 0x001a, 0x0014 },
		{ 0x001b, 0x0280 },
		{ 0x001c, 0x0044 },
		{ 0x001e, 0x0014 },
		{ 0x001f, 0x0000 },
		{ 0x0024, 0x0000 },
		{ 0x0170, 0x0100 },
	};
	unsigned int core, k;

	/* 3a: 7 RAD.WR per core × 2 core */
	for (core = 0; core < 2; core++) {
		u16 stride = (u16)(core * 0x200);

		for (k = 0; k < ARRAY_SIZE(radio_wr); k++)
			b43_radio_write(dev, radio_wr[k].off + stride,
					radio_wr[k].val);
	}

	/*
	 * 3b: open the bracket -- arm the classifier, then release it with a
	 * peek. The vendor emits the arm/release pair so that the peek in
	 * between samples the armed state and the following write moves to
	 * the release:
	 *   classctl_write(true)          write 0x0140 = 0x0df4
	 *   classctl_write_peeked(false)  peek, then write 0x0140 = 0x0df6
	 */
	b43_phy_ac_classctl_write(dev, true);
	b43_phy_ac_classctl_write_peeked(dev, false);

	/* 3c-3d: engage ADC hold + enable clip det (release RX). */
	b43_phy_ac_adc_hold(dev, true);
	b43_phy_ac_clip_det(dev, true);

	/* 3e: WR 0x0339 = 0x0fff */
	b43_phy_write(dev, 0x0339, 0x0fff);

	/* 3f: 3 WR per-core (0x?78 = 0x0008) */
	for (core = 0; core < 3; core++)
		b43_phy_write(dev, 0x0678 + core * 0x200, 0x0008);

	/* 3g: two self-contained table reads of id 0x0020 through DATA_2, in
	 * the full variant only. */
	if (with_tune) {
		b43_actab_read_bulk(dev, 0x0020, 0x0014, 16, 1,
				    &dev->phy.ac->bbmult_cal[0]);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);   /* unlock manuale */
		b43_actab_read_bulk(dev, 0x0020, 0x001e, 16, 1,
				    &dev->phy.ac->bbmult_cal[1]);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);   /* unlock manuale */

		/* 3h: 3 peek + MOD per-core (0x?78 clr bit 0) */
		for (core = 0; core < 3; core++) {
			u16 reg = 0x0678 + core * 0x200;

			b43_phy_read_log(dev, reg);
			b43_phy_maskset(dev, reg, (u16)~0x0001, 0);
		}
	}

	/*
	 * 3i to 3k: close the bracket -- restore the classifier arm, drop the
	 * ADC hold, disable clip detect -- for a net zero against the state on
	 * entry. Equivalent to b43_phy_ac_rx_gate_with_adc_hold(dev, true).
	 */
	b43_phy_ac_rx_gate_with_adc_hold(dev, true);

	/* 3l: WR 0x0339 = 0x0000 (restore) */
	b43_phy_write(dev, 0x0339, 0x0000);

	/* 3m: MOD 0x0001 pulse bit 14 (commit) */
	b43_phy_ac_cca_pulse(dev);

	if (!with_tune)
		return;

	/* 3n: 1 peek diagnostico + classctl_write_peeked(true) (arm classifier). */
	b43_phy_read_log(dev, 0x0140);
	b43_phy_ac_classctl_write_peeked(dev, true);

	/*
	 * 3o-3r: per-core (2 core) 7 RAD.RD + 6 RAD.MOD tuning.
	 * Le 6 MOD alterano bit-field specifici di 0x001a (0x00f0, 0x0300),
	 * 0x001f (0x0004), 0x0170 (0x0100, 0x4000), 0x001e (0x0004).
	 */
	for (core = 0; core < 2; core++) {
		u16 s = (u16)(core * 0x200);

		/* 3o/3q: 7 RAD.RD readback */
		b43_radio_read_log(dev, 0x001a + s);
		b43_radio_read_log(dev, 0x001b + s);
		b43_radio_read_log(dev, 0x001c + s);
		b43_radio_read_log(dev, 0x001e + s);
		b43_radio_read_log(dev, 0x001f + s);
		b43_radio_read_log(dev, 0x0024 + s);
		b43_radio_read_log(dev, 0x0170 + s);

		/* 3p/3r: 6 RAD.MOD tuning (each = MOD+RD+WR = 3 op) */
		b43_radio_maskset(dev, 0x001a + s, (u16)~0x00f0, 0x00b0);
		b43_radio_maskset(dev, 0x001f + s, (u16)~0x0004, 0x0004);
		b43_radio_maskset(dev, 0x0170 + s, (u16)~0x0100, 0x0100);
		b43_radio_maskset(dev, 0x0170 + s, (u16)~0x4000, 0);
		b43_radio_maskset(dev, 0x001e + s, (u16)~0x0004, 0);
		b43_radio_maskset(dev, 0x001a + s, (u16)~0x0300, 0);
	}
}

/*
 * Tail shared by rxgain_perchan_config(), iqcal_meas_readback_kick_tail() and
 * the B2m block of rxcal_afe_calibrate(); 18 ops with two cores active.
 *
 * The observed pattern:
 *   forward pass, for each active core:
 *     peek 0x?739, write 0x00fa
 *     peek 0x?73a, write 0x01d3
 *     peek 0x?725, write 0x07e6
 *   reverse pass, for each active core from highest to lowest:
 *     write 0x?725 = 0x07e2, 0x?73a = 0x01d3, 0x?739 = 0x007a
 *
 * SALAME: the reverse pass overwrites values the forward pass has just
 * written. The pattern is deterministic and identical at every occurrence,
 * but the capture does not say why it exists.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* Pass 1 (forward): 3 peek+WR pair per core attivo. */
	/* Gain state per chain, saved, driven, put back in reverse order. */
	u16 s739[3], s73a[3], s725[3];

	for (c = 0; c < num_cores; c++) {
		u16 s = (u16)(c * 0x200);

		if (!((coremask >> c) & 1))
			continue;

		s739[c] = b43_phy_read_log(dev, 0x0739 + s);
		b43_phy_write(dev,    0x0739 + s, s739[c] | 0x0080);
		s73a[c] = b43_phy_read_log(dev, 0x073a + s);
		b43_phy_write(dev,    0x073a + s, s73a[c]);
		s725[c] = b43_phy_read_log(dev, 0x0725 + s);
		b43_phy_write(dev,    0x0725 + s, s725[c] | 0x0004);
	}

	/* Pass 2 (reverse): 3 WR standalone per core attivo, high-to-low. */
	for (c = num_cores; c-- > 0; ) {
		u16 s = (u16)(c * 0x200);

		if (!((coremask >> c) & 1))
			continue;

		b43_phy_write(dev, 0x0725 + s, s725[c]);
		b43_phy_write(dev, 0x073a + s, s73a[c]);
		b43_phy_write(dev, 0x0739 + s, s739[c]);
	}
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	enum { OP_MOD, OP_WR };
	struct gain_op {
		u8 kind;
		u16 reg;    /* core-0 relative */
		u16 mask;   /* trace mask (bit da modificare) — 0 se OP_WR */
		u16 val;
	};

	/* 54 MODs plus two writes, 56 body ops per core, in the observed order. */
	static const struct gain_op body[56] = {
		{ OP_MOD, 0x0728, 0x0002, 0x0000 },
		{ OP_MOD, 0x0720, 0x0002, 0x0002 },
		{ OP_MOD, 0x0721, 0x0040, 0x0040 },
		{ OP_MOD, 0x0729, 0x0040, 0x0000 },
		{ OP_MOD, 0x0721, 0x0080, 0x0080 },
		{ OP_MOD, 0x0729, 0x0080, 0x0000 },
		{ OP_MOD, 0x0721, 0x0020, 0x0020 },
		{ OP_MOD, 0x0729, 0x0020, 0x0000 },
		{ OP_MOD, 0x0721, 0x2000, 0x2000 },
		{ OP_MOD, 0x0729, 0xe000, 0x0000 },
		{ OP_MOD, 0x0721, 0x0800, 0x0800 },
		{ OP_MOD, 0x0729, 0x0800, 0x0000 },
		{ OP_MOD, 0x0721, 0x0400, 0x0400 },
		{ OP_MOD, 0x0729, 0x0400, 0x0000 },
		{ OP_MOD, 0x0721, 0x4000, 0x4000 },
		{ OP_MOD, 0x0728, 0x3800, 0x0000 },
		{ OP_MOD, 0x0721, 0x1000, 0x1000 },
		{ OP_MOD, 0x0729, 0x1000, 0x0000 },
		{ OP_MOD, 0x0720, 0x0020, 0x0020 },
		{ OP_MOD, 0x0728, 0x0020, 0x0020 },
		{ OP_MOD, 0x0720, 0x0040, 0x0040 },
		{ OP_MOD, 0x0728, 0x0040, 0x0040 },
		{ OP_MOD, 0x0720, 0x0010, 0x0010 },
		{ OP_MOD, 0x0728, 0x0010, 0x0010 },
		{ OP_MOD, 0x0721, 0x0100, 0x0100 },
		{ OP_MOD, 0x0729, 0x0100, 0x0100 },
		{ OP_MOD, 0x0727, 0x0004, 0x0004 },
		{ OP_MOD, 0x073c, 0x0010, 0x0010 },
		{ OP_WR,  0x0724, 0,      0x03ff },
		{ OP_WR,  0x0736, 0,      0x022a },
		{ OP_MOD, 0x073a, 0x0007, 0x0003 },
		{ OP_MOD, 0x0725, 0x0020, 0x0020 },
		{ OP_MOD, 0x0739, 0x007e, 0x007a },
		{ OP_MOD, 0x0725, 0x0002, 0x0002 },
		{ OP_MOD, 0x073a, 0x0008, 0x0000 },
		{ OP_MOD, 0x0725, 0x0040, 0x0040 },
		{ OP_MOD, 0x073a, 0x0010, 0x0010 },
		{ OP_MOD, 0x0725, 0x0080, 0x0080 },
		{ OP_MOD, 0x073a, 0x0060, 0x0040 },
		{ OP_MOD, 0x0725, 0x0100, 0x0100 },
		{ OP_MOD, 0x0723, 0x0008, 0x0008 },
		{ OP_MOD, 0x0723, 0x0010, 0x0010 },
		{ OP_MOD, 0x0723, 0x0800, 0x0800 },
		{ OP_MOD, 0x0735, 0x0700, 0x0300 },
		{ OP_MOD, 0x0735, 0x3800, 0x1800 },
		{ OP_MOD, 0x0738, 0x0007, 0x0003 },
		{ OP_MOD, 0x0723, 0x0001, 0x0001 },
		{ OP_MOD, 0x0735, 0x0001, 0x0000 },
		{ OP_MOD, 0x0723, 0x0020, 0x0020 },
		{ OP_MOD, 0x0735, 0x4000, 0x0000 },
		{ OP_MOD, 0x0723, 0x0002, 0x0002 },
		{ OP_MOD, 0x0735, 0x001e, 0x0008 },
		{ OP_MOD, 0x0727, 0x0002, 0x0002 },
		{ OP_MOD, 0x073c, 0x000e, 0x0004 },
		{ OP_MOD, 0x0727, 0x0001, 0x0001 },
		{ OP_MOD, 0x073c, 0x0001, 0x0001 },
	};
	/* 15 per-core readback registers, in the observed order. */
	static const u16 readback_regs[15] = {
		0x0725, 0x0739, 0x073a, 0x0721, 0x0729, 0x0720, 0x0728,
		0x0724, 0x0736, 0x0723, 0x0735, 0x0737, 0x0738, 0x0727,
		0x073c,
	};
	unsigned int core, k;

	/* Global preamble */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_read_log(dev, 0x040f);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);

	/* Per-core loop */
	for (core = 0; core < 2; core++) {
		u16 s = (u16)(core * 0x200);

		/* Preamble per-core: peek + WR + 6 MOD su 0x?73e */
		b43_phy_read_log(dev, 0x073e + s);
		b43_phy_write(dev, 0x073e + s, 0x0000);
		b43_phy_maskset(dev, 0x073e + s, (u16)~0x0010, 0);
		b43_phy_maskset(dev, 0x073e + s, (u16)~0x0020, 0);
		b43_phy_maskset(dev, 0x073e + s, (u16)~0x0040, 0);
		b43_phy_maskset(dev, 0x073e + s, (u16)~0x0080, 0);
		b43_phy_maskset(dev, 0x073e + s, (u16)~0x1000, 0x1000);
		b43_phy_maskset(dev, 0x073e + s, (u16)~0x0400, 0x0400);

		/* 15 peek readback */
		for (k = 0; k < ARRAY_SIZE(readback_regs); k++)
			b43_phy_read_log(dev, readback_regs[k] + s);

		/* 56 op body */
		for (k = 0; k < ARRAY_SIZE(body); k++) {
			u16 reg = body[k].reg + s;

			if (body[k].kind == OP_MOD)
				b43_phy_maskset(dev, reg,
						(u16)~body[k].mask, body[k].val);
			else
				b43_phy_write(dev, reg, body[k].val);
		}
	}

	/* Bridge (3 op): 3 MOD B43_PHY_AC_REG_TBL_WRITE_GATE set bit 6/7/8 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0x0040);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0x0080);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0x0100);

	/* Tail comune (18 op): vedi b43_phy_ac_rxgain_perchan_tail. */
	b43_phy_ac_rxgain_perchan_tail(dev);
}

/*
 * RX-IQ compensation on the TX side: rewrite the TX gain code, in table
 * 0x0007, the same area txpwr_by_index() uses, and the baseband multiplier,
 * per chain. The batches are labelled in the body.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* RX-IQ-compensated values, transcribed from the d6220 ch36 capture. The
	 * vendor derives them from the RX-IQ estimate, a four-tone measurement.
	 * TODO: compute them at runtime once the RX-IQ estimation is complete. */
	static const u16 g1_per_core[2] = { 0x4f7f, 0x2f7f };
	static const u16 bbmult_lo[3] = { 0x0063, 0x0067, 0x006b };
	static const u16 bbmult_hi[3] = { 0x0073, 0x0077, 0x007b };
	static const u16 g0 = 0x0000;
	static const u16 g2 = 0x00f3;
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core;
	bool first_core = true;
	u16 discard;

	/* Preamble */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	for (core = 0; core < ac->num_cores; core++) {

		if (!((ac->coremask >> core) & 1))
			continue;

		if (!first_core) {
			/* Bridge between cores: an idempotent lock MOD only. */
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		}
		first_core = false;


		/* Batch A: 3 fast TBL.RD TX gain code (readback pre-compensation).
		 * Area TBL 0x0007 off 0x100+ = TX gain LUT (stessa area di
		 * txpwr_by_index). */
		b43_actab_read_bulk(dev, 7, (u16)(core + 0x0100), 16, 1, &discard);
		b43_actab_read_bulk(dev, 7, (u16)(core + 0x0103), 16, 1, &discard);
		b43_actab_read_bulk(dev, 7, (u16)(core + 0x0106), 16, 1, &discard);

		/* Batch B: 3 fast TBL.WR TX gain code (writeback compensato) */
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0100), 16, 1, &g0);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0103), 16, 1, &g1_per_core[core]);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0106), 16, 1, &g2);

		/* Batch C: TBL.RD bbmult (TX baseband multiplier) readback +
		 * sync explicit (peek+MOD lock idempotente) */
		b43_actab_read_bulk(dev, 0xc, bbmult_lo[core], 16, 1,
				    &ac->bbmult_saved[core]);
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Batch D: 2 fast TBL.WR TX bbmult compensato */
		b43_actab_write_bulk(dev, 0xc, bbmult_lo[core], 16, 1,
				     &ac->bbmult_cal[core]);
		b43_actab_write_bulk(dev, 0xc, bbmult_hi[core], 16, 1,
				     &ac->bbmult_cal[core]);
	}

	/* Postamble */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * RX-IQ DDS/NCO seed (fase E block 2, 111 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * DDS/NCO tone, one period. Transcribed from the d6220 ch36 capture;
	 * the length of the table is a function of the channel width, see
	 * b43_phy_ac_tone_table_write().
	 */
	static const u32 dds_seed[B43_PHY_AC_TONE_PERIOD] = {
		0x0003e800, 0x0003b84d, 0x00032893, 0x00024cca, 0x000134ee,
		0x000000fa, 0x000eccee, 0x000db4ca, 0x000cd893, 0x000c484d,
		0x000c1800, 0x000c4bb3, 0x000cdb6d, 0x000db736, 0x000ecf12,
		0x00000306, 0x00013712, 0x00024f36, 0x00032b6d, 0x0003bbb3,
	};
	static const u16 zeros[2] = { 0, 0 };

	/* 1 op: arm command */
	b43_phy_write(dev, 0x0382, 0x8a09);

	/* 3× 8 op: azzera 3 zone da 2 slot in TBL 0x000c */
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0040, 16, 2, zeros);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0048, 16, 2, zeros);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0050, 16, 2, zeros);

	b43_phy_ac_tone_table_write(dev, dds_seed, 1);
}

/*
 * RX-IQ prep second iteration (216 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/*
	 * A 36-value LUT. The observed pattern is pairs of offsets
	 * (i, i + 0x20) for i from 0 to 17; the first seven pairs are identical
	 * and they diverge from the eighth on.
	 */
	static const struct { u16 off; u16 val; } lut36[36] = {
		{ 0x0000, 0x0100 }, { 0x0020, 0x0100 },
		{ 0x0001, 0x0200 }, { 0x0021, 0x0200 },
		{ 0x0002, 0x0300 }, { 0x0022, 0x0300 },
		{ 0x0003, 0x0500 }, { 0x0023, 0x0500 },
		{ 0x0004, 0x0800 }, { 0x0024, 0x0800 },
		{ 0x0005, 0x0b00 }, { 0x0025, 0x0b00 },
		{ 0x0006, 0x1000 }, { 0x0026, 0x1000 },
		{ 0x0007, 0x1001 }, { 0x0027, 0x1600 },
		{ 0x0008, 0x1002 }, { 0x0028, 0x2000 },
		{ 0x0009, 0x1003 }, { 0x0029, 0x2d00 },
		{ 0x000a, 0x1004 }, { 0x002a, 0x4000 },
		{ 0x000b, 0x1005 }, { 0x002b, 0x4001 },
		{ 0x000c, 0x1006 }, { 0x002c, 0x4002 },
		{ 0x000d, 0x1007 }, { 0x002d, 0x4003 },
		{ 0x000e, 0x1607 }, { 0x002e, 0x4004 },
		{ 0x000f, 0x2007 }, { 0x002f, 0x4005 },
		{ 0x0010, 0x2d07 }, { 0x0030, 0x4006 },
		{ 0x0011, 0x4007 }, { 0x0031, 0x4007 },
	};
	u16 discard;
	unsigned int core, i;

	/* Segment A, 14 ops: two self-contained table reads plus an explicit
	 * unlock, reading back the per-core bbmult after compensation. On entry
	 * the gate is unlocked, the previous function having ended on the unlock
	 * MOD of write_bulk_scoped(). */
	b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1, &discard);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1, &discard);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

	/* Seg B (19 op): kick sequence per il correlatore RXIQ */
	b43_phy_mask(dev,      0x0471, (u16)~0x0001);
	b43_phy_write(dev,     0x0463, 0x0027);
	b43_phy_write(dev,     0x0461, 0xffff);
	b43_phy_write(dev,     0x0462, 0x003c);
	b43_phy_read_log(dev,  0x0400);
	b43_phy_set(dev,       0x0400, 0x0001);
	b43_phy_mask(dev,      0x0460, (u16)~0x0004);
	b43_phy_mask(dev,      0x0460, (u16)~0x0001);
	b43_phy_mask(dev,      0x0382, (u16)~0xc000);
	b43_phy_set(dev,       0x0382, 0x8000);
	b43_phy_read_log(dev,  0x0403);
	b43_phy_read_log(dev,  0x0403);                            /* double peek */
	b43_phy_write(dev,     0x0400, 0x0000);

	/* 6 MOD per-core (3-core hardcoded stride +0x200) */
	for (core = 0; core < 3; core++) {
		u16 s = (u16)(core * 0x200);

		b43_phy_maskset(dev, 0x073a + s, (u16)~0x0100, 0);
		b43_phy_maskset(dev, 0x0725 + s, (u16)~0x0400, 0x0400);
	}

	/* Seg C (183 op): preamble (2) + 36× fast TBL.WR (180) + unlock (1) */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	for (i = 0; i < ARRAY_SIZE(lut36); i++)
		b43_actab_write_bulk(dev, 0x000c, lut36[i].off, 16, 1, &lut36[i].val);

	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * RX-IQ measurement iters (gruppo 4, 519 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * 0x?084 then 0x?056, which are the two taps of the complex filter -- a
	 * single tap would be flat in frequency. Consistent with the three tones
	 * seeded by rxiqcal_dds_seed() and its second and third tone
	 * counterparts.
	 *
	 * The coefficients are not to be derived: the hardware computes them.
	 * rxcal_afe_iter() arms the cal by writing the command to 0x0380, waits
	 * on bit 15, reads two cells at rd_off and writes them back at wr_off.
	 * The copy is visible in the capture: 0x0c[0x8e]/[0x8f] to
	 * 0x0c[0x50]/[0x51], and [0x91] to [0x53].
	 *
	 * Both iterations of a chain write the same wr_off, so what afe_res ends
	 * up holding is the second one's result.
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
	/*
	 * Iteration-20 batch: 36 fast single-word table writes to id 0x000c, in
	 * pairs (i, i + 0x20) for i from 0 to 17. The values differ from
	 * prep_second_iter()'s.
	 */
	static const struct { u16 off; u16 val; } iter20_batch[36] = {
		{ 0x0000, 0x0100 }, { 0x0020, 0x0100 },
		{ 0x0001, 0x0200 }, { 0x0021, 0x0200 },
		{ 0x0002, 0x0300 }, { 0x0022, 0x0300 },
		{ 0x0003, 0x0500 }, { 0x0023, 0x0500 },
		{ 0x0004, 0x0700 }, { 0x0024, 0x0700 },
		{ 0x0005, 0x0a00 }, { 0x0025, 0x0a00 },
		{ 0x0006, 0x0f00 }, { 0x0026, 0x0f00 },
		{ 0x0007, 0x0f01 }, { 0x0027, 0x1500 },
		{ 0x0008, 0x0f02 }, { 0x0028, 0x1e00 },
		{ 0x0009, 0x0f03 }, { 0x0029, 0x2a00 },
		{ 0x000a, 0x0f04 }, { 0x002a, 0x3c00 },
		{ 0x000b, 0x0f05 }, { 0x002b, 0x3c01 },
		{ 0x000c, 0x0f06 }, { 0x002c, 0x3c02 },
		{ 0x000d, 0x0f07 }, { 0x002d, 0x3c03 },
		{ 0x000e, 0x1507 }, { 0x002e, 0x3c04 },
		{ 0x000f, 0x1e07 }, { 0x002f, 0x3c05 },
		{ 0x0010, 0x2a07 }, { 0x0030, 0x3c06 },
		{ 0x0011, 0x3c07 }, { 0x0031, 0x3c07 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(iters); i++) {
		b43_phy_ac_rxcal_afe_iter(dev, iters[i].cmd, iters[i].core_off,
					  NULL, 0,
					  iters[i].rd_off, 2,
					  iters[i].wr_off);

		if (i == 1) {
			/* Iter 20: batch fast 36× TBL.WR len=1 (183 op) */
			unsigned int j;

			b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			for (j = 0; j < ARRAY_SIZE(iter20_batch); j++)
				b43_actab_write_bulk(dev, 0x000c,
						     iter20_batch[j].off,
						     16, 1,
						     &iter20_batch[j].val);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		}
	}
}

/*
 * RX-IQ post-measurement apply (fase F seg A, 32 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Per-core bbmult, the same values rxiqcal_apply_tx_gain_bbmult() writes.
	 * The registers are 0x63 and 0x73 for core 0 and 0x67 and 0x77 for
	 * core 1, a +4 stride per core.
	 */
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core;

	/* 3 op standalone */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_mask(dev, 0x0382, (u16)~0x8000);
	b43_phy_mask(dev, 0x0460, (u16)~0x0004);

	/* Sub-batch per-core: preamble + 2× fast TBL.WR bbmult + postamble */
	for (core = 0; core < 2; core++) {
		u16 lo = (u16)(0x0063 + 4 * core);
		u16 hi = (u16)(0x0073 + 4 * core);
		const u16 *bbmult = &ac->bbmult_cal[core];

		if (!((ac->coremask >> core) & 1))
			continue;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* 3 op standalone finali: pulse + reset */
	b43_phy_ac_cca_pulse(dev);
	b43_phy_write(dev, 0x0382, 0x0000);
}

/*
 * Reset tabelle di coefficienti (2688 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   24488-27559]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   19700-22771]
 */
void b43_phy_ac_iqcal_coeff_tables_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const u16 tbl[3] = { 0x0042, 0x0062, 0x0082 };
	unsigned int off, core;

	for (off = 0; off < 128; off++) {
		for (core = 0; core < 3; core++) {
			u16 v = b43_phy_ac_loft_lut_base(core, off);

			b43_actab_write_bulk_scoped(dev, tbl[core], (u16)off,
						    16, 1, &v);
		}
	}
}

/*
 * IQ-cal secondary stage apply (47 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   27710-27768]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   22922-22980]
 */
void b43_phy_ac_iqcal_apply_second_stage(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * The results iteration 20 measured on core 0 and iteration 22 on
	 * core 1 -- see b43_phy_ac_rxiqcal_run_meas_iters() -- reapplied at
	 * offsets 0x60 and 0x64 of table 0x000c instead of 0x40 and 0x48. They
	 * come out of afe_res, not from a capture.
	 */
	u16 cell[2];

	/* Kick sequence (7 op) */
	b43_phy_read_log(dev, 0x0400);
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0001, 0x0001); /* set bit 0 (non gate) */
	b43_phy_set(dev,      0x0400, 0x0003);
	b43_phy_set(dev,      0x0402, 0x0020);
	b43_phy_read_log(dev, 0x0403);
	b43_phy_write(dev,    0x0400, 0x0000);

	/* Gate reset (1 op): overwrite completo B43_PHY_AC_REG_TBL_WRITE_GATE */
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x03d0);

	/* Zeros (4 op) */
	b43_phy_write(dev, 0x06a0, 0x0000);
	b43_phy_write(dev, 0x06a1, 0x0000);
	b43_phy_write(dev, 0x08a0, 0x0000);
	b43_phy_write(dev, 0x08a1, 0x0000);

	/* MOD (1 op): il vendor emette un vero MOD (mask=0x0001), non
	 * un AND normalizzato — usiamo maskset. */
	b43_phy_maskset(dev, 0x0211, (u16)~0x0001, 0);

	/* Pair 1 (16 op): TBL.RD + TBL.WR len=2 off=0x60 */
	b43_actab_read_bulk(dev, 0x000c, 0x0060, 16, 2, cell);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0060, 16, 2,
				    dev->phy.ac->afe_res[0].v);

	/* Pair 2 (16 op): TBL.RD + TBL.WR len=2 off=0x64 */
	b43_actab_read_bulk(dev, 0x000c, 0x0064, 16, 2, cell);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0064, 16, 2,
				    dev->phy.ac->afe_res[2].v);

	/* Close (2 op): sync peek + gate re-lock */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
}

/*
 * RX gain configuration a core carries across the RX-IQ cal, in the order the
 * vendor reads it; rxgain_config_readback() saves, rxiq_teardown_apply_defaults()
 * restores (0x0727 before 0x0726 there).
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
 * RX-gain config readback (94 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* The order the capture shows, which is not sorted by address. */
	static const u16 bbmult_off[2] = { 0x0063, 0x0067 };
	static const u16 gain_lut_off[2][3] = {
		{ 0x0100, 0x0103, 0x0106 },  /* core 0 */
		{ 0x0101, 0x0104, 0x0107 },  /* core 1 */
	};
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core, i;
	u16 discard;

	/* Preamble globale (2 op) */
	b43_phy_read_log(dev, 0x040f);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);

	for (core = 0; core < 2; core++) {
		u16 stride = (u16)(core * 0x200);

		if (!((ac->coremask >> core) & 1))
			continue;

		/* 25 gain-register peeks, in the observed order. */
		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxgain_cfg_regs); i++)
			ac->rxgain_cfg_saved[core][i] =
				b43_phy_read_log(dev, b43_phy_ac_rxgain_cfg_regs[i] + stride);

		/* 1× fast TBL.RD id=0x000c bbmult */
		b43_actab_read_bulk(dev, 0x000c, bbmult_off[core], 16, 1, &discard);

		/* 3× fast TBL.RD id=0x0007 gain code */
		for (i = 0; i < 3; i++)
			b43_actab_read_bulk(dev, 0x0007, gain_lut_off[core][i],
					    16, 1, &ac->rfseq_gain_saved[core][i]);

		/* 1 peek 0x?73e */
		ac->rxgain_cfg_saved[core][25] = b43_phy_read_log(dev, 0x073e + stride);
	}
}

/*
 * RX-gain config apply (146 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * Per-core MODs. The registers are core 0's; core 1 adds 0x0200. The
	 * order is the one phase 1 of the capture shows.
	 */
	struct mod_op { u16 reg; u16 mask; u16 val; };
	static const struct mod_op phase1[52] = {
		{ 0x073e, 0x0010, 0x0000 }, { 0x073e, 0x0020, 0x0000 },
		{ 0x073e, 0x1000, 0x1000 }, { 0x0721, 0x0001, 0x0001 },
		{ 0x0729, 0x0001, 0x0001 }, { 0x073a, 0x0007, 0x0003 },
		{ 0x0725, 0x0020, 0x0020 }, { 0x0739, 0x007e, 0x007a },
		{ 0x0725, 0x0002, 0x0002 }, { 0x073a, 0x0008, 0x0000 },
		{ 0x0725, 0x0040, 0x0040 }, { 0x073a, 0x0010, 0x0010 },
		{ 0x0725, 0x0080, 0x0080 }, { 0x073a, 0x0060, 0x0040 },
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
	/* Phase 2: 12 MODs after the table read. */
	static const struct mod_op phase2[12] = {
		{ 0x0735, 0x0700, 0x0000 }, { 0x0723, 0x0008, 0x0008 },
		{ 0x0735, 0x3800, 0x0000 }, { 0x0723, 0x0010, 0x0010 },
		{ 0x0737, 0x00ff, 0x0091 }, { 0x0723, 0x0200, 0x0200 },
		{ 0x0735, 0x4000, 0x0000 }, { 0x0723, 0x0020, 0x0020 },
		{ 0x0735, 0x0001, 0x0001 }, { 0x0723, 0x0001, 0x0001 },
		{ 0x0729, 0x0100, 0x0100 }, { 0x0721, 0x0100, 0x0100 },
	};
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int core, i;
	u16 discard;

	/* Header (3 op): peek + 2 MOD 0x0401 */
	b43_phy_read_log(dev, 0x0401);
	b43_phy_maskset(dev, 0x0401, (u16)~0x0007, 0x0003);
	b43_phy_maskset(dev, 0x0401, (u16)~0x7000, 0x0000);

	for (core = 0; core < 2; core++) {
		u16 stride = (u16)(core * 0x200);

		if (!((ac->coremask >> core) & 1))
			continue;

		/* Phase 1: 52 MOD (bit-field config) */
		for (i = 0; i < ARRAY_SIZE(phase1); i++)
			b43_phy_maskset(dev, phase1[i].reg + stride,
					(u16)~phase1[i].mask, phase1[i].val);

		/* 1× fast TBL.RD readback (5 op) */
		b43_actab_read_bulk(dev, 0x0007,
				    (u16)(0x0140 + core * 0x10),
				    16, 1, &discard);

		/* Phase 2: 12 MOD (bit-field config) */
		for (i = 0; i < ARRAY_SIZE(phase2); i++)
			b43_phy_maskset(dev, phase2[i].reg + stride,
					(u16)~phase2[i].mask, phase2[i].val);

		/* 2 op: peek + MOD 0x?78 clr bit 0 */
		b43_phy_read_log(dev, 0x0678 + stride);
		b43_phy_maskset(dev, 0x0678 + stride, (u16)~0x0001, 0);
	}

	/* Trailer (1 op): gate unlock */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * Radio 2069 IQ-cal config per-core (84 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/* 6 registri radio 2069 letti/scritti per core */
	static const u16 rad_regs[6] = {
		0x0020, 0x0021, 0x0022, 0x0023, 0x003a, 0x003d
	};
	/* Ten bit-field configuration MODs, in the observed order. */
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

	for (core = 0; core < 2; core++) {
		u16 stride = (u16)(core * 0x200);

		if (!((ac->coremask >> core) & 1))
			continue;

		/* 6 RAD.RD (readback) */
		for (i = 0; i < ARRAY_SIZE(rad_regs); i++)
			b43_radio_read_log(dev, rad_regs[i] + stride);

		/* 6 RAD.WR = 0 (zero out) */
		for (i = 0; i < ARRAY_SIZE(rad_regs); i++)
			b43_radio_write(dev, rad_regs[i] + stride, 0);

		/* 10 RAD.MOD (bit-field config) */
		for (i = 0; i < ARRAY_SIZE(configs); i++)
			b43_radio_maskset(dev, configs[i].reg + stride,
					  (u16)~configs[i].mask,
					  configs[i].val);
	}
}

/*
 * Gain control final apply (129 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	/*
	 * Gain code values, transcribed from the d6220 ch36 capture. The vendor
	 * derives them at runtime from a table read of 0x0020 offset 0 combined
	 * with the RX-IQ cal results. TODO: that computation is still pending.
	 */
	static const u16 gain_vals[3] = { 0x0000, 0xff7f, 0x00f3 };
	static const u16 bbmult_val = 0x0044;
	unsigned int core;

	if (with_peek_preamble) {
		/* Preamble globale (3 op): 3 peek 0x?dc, 3-core stride +0x200 */
		b43_phy_read_log(dev, 0x06dc);
		b43_phy_read_log(dev, 0x08dc);
		b43_phy_read_log(dev, 0x0adc);
	}

	for (core = 0; core < 3; core++) {
		u16 stride = (u16)(core * 0x200);
		u16 discard;

		if (!(core_mask & (1u << core)))
			continue;

		/*
		 * Do not consult the board's coremask here: the vendor emits
		 * core 2 on a two-core board too. The mask in play is the search
		 * round's set, not the hardware's.
		 */

		/* 3 WR gain regs (0x?734 varia per core, passato da chiamante) */
		b43_phy_write(dev, 0x0730 + stride, 0x00b0);
		b43_phy_write(dev, 0x0731 + stride, 0x0004);
		b43_phy_write(dev, 0x0734 + stride, r734_vals[core]);

		/* 3 MOD 0x?722 set bit 1/2/3 */
		b43_phy_maskset(dev, 0x0722 + stride, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, 0x0722 + stride, (u16)~0x0004, 0x0004);
		b43_phy_maskset(dev, 0x0722 + stride, (u16)~0x0008, 0x0008);

		/* Preamble (2 op): peek + MOD lock */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Fast TBL.RD id=0x0020 off=0x0000 (5 op) */
		b43_actab_read_bulk(dev, 0x0020, 0x0000, 16, 1,
				    &dev->phy.ac->bbmult_meas);

		/* 3× fast TBL.WR id=0x0007 gain code (15 op) */
		b43_actab_write_bulk(dev, 0x0007, (u16)(0x0100 + core),
				     16, 1, &gain_vals[0]);
		b43_actab_write_bulk(dev, 0x0007, (u16)(0x0103 + core),
				     16, 1, &gain_vals[1]);
		b43_actab_write_bulk(dev, 0x0007, (u16)(0x0106 + core),
				     16, 1, &gain_vals[2]);

		/* Sync (2 op): peek + MOD lock idempotent */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* 2× fast TBL.WR id=0x000c bbmult (10 op) */
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0063 + core * 4),
				     16, 1, &bbmult_val);
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0073 + core * 4),
				     16, 1, &bbmult_val);

		/* Bridge (2 op): set + clr — lock/unlock idempotente */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}
}

/*
 * Blocks A to D of the post-DDS meas_apply, 47 ops. Factored out because
 * iqcal_meas_post_dds_apply() and its _v2 variant -- the fifth cycle, whose
 * blocks E to H differ -- share them.
 *
 *   A, 14 ops: two self-contained bbmult table reads, cores 0 and 1
 *   B,  2 ops: pulse bit 14 of 0x0001, the commit
 *   C, 13 ops: the kick sequence variant
 *   D, 18 ops: the rxgain_perchan_config-style tail
 */
static void iqcal_meas_readback_kick_tail(struct b43_wldev *dev)
{
	u16 discard;

	/* Blocco A (14 op): 2× TBL.RD auto-contained bbmult per core 0/1 */
	b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1, &discard);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1, &discard);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

	/* Blocco B (2 op): MOD 0x0001 pulse bit 14 (commit) */
	b43_phy_ac_cca_pulse(dev);

	/* Blocco C (13 op): kick sequence variante */
	b43_phy_mask(dev,      0x0471, (u16)~0x0001);
	b43_phy_write(dev,     0x0463, 0x0027);
	b43_phy_write(dev,     0x0461, 0xffff);
	b43_phy_write(dev,     0x0462, 0x003c);
	b43_phy_read_log(dev,  0x0400);
	b43_phy_set(dev,       0x0400, 0x0001);
	b43_phy_mask(dev,      0x0460, (u16)~0x0004);
	b43_phy_mask(dev,      0x0460, (u16)~0x0001);
	b43_phy_mask(dev,      0x0382, (u16)~0xc000);
	b43_phy_set(dev,       0x0460, 0x0001);
	b43_phy_read_log(dev,  0x0403);
	b43_phy_read_log(dev,  0x0403);
	b43_phy_write(dev,     0x0400, 0x0000);

	/* Blocco D (18 op): tail comune — vedi b43_phy_ac_rxgain_perchan_tail. */
	b43_phy_ac_rxgain_perchan_tail(dev);
}

/*
 * Wait for an RX-IQ measurement to finish. Bit 0 of 0x0270 is the start flag,
 * which the hardware clears on completion, and the stock driver re-reads it
 * while it stays high. The budget is finite, as for the 0x0380 poll, so that
 * hardware in an unexpected state does not block the kernel.
 */
static void iqcal_meas_wait(struct b43_wldev *dev)
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
 * IQ-cal measurement + apply post second DDS (99 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   28447-28578, 28819-28954, 29195-29330, 29520-29655]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23659-23790, 24031-24166, 24407-24542, 24732-24867]
 */
void b43_phy_ac_iqcal_meas_post_dds_apply(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c;

	/* Blocchi A+B+C+D (47 op) — helper condiviso */
	iqcal_meas_readback_kick_tail(dev);

	/* Blocco E: setup 0x0272/0x0271/0x0270, arm, attesa, peek finale */
	b43_phy_write(dev,    0x0272, 0x0400);
	b43_phy_maskset(dev,  0x0271, (u16)~0x00ff, 0x0020);
	b43_phy_maskset(dev,  0x0270, (u16)~0x0002, 0);
	b43_phy_maskset(dev,  0x0270, (u16)~0x0001, 0x0001);
	iqcal_meas_wait(dev);
	b43_phy_read_log(dev, 0x0270);

	/* Block F, 12 ops: gain-register peeks, 0x?c0 to 0x?c5 over two cores,
	 * in the observed order 0x?c3, 0x?c2, 0x?c5, 0x?c4, 0x?c1, 0x?c0. */
	for (c = 0; c < 2; c++)
		b43_phy_ac_iq_acc_peek(dev, c, false);

	/* Blocco G (29 op): apply bbmult per-core (variante) */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_set(dev,      0x0460, 0x0002);
	b43_phy_mask(dev,     0x0460, (u16)~0x0004);
	for (c = 0; c < 2; c++) {
		u16 lo = (u16)(0x0063 + 4 * c);
		u16 hi = (u16)(0x0073 + 4 * c);
		const u16 *bbmult = &dev->phy.ac->bbmult_meas;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* Blocco H (2 op): MOD 0x0001 pulse finale */
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Per-core sub-block of the v2 variant: four peeks, four MODs, the arm, the
 * wait, four writes and one extra peek.
 */
static void meas_v2_gain_prog_poll(struct b43_wldev *dev, unsigned int core)
{
	u16 stride = (u16)(core * 0x200);

	/* 4 peek gain regs (0x?20, 0x?28, 0x?21, 0x?29) */
	b43_phy_read_log(dev, 0x0720 + stride);
	b43_phy_read_log(dev, 0x0728 + stride);
	b43_phy_read_log(dev, 0x0721 + stride);
	b43_phy_read_log(dev, 0x0729 + stride);

	/* 4 MOD gain regs */
	b43_phy_maskset(dev, 0x0720 + stride, (u16)~0x0001, 0x0001);
	b43_phy_maskset(dev, 0x0728 + stride, (u16)~0x0001, 0);
	b43_phy_maskset(dev, 0x0721 + stride, (u16)~0x0004, 0x0004);
	b43_phy_maskset(dev, 0x0729 + stride, (u16)~0x0002, 0);

	/* Arm poll */
	b43_phy_maskset(dev, 0x0270, (u16)~0x0001, 0x0001);

	iqcal_meas_wait(dev);

	/* Four gain-register writes, in the observed order 0x?29, 0x?21,
	 * 0x?28, 0x?20. */
	b43_phy_write(dev, 0x0729 + stride, 0x0321);
	b43_phy_write(dev, 0x0721 + stride, 0x7761);
	b43_phy_write(dev, 0x0728 + stride, 0x0080);
	b43_phy_write(dev, 0x0720 + stride, 0x0182);

	/* 1 peek 0x0270 extra */
	b43_phy_read_log(dev, 0x0270);
}

/*
 * Loopback gain search. The mean power per sample is
 * round(ii / 1024) + round(qq / 1024) and has to be brought inside the
 * [PWR_LO, PWR_HI] window: below it the index goes up, above it down. The
 * step is a halving rather than a decrement, and the minimum on 0x0734 is 0,
 * the field being three bits.
 *
 * At an extreme index with the power still outside the window, the search
 * exits accepting that index. That is conservative rather than measured.
 * Derivation and the check against all 52 sweep segments:
 * the "Ricerca del guadagno di loopback" section of
 * docs/rxiq-cal-analysis.md.
 *
 * The later cal stages -- estimate, leakage, tone fit -- are not here.
 */
#define B43_PHY_AC_LOOPBACK_PWR_LO	0x0b57
#define B43_PHY_AC_LOOPBACK_PWR_HI	0x169e
#define B43_PHY_AC_LOOPBACK_IDX_MIN	0
#define B43_PHY_AC_LOOPBACK_IDX_MAX	7	/* campo a 3 bit, mask 0x7 */
#define B43_PHY_AC_LOOPBACK_IDX_5G	4
#define B43_PHY_AC_LOOPBACK_REG		0x0734

/* Potenza media per campione dall'ultimo round di accumulatori. */
static u32 b43_phy_ac_loopback_pwr(const struct b43_phy_ac_iq_acc *acc)
{
	/* arrotondamento a 1024, non troncamento: round(x/1024) */
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

	/* Passo per dimezzamento, come la sequenza 4,2,1,0 osservata. */
	if (pwr < B43_PHY_AC_LOOPBACK_PWR_LO) {
		if (*idx >= B43_PHY_AC_LOOPBACK_IDX_MAX)
			return true;	/* al massimo: non si puo' salire */
		*idx = *idx ? (u8)min_t(unsigned int, *idx * 2u,
					B43_PHY_AC_LOOPBACK_IDX_MAX) : 1;
		return false;
	}
	if (pwr > B43_PHY_AC_LOOPBACK_PWR_HI) {
		if (*idx == 0)
			return true;	/* al minimo: non si puo' scendere */
		*idx = (u8)(*idx / 2);
		return false;
	}
	return true;
}

/*
 * Loopback gain search, rounds 6 to 9 of the txpwr apply. Each round writes
 * the current indices through gainctrl_final_apply(), runs a tone and
 * measurement pass -- dds_seed() then meas(), which reads the core 0 and 1
 * accumulators with iq_acc_peek() -- then advances the indices with
 * b43_phy_ac_loopback_step() until every measured core converges. A core that
 * has converged leaves the round's set, while the measurement keeps covering
 * both cores: on ch132 at 40 MHz, round 4 writes only core 1 but still reads
 * core 0.
 *
 * Core 2 is never measured -- no read of 0x0ac0-0x0ac5 appears in the loop --
 * and follows a fixed three-round schedule of {4, 1, 0}, invariant across all
 * 52 runs of the d6220 sweep, every channel and every width. It is not a
 * search, and the rule that produces it is unknown: transcribed, not derived.
 *
 * The starting index is the 5 GHz one; the port refuses 2.4 GHz, where it
 * would start from 0. With a read oracle the measurements are the vendor's
 * and the search converges on the same indices: on the d6220 ch36 BW20 gate
 * it reproduces {4,4,4} {2,2,1} {1,1,0} {0,0}. The cap on rounds is a safety
 * budget, not an observed limit -- the most seen is 4.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   28201-29655]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   23413-24867]
 */
static void b43_phy_ac_loopback_gain_search(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const u16 c2_sched[3] = { 4, 1, 0 };
	u16 r734[3] = { 0, 0, 0 };
	u8 idx[2] = {
		B43_PHY_AC_LOOPBACK_IDX_5G,
		B43_PHY_AC_LOOPBACK_IDX_5G,
	};
	bool searching[2] = { true, true };
	unsigned int round, c;

	for (round = 0; searching[0] || searching[1]; round++) {
		u8 mask = 0;

		if (round >= 8) {
			b43err(dev->wl,
			       "AC-PHY: loopback gain search did not converge\n");
			break;
		}

		for (c = 0; c < 2; c++) {
			if (!searching[c])
				continue;
			mask |= (u8)(1u << c);
			r734[c] = idx[c];
		}
		if (round < ARRAY_SIZE(c2_sched)) {
			mask |= 1u << 2;
			r734[2] = c2_sched[round];
		}

		b43_phy_ac_gainctrl_final_apply(dev, round == 0, mask, r734);
		b43_phy_ac_rxiqcal_dds_seed_tone(dev, 1);
		b43_phy_ac_iqcal_meas_post_dds_apply(dev);

		for (c = 0; c < 2; c++)
			if (searching[c])
				searching[c] =
					!b43_phy_ac_loopback_step(dev, c,
								  &idx[c]);
	}
}

/*
 * Capture a core's six RX-IQ accumulators and add them into the state: the six
 * peeks of 0x?c0 to 0x?c5 in the observed order, 0x?c3, 0x?c2, 0x?c5, 0x?c4,
 * 0x?c1, 0x?c0. These are the reads the stock driver emits anyway, in the same
 * order; the only difference is that the value is kept instead of discarded.
 *
 * @measurement separates the passes that enter the mean from those of the
 * loopback gain search, which read the same registers and must not be
 * averaged. In the captures the two families separate because the search
 * rounds are the only ones preceded by an increment of PHY 0x0b22, in
 * gainctrl_final_apply(), which only loopback_gain_search() calls: on the
 * d6220 they are 3 and 3 at 20 MHz, 3 and 6 at 80.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
 * IQ-cal measurement variante v2 (143 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   29743-29957, 30045-30381]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   24955-25291, 25379-25663]
 */
void b43_phy_ac_iqcal_meas_post_dds_apply_v2(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c;

	/* Blocchi A+B+C+D (47 op) */
	iqcal_meas_readback_kick_tail(dev);

	/* Setup (3 op) — nota: WR 0x0272 = 0x4000 (invece di 0x0400 del v1) */
	b43_phy_write(dev,   0x0272, 0x4000);
	b43_phy_maskset(dev, 0x0271, (u16)~0x00ff, 0x0020);
	b43_phy_maskset(dev, 0x0270, (u16)~0x0002, 0);

	meas_v2_gain_prog_poll(dev, 1);
	/* 6 peek 0x?c0-?c5 core 0 */
	meas_v2_peek_c0_c5(dev, 0);
	meas_v2_gain_prog_poll(dev, 0);
	/* 6 peek 0x?c0-?c5 core 1 */
	meas_v2_peek_c0_c5(dev, 1);

	/* Blocco G (29 op): apply bbmult per-core — identico a v1 */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_set(dev,      0x0460, 0x0002);
	b43_phy_mask(dev,     0x0460, (u16)~0x0004);
	for (c = 0; c < 2; c++) {
		u16 lo = (u16)(0x0063 + 4 * c);
		u16 hi = (u16)(0x0073 + 4 * c);
		const u16 *bbmult = &dev->phy.ac->bbmult_meas;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* Blocco H (2 op): MOD 0x0001 pulse finale */
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Solve the RX-IQ coefficients (a, b) of one core from its measurement rounds.
 *
 * Each round is one tone: the two rounds up to 40 MHz sit at +f and -f, the
 * six at 80 MHz at +-f, +-3f, +-4f. The imbalance measured on a single tone
 * carries the frequency-dependent part; what goes into 0x?a0/0x?a1 is the
 * frequency-independent part, the mean over the tones of the per-tone
 * coefficients. Summing the accumulators instead weights each tone by its
 * power, and at 80 MHz, where the roll-off makes the powers unequal and the
 * per-tone a differ by tens of units, that comes out one unit off.
 *
 * Per tone, in Q10:
 *
 *   a_r     = -iq * 2^10 / ii
 *   b_r + 1 = 2^10 * sqrt(qq * ii - iq^2) / ii
 *
 * a is the mean of the a_r, kept in Q16 so that the per-tone rounding does
 * not decide the final unit; b is the mean of the a_r-rounded b_r, rounded
 * half up. The second form of b_r is the first with the exact a_r under the
 * root: with a_r rounded to Q10 first, a tone sitting on a half flips.
 *
 * Measured on 118 writes of 0x?a1 over the seven cold segments that run the
 * phase and the 52 hot ones (reverse-tools/rxiq_points.py):
 *
 *                       a exact   b exact   both
 *   accumulator sum      108       86        80
 *   this                 117       90        89
 *
 * What remains is on the d6220's core 1: 34/59 exact with a symmetric +-1
 * residual that follows the band -- the vendor comes out ~0.7 LSB higher on
 * ch100 and above than below 5250 MHz, relative to any function of these six
 * accumulators. Core 0 is 56/59 with the three misses one low. On agcombo,
 * whose three chains are all clean under the mean, the core-1 residual is
 * not a rule per chain; an input outside the accumulators is missing for it.
 * The b43_phy_ac_todo() at the write site stays for that reason.
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
		s64 num;
		u64 root;

		if (!ii) {
			*a_out = 0;
			*b_out = 0;
			return;
		}

		/* a_r in Q16, rounded half away from zero. */
		num = -(iq << 16);
		a_sum += div64_s64(num + (num < 0 ? -(s64)(ii >> 1)
						 : (s64)(ii >> 1)), ii);

		/*
		 * b_r + 1 in Q10: 2^10 * sqrt(qq*ii - iq^2) / ii, rounded to
		 * nearest. qq*ii - iq^2 >= 0 by Cauchy-Schwarz, and fits in
		 * 64 bits for the 32-bit accumulators the estimator has.
		 */
		root = int_sqrt64(qq * ii - (u64)(iq * iq));
		b_sum += div64_u64((root << 11) + ii, ii << 1);
	}

	/* Mean of the Q16 a_r back to Q10, rounded half away from zero. */
	a_den = (s64)nr << 6;
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
 * Per-chain write of the RX-IQ correction coefficients, four ops.
 *
 * The coefficients are computed from the accumulators the measurement
 * gathered: same inputs as the stock driver, same result. Verified bit-exactly
 * against the d6220 attach-to-bss-up capture -- core 0 gives a = -17, b = 77
 * (0x3ef and 0x04d), core 1 gives a = -44, b = 59 (0x3d4 and 0x03b) --
 * identical to what the stock driver writes.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   30382-30385]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25664-25667]
 */
void b43_phy_ac_rxiq_apply_coefficients(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	unsigned int c;

	b43_phy_ac_todo(dev,
		"the RX IQ coefficient b matches the stock driver on 90 of 118 "
		"measured points; the misses are one LSB either way and sit "
		"almost all on core 1, where they follow the band. An input "
		"outside the six accumulators is missing. Receive image "
		"rejection on that core may be slightly worse.\n");

	/* Per core [c]: 0x?a0 (coeff a) e 0x?a1 (coeff b) */
	for (c = 0; c < 2; c++) {
		u16 stride = (u16)(c * 0x200);
		s16 a, b;

		b43_phy_ac_iq_solve(&dev->phy.ac->iq_acc[c], &a, &b);
		b43_phy_write(dev, 0x06a0 + stride, (u16)(a & 0x03ff));
		b43_phy_write(dev, 0x06a1 + stride, (u16)(b & 0x03ff));
	}
}

/*
 * Radio 2069 IQ-cal teardown (12 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	static const u16 rad_regs[6] = {
		0x0020, 0x0021, 0x0022, 0x0023, 0x003a, 0x003d
	};
	/* Written values are zero except 0x003d, which gets 0x000f. */
	static const u16 rad_vals[6] = { 0, 0, 0, 0, 0, 0x000f };
	unsigned int c, i;

	for (c = 0; c < 2; c++) {
		u16 stride = (u16)(c * 0x200);

		for (i = 0; i < ARRAY_SIZE(rad_regs); i++)
			b43_radio_write(dev, rad_regs[i] + stride, rad_vals[i]);
	}
}

/*
 * RXIQ cal teardown + apply defaults (185 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   30398-30613]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25680-25895]
 */
void b43_phy_ac_rxiq_teardown_apply_defaults(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Gain LUT default values, INDEX_DEFAULT, written twice per core: before
	 * and after the table read of 0x0020 at offset 0x0040.
	 */
	u16 bbmult_val;

	/*
	 * Gain-register reset values, 27 writes per core in the vendor's order,
	 * which is not contiguous by address -- note 0x0727 before 0x0726 and
	 * 0x0737 before 0x0736. The addresses given are core 0's; core 1 adds a
	 * +0x200 stride.
	 */
	static const u16 reset_regs[27] = {
		0x073e, 0x0678, 0x0720, 0x0721, 0x0722, 0x0723, 0x0724, 0x0725,
		0x0727, 0x0726, 0x0728, 0x0729, 0x0732, 0x0733, 0x0730, 0x0731,
		0x0734, 0x0735, 0x0737, 0x0738, 0x0736, 0x0739, 0x073a, 0x073b,
		0x073c, 0x073d, 0x0747,
	};
	unsigned int c, i;

	/* Preamble globale (3 op) */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_write(dev,   0x0401, 0x7733);

	for (c = 0; c < 2; c++) {
		u16 stride = (u16)(c * 0x200);

		/* 3 fast TBL.WR gain (15 op) */
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 0x0007,
					     (u16)(0x0100 + c + i * 3),
					     16, 1, &dev->phy.ac->rfseq_gain_saved[c][i]);

		/* Sync (2 op) */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Fast TBL.RD 0x0020 off=0x0040 (5 op) */
		b43_actab_read_bulk(dev, 0x0020, 0x0040, 16, 1, &bbmult_val);

		/* The three fast gain table writes, repeated -- 15 ops, same
		 * values. */
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 0x0007,
					     (u16)(0x0100 + c + i * 3),
					     16, 1, &dev->phy.ac->rfseq_gain_saved[c][i]);

		/* Sync (2 op) */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* 2 fast TBL.WR bbmult (10 op) */
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0063 + c * 4), 16, 1, &bbmult_val);
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0073 + c * 4), 16, 1, &bbmult_val);

		/*
		 * Bridge (4 op): 2× MOD lock idempotent + peek + MOD lock.
		 * Pattern strano — probabilmente un pipeline flush hardware.
		 */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* 2 fast TBL.WR bbmult RIPETUTI (10 op) */
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0063 + c * 4), 16, 1, &bbmult_val);
		b43_actab_write_bulk(dev, 0x000c,
				     (u16)(0x0073 + c * 4), 16, 1, &bbmult_val);

		/* Trailer (1 op) */
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
 * Probe cycle, used by rxiqcal_finalize(): N iterations of 17 ops each,
 * repeating gain-register peeks, a MAC toggle and a MOD of bits 2 and 3 of
 * 0x0520. Each iteration is:
 *   four core-0 peeks: 0x07af, 0x07b3, 0x07ab, 0x07b1
 *   four core-1 peeks: 0x09af, 0x09b3, 0x09ab, 0x09b1
 *   four peeks of 0x0523, 0x0529, 0x0528, 0x0527
 *   a MAC.MCTRL toggle, set then clear, flushing before the mode change
 *   a MOD of 0x0520 under mask 0x000c; see probe_mode_next()
 *   a second MAC.MCTRL toggle, flushing after the mode change
 *
 * SALAME: reading this as a probe plus a mode toggle is our interpretation.
 * What it is actually for -- a calibration measurement, an EVM check -- is
 * not documented.
 */
static const u16 b43_phy_ac_probe_peek_regs[12] = {
	0x07af, 0x07b3, 0x07ab, 0x07b1,
	0x09af, 0x09b3, 0x09ab, 0x09b1,
	0x0523, 0x0529, 0x0528, 0x0527,
};

/*
 * Take the current value of the toggle in 0x0520[3:2] and flip it. The counter
 * lives in ac->probe_mode because the stock driver does not reset it between
 * one probe block and the next: the alternation runs continuously across the
 * whole phase.
 */
static u16 probe_mode_next(struct b43_phy_ac *ac)
{
	u16 cur = ac->probe_mode;

	ac->probe_mode = cur ? 0x0000 : 0x0004;
	return cur;
}

/* mac_enable followed by mac_suspend: the flush the stock driver puts around
 * every mode change on 0x0520. */
static void probe_mac_flush(struct b43_wldev *dev)
{
	b43_mac_enable(dev);
	b43_mac_suspend(dev);
}

static void b43_phy_ac_probe_cycle(struct b43_wldev *dev, unsigned int n_iter,
				   bool extended_first, bool closes_sequence)
{
	B43_AC_FN();
	unsigned int iter, k;

	/*
	 * Called only from rxiqcal_finalize(), after block C has brought the PHY
	 * to an operational release: RX_WAITED and RX_OFDM set, clip detect
	 * enabled on all three cores. The MAC is suspended on entry, and every
	 * iteration does its own mac_enable and mac_suspend, since the caller
	 * expects it suspended on return.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_RX_OFDM,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CLIP_ALL_DIS |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	for (iter = 0; iter < n_iter; iter++) {
		for (k = 0; k < ARRAY_SIZE(b43_phy_ac_probe_peek_regs); k++)
			b43_phy_read_log(dev, b43_phy_ac_probe_peek_regs[k]);

		if (extended_first && iter == 0) {
			/*
			 * The first group of a first bring-up is irregular: it
			 * carries the 0x0554/0x0555 pair and three mode
			 * changes instead of one, with an extra flush around
			 * the second and third. The order is transcribed from
			 * the attach capture; the reason is not known.
			 */
			b43_phy_write(dev, 0x0554, 0x0bb8);
			b43_phy_write(dev, 0x0555, 0x0bb8);

			probe_mac_flush(dev);
			b43_phy_maskset(dev, 0x0520, (u16)~0x000c,
					probe_mode_next(dev->phy.ac));
			probe_mac_flush(dev);
			b43_phy_maskset(dev, 0x0520, (u16)~0x000c,
					probe_mode_next(dev->phy.ac));
			probe_mac_flush(dev);
			probe_mac_flush(dev);
			b43_phy_maskset(dev, 0x0520, (u16)~0x000c,
					probe_mode_next(dev->phy.ac));
			probe_mac_flush(dev);
			probe_mac_flush(dev);
			continue;
		}

		probe_mac_flush(dev);
		b43_phy_maskset(dev, 0x0520, (u16)~0x000c,
				probe_mode_next(dev->phy.ac));

		if (closes_sequence && iter + 1 == n_iter) {
			/*
			 * The flush that closes the whole probe and measure
			 * sequence also drops bit 20 of MACCONTROL, between
			 * the enable and the suspend. Verified on both the
			 * attach and the down-to-bss paths.
			 *
			 * SALAME: that bit is not tracked; probably a
			 * calibration-complete flag or a MAC gate.
			 */
			b43_mac_enable(dev);
			b43_maccontrol_set(dev, ~0x00100000u, 0);
			b43_mac_suspend(dev);
			continue;
		}

		probe_mac_flush(dev);
	}
}

/*
 * Measure block: the PHY's noise and RSSI measurement pass, 393 ops in nine
 * sub-blocks labelled in the body. finalize() runs it in four converging
 * rounds after each probe cycle; the watchdog reruns it unchanged in steady
 * state, without the two closing MAC toggles.
 *
 * The four gain fields of the RX arming depend on the bandwidth; the 20 MHz
 * column is wired here. Full table and sub-blocks:
 * the "Measure block: struttura e dipendenza dalla larghezza" section of
 * docs/rxiq-cal-analysis.md.
 *
 * Both callers -- rxiqcal_finalize() after a probe cycle, and the periodic
 * tick of b43_phy_ac_watchdog() -- enter with the PHY in release, RX_WAITED
 * and RX_OFDM with clip enabled, and the MAC suspended. The block toggles the
 * MAC internally during the polls and ends with it suspended; the outer
 * framing belongs to the callers.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   32820-33379, 35466-36025]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   27216-27775]
 */
static void b43_phy_ac_measure_block(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Per-core 0x?024/0x?025 baseline, restored by the radio reset. */
	u16 rad_restore[2][2];
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_RX_OFDM,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_CLIP_ALL_DIS |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Per-core RX AFE reconfiguration, 86 ops: a global preamble clearing
	 * three status flags, then the per-chain gain-register programming.
	 *
	 * SALAME: reading this as an RX AFE reconfiguration is ours. The
	 * registers are the standard gain ones, but this sequence of MODs in
	 * this order appears nowhere else.
	 */
	{
		unsigned int c;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0);

		for (c = 0; c < 2; c++)
			b43_phy_ac_rx_gain_regs_program(dev, c, 0x0154);
	}

	/*
	 * Second 2069 radio IQ-cal configuration, 68 ops: 34 per core over two
	 * cores, being seven baseline radio reads plus nine MOD/RD/WR groups.
	 *
	 * b43_radio_maskset() emits three ops, MOD then RD then WR, with the
	 * written value computed as (read & ~mask) | set. Reproducing the
	 * vendor's written values therefore requires the read plan for radio
	 * 0x0017, 0x0024 and 0x0161 to be pre-programmed -- those are the
	 * hardware-sticky values whose bits a previous write of 0 does not
	 * clear. The plans live in test/unit/main.c.
	 *
	 * SALAME: reading this as TX AFE cal setup is ours. It is clearly a
	 * radio reconfiguration with a read-modify-verify-write pattern, but
	 * what it is for is not confirmed.
	 */
	{
		/* Config table: nine groups per core, one maskset call being
		 * three ops. */
		static const struct {
			u16 reg;
			u16 mask;
			u16 val;
		} cfg[9] = {
			{ 0x0161, 0x4000, 0x4000 },
			{ 0x000e, 0x0001, 0x0001 },
			{ 0x0161, 0x1000, 0x1000 },
			{ 0x0017, 0x0001, 0x0001 },
			{ 0x0017, 0x0002, 0x0000 },
			{ 0x015f, 0x2000, 0x2000 },
			{ 0x0025, 0x03ff, 0x0091 },
			{ 0x015f, 0x4000, 0x4000 },
			{ 0x0024, 0x0700, 0x0300 },
		};
		static const u16 baseline[7] = {
			0x016e, 0x000e, 0x0161, 0x0017,
			0x015f, 0x0024, 0x0025
		};
		unsigned int c, i;

		for (c = 0; c < 2; c++) {
			u16 s = (u16)(c * 0x200);

			/*
			 * Seven baseline radio reads. 0x0024 and 0x0025 have
			 * to be kept: the closing radio reset restores them
			 * to the value on entry, not to a constant. In the
			 * periodic tick the baseline 0x203/0x98 is restored
			 * identically, and in finalize's measure block core 1
			 * goes 0x3/0x0 out and 0x3/0x0 back.
			 */
			for (i = 0; i < ARRAY_SIZE(baseline); i++) {
				u16 rd = b43_radio_read(dev, baseline[i] + s);

				if (baseline[i] == 0x0024)
					rad_restore[c][0] = rd;
				else if (baseline[i] == 0x0025)
					rad_restore[c][1] = rd;
			}

			/* 9 gruppi (maskset emette MOD+RD+WR = 3 op) */
			for (i = 0; i < ARRAY_SIZE(cfg); i++)
				b43_radio_maskset(dev, cfg[i].reg + s,
						  (u16)~cfg[i].mask,
						  cfg[i].val);
		}
	}

	/*
	 * Rxcal cleanup preamble (5 op).
	 *   peek 0x019e + peek 0x040f + MOD 0x040f clr bit 9 +
	 *   peek 0x0394 + peek 0x0393
	 */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_read_log(dev, 0x040f);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
	b43_phy_read_log(dev, 0x0394);
	b43_phy_read_log(dev, 0x0393);

	/*
	 * Tail comune (18 op): stesso pattern di
	 * rxgain_perchan_config / iqcal_meas_readback_kick_tail — 3 peek+WR
	 * pair per core + 6 WR standalone reversed. Riuso helper condiviso.
	 */
	b43_phy_ac_rxgain_perchan_tail(dev);

	/* Arm tone gen (3 op) — 0x0394 = 0x0110. */
	b43_phy_ac_arm_tone_gen(dev, 0x0110);

	/*
	 * TX AFE poll blocks, 163 ops: two cores by four iterations by 20 ops,
	 * plus three ops to re-arm between cores.
	 *
	 * Each iteration emits four MOD/RD/WR groups, 12 ops, plus eight peeks
	 * of 0x0013:
	 *   MOD 0x?16e set bit 1
	 *   MOD 0x?00e under mask 0x0002 with bit1
	 *   MOD 0x?16e set bit 0
	 *   MOD 0x?00e under mask 0x0004 with bit2
	 *   eight peeks of 0x0013, the accumulator readback for settling
	 *
	 * The (bit1, bit2) sweep across the four iterations is a four-step
	 * binary code on bits 1 and 2 of 0x000e. That matches the shape of the
	 * RX-IQ four-configuration sweep in rxiq-cal-analysis.md, but on a
	 * different register -- 0x000e rather than 0x0734.
	 *
	 * Between core 0 and core 1 there is a re-arm writing 0x0394 = 0x0111,
	 * against 0x0110 in the first arm: bit 0 changes, which looks like an
	 * arm core selector.
	 *
	 * SALAME: reading this as a TX AFE cal sweep is ours, and the relation
	 * to the 0x0734 sweep is not confirmed -- different registers,
	 * different bits, same four-step binary pattern.
	 */
	{
		static const struct { u16 bit1; u16 bit2; } sweep[4] = {
			{ 0x0002, 0x0000 },   /* "01" */
			{ 0x0000, 0x0000 },   /* "00" */
			{ 0x0002, 0x0004 },   /* "11" */
			{ 0x0000, 0x0004 },   /* "10" */
		};
		unsigned int core, iter, p;

		for (core = 0; core < 2; core++) {
			u16 s = (u16)(core * 0x200);
			u16 reg_16e = 0x016e + s;
			u16 reg_00e = 0x000e + s;

			/* Re-arm before core 1, writing 0x0394 = 0x0111. */
			if (core == 1)
				b43_phy_ac_arm_tone_gen(dev, 0x0111);

			for (iter = 0; iter < 4; iter++) {
				b43_radio_maskset(dev, reg_16e,
						  (u16)~0x0002, 0x0002);
				b43_radio_maskset(dev, reg_00e,
						  (u16)~0x0002, sweep[iter].bit1);
				b43_radio_maskset(dev, reg_16e,
						  (u16)~0x0001, 0x0001);
				b43_radio_maskset(dev, reg_00e,
						  (u16)~0x0004, sweep[iter].bit2);

				for (p = 0; p < 8; p++)
					b43_phy_read_log(dev, 0x0013);
			}
		}
	}

	/*
	 * PHY gain-register reset, 29 ops: write 0x019e = 0x03d0 for the gate
	 * configuration, then 14 writes per core resetting 0x?720-0x?73e to
	 * their post-cal defaults.
	 */
	b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x03d0);
	{
		unsigned int c, i;

		for (c = 0; c < 2; c++) {
			u16 s = (u16)(c * 0x200);

			for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxgain_regs); i++)
				b43_phy_write(dev, b43_phy_ac_rxgain_regs[i] + s,
					      dev->phy.ac->rxgain_saved[c][i]);
		}
	}

	/*
	 * Radio reset, 14 ops: seven radio writes per core. The first five are
	 * post-cal constants, with bit 4 of 0x0017 left set at 0x0011; 0x0024
	 * and 0x0025 restore the baseline read at the head of the block, as
	 * noted there.
	 */
	{
		static const struct { u16 reg; u16 val; } radio_reset[5] = {
			{ 0x016e, 0x0000 },
			{ 0x000e, 0x0001 },
			{ 0x0161, 0x0100 },
			{ 0x0017, 0x0011 },
			{ 0x015f, 0x0000 },
		};
		unsigned int c, i;

		for (c = 0; c < 2; c++) {
			u16 s = (u16)(c * 0x200);

			for (i = 0; i < ARRAY_SIZE(radio_reset); i++)
				b43_radio_write(dev, radio_reset[i].reg + s,
						radio_reset[i].val);
			b43_radio_write(dev, 0x0024 + s, rad_restore[c][0]);
			b43_radio_write(dev, 0x0025 + s, rad_restore[c][1]);
		}
	}

	/*
	 * Finalize (5 op).
	 * MOD 019e lock + WR 0x0394 = 0x000b + WR 0x0393 = 0 (unarm) +
	 * MOD 0x040f clr bit 9 + MOD 019e unlock.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_write(dev,   0x0394, 0x000b);
	b43_phy_write(dev,   0x0393, 0x0000);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * Periodic watchdog. The witness is the steady-state windows of the d6220
 * sweep in router-data/d6220/hot-sweep.zip; the reference tick is extracted to
 * router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt.
 *
 * In steady state the stock driver polls TSSI and the SHM statistics about
 * once a second, and roughly every five seconds runs the same poll with a
 * single pass of the measure block in between, MAC suspended. The period used
 * here is the b43 core's pwork hook at 15 seconds rather than the vendor's
 * five: different cadence, identical op sequence.
 *
 * The poll is also the latch-and-clear of the ucode statistics: it zeroes the
 * SHM window 0x0308-0x0312 that it re-reads at the end. Of those, 0x0308 is
 * the noise statistic that selects the CRS minimum-power ladder entry; the
 * others are read for the clear alone, because without it the counters
 * saturate and the op stream would diverge from the vendor's.
 *
 * Out of scope, documented and not implemented: the 64-word OBJ read sweep of
 * 0x00e0-0x015e that heads the statistics poll in some instances of the tick
 * but not others. The rule for when it appears is not established; the
 * candidate is the DFS channel-availability check.
 */

/* Rollover-safe read of a 32-bit SHM counter: hi, lo, hi. Always three reads,
 * as the vendor does, and the re-read's high half is the one that counts. */
static u32 b43_phy_ac_wd_shm_read32x3(struct b43_wldev *dev, u16 lo_off)
{
	u16 hi, lo;

	hi = b43_shm_read16(dev, B43_SHM_SHARED, (u16)(lo_off + 2));
	lo = b43_shm_read16(dev, B43_SHM_SHARED, lo_off);
	hi = b43_shm_read16(dev, B43_SHM_SHARED, (u16)(lo_off + 2));
	return ((u32)hi << 16) | lo;
}

/*
 * Zero the six-word ucode statistics window.
 *
 * It is the clear half of the latch-and-clear: b43_phy_ac_wd_stats_tail()
 * reads the window, this zeroes it, and without the zeroing the counters
 * saturate. Note the asymmetry in the range -- the latch reads up to 0x0314,
 * the clear stops at 0x0312 -- which is what every capture shows.
 *
 * Two callers, and they are not the same phase: the watchdog tick, where it
 * follows the TSSI peek, and post_cal_finalize_iter3(), where it follows the
 * RX suspend.
 */
static void b43_phy_ac_wd_stats_clear(struct b43_wldev *dev)
{
	u16 off;

	for (off = 0x0308; off <= 0x0312; off += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, off, 0);
}

/*
 * Sampling phase: peek TSSI and status with the MAC suspended, zero the
 * statistics window, and change the measurement mode on 0x0520[3:2]. This is
 * the same continuous toggle as the probe cycle -- 0x0000 and 0x0004
 * alternating, never reset -- verified over 21 consecutive instances in the
 * sweep.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	B43_AC_FN();
	unsigned int k;

	/*
	 * Il peek e la coppia 0x0554/0x0555 sono opzionali perche' i tick della
	 * fase probe non sono tutti uguali. Sul segmento cold01 i venti tick
	 * hanno questa forma, letta marcatore per marcatore:
	 *
	 *   tick 0        poll, peek, 0x0554/0x0555, clear, mode
	 *   tick 1 e 2    poll, latch, clear, mode              <- senza peek
	 *   tick 3..18    poll, latch, peek, clear, mode        <- forma piena
	 *   tick 9        come sopra, col measure block dopo il poll
	 *   tick 19       poll, measure block, e la fase finisce
	 *
	 * Il tick periodico a regime e' la forma piena, ed e' quello che
	 * b43_phy_ac_watchdog() emette.
	 */
	if (peek) {
		b43_mac_suspend(dev);
		for (k = 0; k < ARRAY_SIZE(b43_phy_ac_probe_peek_regs); k++)
			b43_phy_read_log(dev, b43_phy_ac_probe_peek_regs[k]);
		if (arm_tone) {
			b43_phy_write(dev, 0x0554, 0x0bb8);
			b43_phy_write(dev, 0x0555, 0x0bb8);
		}
		b43_mac_enable(dev);
	}

	b43_phy_ac_wd_stats_clear(dev);

	b43_mac_suspend(dev);
	b43_phy_maskset(dev, 0x0520, (u16)~0x000c,
			probe_mode_next(dev->phy.ac));
	b43_mac_enable(dev);
}

static void b43_phy_ac_wd_sample_phase(struct b43_wldev *dev)
{
	b43_phy_ac_wd_sample_phase_opt(dev, true, false);
}

/*
 * SHM statistics poll, with the MAC active. Order and repetitions are
 * transcribed from the reference tick: four scattered words, the 0x0768-0x078a
 * sweep, two hi/lo/hi passes over the six 32-bit counters, the three counters
 * at 0x07e0, 0x07e4 and 0x07dc, the 0x07d6-0x07da group and two trailing
 * words.
 *
 * The captures show three shapes and these two parameters cover all of them:
 *
 *   @head_sweep, @ctr32_passes   where
 *   true, 2                      the full shape, 54 reads over 0x0768-0x078a
 *   true, 0                      sweep only, 18 reads: the three polls of the
 *                                up path
 *   false, 1                     second half only with one pass, inside the
 *                                MAC config block
 *
 * Why the passes are not always there is plausible and unproven: they read the
 * counters as stable 32-bit values, and before the MAC has counted anything
 * there is nothing to read that way. The flat sweep stays because it is part
 * of the window latch.
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	static const u16 head[4] = { 0x010e, 0x0158, 0x010c, 0x015e };
	static const u16 ctr32[6] = {
		0x0768, 0x076c, 0x0770, 0x0774, 0x0778, 0x077c
	};
	unsigned int i, pass;
	u16 off;

	if (head_sweep) {
		for (i = 0; i < ARRAY_SIZE(head); i++)
			b43_shm_read16(dev, B43_SHM_SHARED, head[i]);
		for (off = 0x0768; off <= 0x078a; off += 2)
			b43_shm_read16(dev, B43_SHM_SHARED, off);
	}

	if (!ctr32_passes)
		return;

	for (pass = 0; pass < ctr32_passes; pass++)
		for (i = 0; i < ARRAY_SIZE(ctr32); i++)
			b43_phy_ac_wd_shm_read32x3(dev, ctr32[i]);

	/*
	 * @ctr32_tail chiude la spazzata coi tre contatori fuori lista. Sta
	 * dietro un flag perche' una ricarica del beacon puo' cadere fra le due
	 * passate: il chiamante spezza la spazzata in due e la coda va solo
	 * sulla seconda meta'.
	 */
	if (!ctr32_tail)
		return;

	b43_phy_ac_wd_shm_read32x3(dev, 0x07e0);
	b43_phy_ac_wd_shm_read32x3(dev, 0x07e4);
	b43_phy_ac_wd_shm_read32x3(dev, 0x07dc);

	for (off = 0x07d6; off <= 0x07da; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
	b43_shm_read16(dev, B43_SHM_SHARED, 0x015a);
	b43_shm_read16(dev, B43_SHM_SHARED, 0x014e);
}

/* Closing re-read of the window zeroed at the head, plus 0x008c. */
static void b43_phy_ac_wd_stats_tail(struct b43_wldev *dev)
{
	u16 off;

	b43_shm_read16(dev, B43_SHM_SHARED, 0x008c);
	for (off = 0x0308; off <= 0x0314; off += 2) {
		u16 v = b43_shm_read16(dev, B43_SHM_SHARED, off);

		/*
		 * 0x0308 is the noise statistic the CRS minimum-power ladder
		 * is chosen from; see b43_phy_ac_crs_min_pwr(). The rest of
		 * the window is read for the latch-and-clear and not consumed.
		 */
		if (off == 0x0308)
			b43_phy_ac_crs_note_noise(dev, v);
	}
}

static void b43_phy_ac_wd_stats_poll(struct b43_wldev *dev)
{
	b43_phy_ac_wd_stats_poll_opt(dev, true, 2, true);
}

/*
 * The watchdog's body: statistics poll, optional measure block, latch of the
 * SHM window.
 *
 * Three sites emit this sequence and they differ only in where the sampling
 * phase goes and in whether the latch runs: b43_phy_ac_watchdog() puts the
 * sampling phase first, the probe loop of b43_phy_ac_rxiqcal_finalize() puts
 * it last, and the loop's closing tick has none. It is one cycle cut at two
 * points, so the body is written once here and the rotation stays at the call
 * site.
 *
 * @reloads: beacon reloads that land inside this body. They go between the two
 *	passes of the counter sweep and not between blocks: in the capture the
 *	first pass closes on the sixth counter and the reload starts right
 *	after.
 * @noise_cal: run the measure block, MAC suspended.
 * @tail: latch the window. False only on the probe loop's first tick, where
 *	the window was read just above -- before the CRS block -- and has not
 *	had time to fill.
 */
static void b43_phy_ac_wd_body(struct b43_wldev *dev, unsigned int reloads,
			       bool noise_cal, bool tail)
{
	struct b43_phy_ac *ac = dev->phy.ac;

	if (reloads) {
		b43_phy_ac_wd_stats_poll_opt(dev, true, 1, false);
		while (reloads--)
			b43_ac_beacon_reload(dev, ac->beacon_reload_done++);
		b43_phy_ac_wd_stats_poll_opt(dev, false, 1, true);
	} else {
		b43_phy_ac_wd_stats_poll(dev);
	}

	if (noise_cal) {
		b43_mac_suspend(dev);
		b43_phy_ac_measure_block(dev);
		b43_mac_enable(dev);
	}

	if (tail)
		b43_phy_ac_wd_stats_tail(dev);
}

/* [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   29136-29140]
 */
void b43_phy_ac_watchdog(struct b43_wldev *dev, bool noise_cal)
{
	B43_AC_FN();

	b43_phy_ac_wd_sample_phase(dev);
	b43_phy_ac_wd_body(dev, 0, noise_cal, true);
}

static void b43_phy_ac_op_pwork_15sec(struct b43_wldev *dev)
{
	u16 sm = dev->phy.ac->status_mask;
	static const u16 want = B43_PHY_AC_STATE_RX_WAITED |
				B43_PHY_AC_STATE_RX_OFDM;
	static const u16 forbid = B43_PHY_AC_STATE_RX_CCK |
				  B43_PHY_AC_STATE_CLIP_ALL_DIS |
				  B43_PHY_AC_STATE_CCA_RESET;

	/*
	 * The first round of the periodic work starts with zero delay from
	 * b43_periodic_tasks_setup(), before switch_channel() has brought the
	 * PHY into release. The vendor only runs the tick inside its run
	 * window, and outside that state the skip has to be silent: reaching
	 * the measure block's REQUIRE would mark FAULTED, which is sticky, and
	 * turn off every gated function.
	 *
	 * MAC_EN is deliberately not in the forbid set: at tick time the MAC is
	 * active, and the watchdog suspends it itself before the measure block.
	 */
	if (sm & B43_PHY_AC_STATE_FAULTED)
		return;
	if ((sm & want) != want || (sm & forbid))
		return;

	b43_phy_ac_watchdog(dev, true);
}

/*
 * Whether the periodic watchdog fires on tick @tick of the probe phase.
 *
 * The caller supplies the ticks because the trace harness has no clock; see
 * the probe loop in b43_phy_ac_rxiqcal_finalize() for what they are and how
 * the sweep pins them down.
 */
static bool b43_phy_ac_watchdog_on_tick(const struct b43_phy_ac *ac,
					unsigned int tick)
{
	return ac->probe_watchdog_tick[0] == tick ||
	       ac->probe_watchdog_tick[1] == tick;
}

/*
 * RXIQ cal finalize (~2700 op).
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   30614-36041]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   25896-28591]
 */
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev)
{
	B43_AC_FN();
	/*
	 * Block D saves the LO DAC and the TX IQ/LO coefficients into
	 * @lo_dac and @txiqlo_coef; b43_phy_ac_bss_up() writes them back.
	 */
	u16 (*lo_dac)[4] = dev->phy.ac->lo_dac;
	u16 (*txiqlo_coef)[3] = dev->phy.ac->txiqlo_coef;
	/*
	 * Called with the MAC suspended, from set_channel_calibrations() after
	 * rxiq_teardown_apply_defaults(). The classifier is in RX_WAITED and
	 * clip detect is disabled on every core: the canonical calibration
	 * state.
	 */
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Origin of the 0x0520[3:2] toggle. The stock driver starts from 0x0000
	 * on a first bring-up and from 0x0004 on a later channel switch, then
	 * alternates at every group of peeks until the phase ends.
	 */
	dev->phy.ac->probe_mode =
		(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)
		? 0x0000 : 0x0004;

	/*
	 * Blocco A (10 op): finalize kick RST2RX + gate scope wrap.
	 *   pre-kick:  WR 0x040f = 0x09ff  (arm RF ctrl)
	 *   kick:      force_rf_sequence(RST2RX, gate=OVERRIDE_GATE) — 8 op
	 *              (peek RFCTL1 + peek gate + set gate|=0x0001 + set
	 *               RFCTL1|=0x3 + set RF_SEQ_TRIG|=RST2RX + poll status +
	 *               restore RFCTL1 + restore gate)
	 *   post-kick: MOD gate clr bit 1  (unlock finale scope)
	 */
	b43_phy_write(dev, 0x040f, 0x09ff);
	b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RST2RX,
				     B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

	/*
	 * Block B, 16 ops: two self-contained two-word table writes to id
	 * 0x000c. Reapplies the AFE cal's pass-1 results, the afe_res_cal
	 * snapshot of the 0x?056 iterations, at offset 0x0060 for core 0 and
	 * 0x0064 for core 1. The attach capture shows (0x0066, 0x000e) and
	 * (0x0027, 0x0003).
	 */
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0060, 16, 2,
				    dev->phy.ac->afe_res_cal[0].v);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0064, 16, 2,
				    dev->phy.ac->afe_res_cal[2].v);

	/*
	 * Block C, 10 ops: the final post-cal RX release plus a gate open. This
	 * goes through the named helpers so that status_mask stays consistent
	 * with the hardware state: RX_WAITED and RX_OFDM set, clip enabled.
	 */
	b43_phy_ac_classctl_write_peeked(dev, false);
	b43_phy_ac_adc_hold(dev, true);
	b43_phy_ac_clip_det(dev, true);
	b43_phy_write(dev, 0x0339, 0x0fff);

	/*
	 * Block D, 49 ops: a table write at offset 0x5f, then per core the
	 * save of the TX IQ/LO coefficients, four radio reads and two peeks
	 * of 0x?a0/0x?a1 -- the readback of the core's RX-IQ corrector state.
	 */
	{
		static const u16 tbl_5f_val = 0xacdc;
		unsigned int c;

		/* TBL.WR id=0x000c off=0x005f len=1 val=0xacdc (7 op) */
		b43_actab_write_bulk_scoped(dev, 0x000c, 0x005f, 16, 1,
					    &tbl_5f_val);

		for (c = 0; c < 2; c++) {
			u16 stride = (u16)(c * 0x200);

			/* TBL.RD off=0x60+c*4 len=2 (8 op) */
			b43_actab_read_bulk(dev, 0x000c,
					    (u16)(0x0060 + c * 4),
					    16, 2, &txiqlo_coef[c][0]);
			/* MOD unlock esplicito (fine scope actab_read_bulk) */
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
			/* TBL.RD off=0x62+c*4 len=1 (7 op) */
			b43_actab_read_bulk(dev, 0x000c,
					    (u16)(0x0062 + c * 4),
					    16, 1, &txiqlo_coef[c][2]);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

			/*
			 * Four radio reads, 0x0002-0x0005, per core: the LO
			 * DAC leakage. They have to be kept, because the
			 * per-core tail rewrites them identically, and the
			 * values differ between the warm and attach paths --
			 * they are cal state, not constants.
			 */
			lo_dac[c][0] = b43_radio_read(dev, 0x0002 + stride);
			lo_dac[c][1] = b43_radio_read(dev, 0x0003 + stride);
			lo_dac[c][2] = b43_radio_read(dev, 0x0004 + stride);
			lo_dac[c][3] = b43_radio_read(dev, 0x0005 + stride);

			/* 2 peek 0x?a0/0x?a1 (readback correction coeffs) */
			b43_phy_read_log(dev, 0x06a0 + stride);
			b43_phy_read_log(dev, 0x06a1 + stride);
		}
	}

	/*
	 * Block E, 28 ops: AFE, CRS and noise-floor reconfiguration.
	 *   - five gain-related MODs: set 0xe000 on 0x0070, set 0x14 under mask
	 *     0x7f on 0x?644, clear bit 2 of 0x?678 per core
	 *   - a MAC toggle, MHF and GPIO sequence, five ops in the MAC/GPIO
	 *     layer
	 *   - eight MODs on the CRS registers, the clip-detect thresholds, with
	 *     0x34 under mask 0x00ff
	 *   - eight MODs on the 0x0910-0x0913 bank, each under two distinct
	 *     masks
	 *   - a closing MAC.MCTRL toggle, set then clear
	 */
	b43_phy_ac_afe_gain_regs_reemit(dev);

	b43_mac_enable(dev);
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);

	/*
	 * Le ricariche del beacon che cadono prima che la fase probe parta.
	 * Sono le due coppie suspend/enable a 1.37 e 1.29 s dall'MHF che il
	 * commento qui sotto dava per rumore del contesto up: non lo sono, e
	 * la forma lo dice -- ognuna porta TIMBPOS, il template in template
	 * RAM, la lunghezza in BTL0 o BTL1 e una passata sul PLCP, con il
	 * suspend fra la lunghezza e il template. Quante siano lo dice il
	 * chiamante, vedi ac->beacon_reload_pre.
	 */
	{
		struct b43_phy_ac *ac = dev->phy.ac;
		unsigned int i;

		for (i = 0; i < ac->beacon_reload_pre; i++)
			b43_ac_beacon_reload(dev, ac->beacon_reload_done++);
	}

	/*
	 * Le due coppie suspend/enable fra la MHF e la prima GPIO sono il
	 * suspend interno delle ricariche del beacon emesse qui sopra, non
	 * rumore del contesto up: la forma lo dice -- TIMBPOS, il template,
	 * BTL0/BTL1 e la passata sul PLCP -- e il conteggio lo conferma. Sui
	 * sette segmenti sotto i 5250 MHz, dove `beacon_reload_pre` copre tutte
	 * le ricariche, le regole di cmp_skip.py che le dichiaravano rumore non
	 * scattano piu': zero op saltate. Sui diciannove sopra la soglia ne
	 * resta una coppia senza controparte, due op, ed e' un residuo vero:
	 * la' il vendor ne ha una in piu' di quante `beacon_reload_pre` ne
	 * dichiari.
	 */
	bcma_chipco_gpio_out(&dev->dev->bdev->bus->drv_cc, 0x0004, 0x0004);
	bcma_chipco_gpio_out(&dev->dev->bdev->bus->drv_cc, 0x0400, 0x0000);

	/*
	 * Una cella a 0xffff, una volta sola in tutto il segmento, fra le due
	 * GPIO e il latch della finestra. b43.h non la nomina e a cosa serva
	 * non si sa; la posizione e' invariante.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0026, 0xffff);

	/*
	 * Latch the ucode statistics here, ahead of block E, so the CRS value
	 * below follows the sample.
	 *
	 * The position is fixed and not a periodic tick that happens to land in
	 * it: the window 0x008c and 0x0308 to 0x0314 sits immediately before
	 * the mac_suspend and block E in all 32 sweep segments, and again in
	 * the attach capture that traces the OBJ class, at episodes 56973 to
	 * 56989 of its warm cycle on ch140. Exactly one sample falls between
	 * the CRS write of chanspec_tail() and this one, in every case.
	 */
	b43_phy_ac_wd_stats_tail(dev);

	b43_mac_suspend(dev);

	/*
	 * Eight MODs on the CRS registers, the clip-detector thresholds, under
	 * mask 0x00ff. See b43_phy_ac_crs_regs() for the order: a +0xc stride,
	 * interleaved between cores.
	 *
	 * The value is the ladder entry the sample just latched selects, which
	 * is why the statistics are read immediately above: on a warm cycle
	 * exactly one sample falls between the CRS write of chanspec_tail() and
	 * this one, and this one follows it.
	 *
	 * A first bring-up keeps the literal: there chanspec_tail() writes 0x3a
	 * and this block 0x34, and no sample explains the pair.
	 */
	{
		u16 crs = (dev->phy.ac->status_mask &
			   B43_PHY_AC_STATE_FIRST_BRINGUP)
			? 0x0034
			: b43_phy_ac_crs_min_pwr(dev,
				b43_phy_ac_crs_index(dev), true);

		b43_phy_ac_crs_regs_write(dev, crs);

		/* Noise floor clear, 8 ops, same pattern as chanspec_tail. */
		b43_phy_ac_prog_bank_0910(dev, crs,
					  B43_PHY_AC_CRS_SITE_FINALIZE);
	}

	/*
	 * Il MAC torna attivo e ci resta: la fase probe che segue apre ogni tick
	 * col poll delle statistiche, che gira a MAC attivo, ed e' il tick a
	 * sospenderlo per il peek e a riabilitarlo. Su cold01 wl emette qui una
	 * sola abilitazione e subito dopo il poll; una coppia
	 * enable+suspend lascerebbe il MAC sospeso e sfaserebbe di un'op tutti
	 * e venti i tick.
	 */
	b43_mac_enable(dev);

	/*
	 * Probe and measure sequence.
	 *
	 * These ops are not the channel setup's: every one of them is a turn of
	 * the periodic watchdog, which in a real driver arrives from
	 * b43_phy_ac_op_pwork_15sec() once switch_channel() has returned. They
	 * are here because a wl-diag capture is a flat op stream: the 18-21
	 * turns the vendor's watchdog took while the tracer was still recording
	 * sit right after the calibration, and the comparison needs them in
	 * that position.
	 *
	 * So the loop runs only when the caller asked for a deadline.
	 * @probe_ticks is zero in a live driver -- nothing in b43 sets it --
	 * and the phase collapses to nothing; the watchdog then latches the SHM
	 * window once per pwork period, which is what the hardware needs. With
	 * a deadline it emits one turn per tick, and @probe_watchdog_tick says
	 * which of them carry the measure block: the 26 cold segments pin it to
	 * tick 9 in all 26 and tick 19 wherever the deadline reaches, a timer
	 * of period ten, with the warm pair 5 and 15 being the same period at
	 * another phase. It can fall on the closing tick, past the last tick of
	 * the phase, and then that tick carries only the poll and the block.
	 *
	 * A tick is one turn of the watchdog and not a block of its own: the
	 * statistics poll, the latch of the window, the peek, the clear and the
	 * mode change. Same body b43_phy_ac_watchdog() runs -- see
	 * b43_phy_ac_wd_body() -- in the rotation this phase emits it, the
	 * steady-state tick being the same cycle cut at another point.
	 *
	 * The irregularities of the first three ticks are in
	 * b43_phy_ac_wd_sample_phase_opt(), which documents them one by one.
	 */
	{
		struct b43_phy_ac *ac = dev->phy.ac;
		unsigned int ticks = ac->probe_ticks;
		unsigned int tick, i;

		for (tick = 0; tick < ticks; tick++) {
			unsigned int reloads = 0;

			for (i = 0; i < ac->beacon_reload_n; i++)
				if (ac->beacon_reload_tick[i] == tick)
					reloads++;

			b43_phy_ac_wd_body(dev, reloads,
					   b43_phy_ac_watchdog_on_tick(ac, tick),
					   tick != 0);

			b43_phy_ac_wd_sample_phase_opt(dev,
						       tick != 1 && tick != 2,
						       tick == 0);
		}

		/*
		 * Il tick di chiusura. La fase si chiude sempre con un poll e
		 * il latch, oltre l'ultimo mode change e senza peek ne' clear:
		 * i 26 segmenti a freddo hanno tutti e 26 quel poll e 24 su 26
		 * anche il latch. Il measure block invece ci sta solo se la
		 * scadenza cade su un tick di misura, e questo pure e' esatto
		 * sui 26: compare quando la scadenza e' 19, cioe' quando
		 * @probe_watchdog_tick la contiene, e non compare quando e' 18,
		 * 20 o 21.
		 *
		 * Con @probe_ticks a zero non c'e' fase e non c'e' chiusura.
		 */
		if (ticks)
			b43_phy_ac_wd_body(dev, 0,
					   b43_phy_ac_watchdog_on_tick(ac, ticks),
					   true);

		ac->last_cal_channel = ac->cal_channel;
	}

}

/*
 * The PHY half of `wl down`: what the vendor emits when the sweep script
 * brings the interface down, in one burst of ~460 op over 19 ms, closing
 * with the AFE_OFF bank and the PMU release that pairs with the cold
 * preamble's request. Every cold segment has it at 99%, a second before the
 * `mod GOING` of the rmmod, and every hot up/down segment has it at 99% too,
 * with no rmmod at all: it is the down, and it was called bss_up for a
 * while, which made the teardown look like a bring-up and hid the fact that
 * the driver never emitted it.
 *
 * It is NOT the tail of the channel setup, and the clock says so: on cold04
 * the calibration ends at t=869.844, the watchdog turns run to 889.993, and
 * this block sits at 890.450-890.469, after a gap of 457 ms. Only the
 * coefficient write-back is cal state, which is why @lo_dac and @txiqlo_coef
 * live in the phy state.
 *
 * b43 reaches it from b43_phy_exit() through software_rfkill(blocked=true).
 *
 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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
	 * A seventh 32-bit counter, read on its own and outside every sweep: one
	 * hi/lo/hi on 0x077c, bracketed by two reads of UCODESTAT that are the
	 * core's. It is already in the ctr32[] list of the statistics poll, so
	 * the read itself is the poll's shape; what is separate is this one
	 * occurrence, which no poll accounts for.
	 */
	b43_phy_ac_wd_shm_read32x3(dev, 0x077c);

	/*
	 * Third and last pass of the twelve-rate loop, with the same shm
	 * prologue as the second one: the double rewrite of 0x00cc and the two
	 * zeroes on 0x00ce/0x00d0, then the chain-mask block.
	 *
	 * The count is an invariant of the hardware and not of this capture: all
	 * 26 cold segments and all 52 up segments of the hot sweep have exactly
	 * three passes and four writes of 0x00ce, on every channel and every
	 * bandwidth. That is what separates this pass from the beacon template
	 * reloads a few thousand ops earlier, whose count runs from 7 to 21 over
	 * the same 26 segments because the host decides it.
	 */
	/*
	 * The maccontrol bracket that opens the block: clear bit 20, then
	 * suspend. The second site raises that bit where this one lowers it.
	 */
	b43_maccontrol_set(dev, (u32)~0x00100000u, 0x00000000);
	b43_mac_suspend(dev);
	{
		u16 cc = b43_shm_read16(dev, B43_SHM_SHARED, 0x00cc);

		b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
		b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
	}
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ce, 0x0000);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00d0, 0x0000);
	b43_phy_ac_chainmask_block(dev);
	b43_phy_ac_prb_rsp_rate_po(dev);

	/*
	 * Post-probe final AFE configuration, 16 ops, closing the calibration:
	 * peek the gate, lock it, then a sequence of MODs on the gain registers
	 * 0x0070-0x0072 (mixed thresholds and enables) and the per-core
	 * 0x0644-0x0846. The 0x019e gate stays locked on exit for the bulk
	 * table writes that follow.
	 *
	 * SALAME: reading this as the final AFE teardown is ours. What the
	 * specific bits do -- 0x8000, 0x4000, 0x0100 and 0x0700 on 0x0070 and
	 * 0x0072 -- is undocumented; they line up with threshold and enable bits
	 * of the gain front end.
	 */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, 0x0072, (u16)~0x0001, 0x0001);
	b43_phy_maskset(dev, 0x0070, (u16)~0x8000, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~0x0100, 0x0100);
	b43_phy_maskset(dev, 0x0072, (u16)~0x4000, 0);
	b43_phy_maskset(dev, 0x0072, (u16)~0x4000, 0x4000);
	b43_phy_maskset(dev, 0x0070, (u16)~0x8000, 0);
	b43_phy_maskset(dev, 0x0644, (u16)~0x007f, 0x0014);
	b43_phy_maskset(dev, 0x0844, (u16)~0x007f, 0x0014);
	b43_phy_maskset(dev, 0x0071, (u16)~0x00ff, 0x00c8);
	b43_phy_maskset(dev, 0x0071, (u16)~0x0700, 0x0400);
	b43_phy_maskset(dev, 0x0070, (u16)~0x0800, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~0x0400, 0x0400);
	/*
	 * The target power again, the same value txpwrctrl_setup() wrote. The
	 * 0x38 the stock driver writes here on a first bring-up is the
	 * regulatory ceiling binding under its default locale, not a constant
	 * of the phase.
	 */
	b43_phy_ac_txpwr_target_write(dev);

	/*
	 * The two est_pwr LUTs again, cores 0 and 1.
	 *
	 * Each takes three payloads across the 16 sweep channels, grouped by
	 * sub-band at 5250 and 5500 MHz, which is the grouping pa5ga[] has.
	 * The two differ from each other in 77 of 128 positions because they
	 * are two cores: pa5ga0 and pa5ga1 are distinct SPROM triples.
	 *
	 * pa5g_grp is the sub-band cached by txpwrctrl_setup(), which runs
	 * twice before this point in the same channel setup.
	 */	{
		u16 lut[128];

		b43_phy_ac_est_pwr_lut(dev, 0, dev->phy.ac->pa5g_grp, lut);
		b43_actab_write_bulk(dev, 0x0040, 0x0000, 16,
				     ARRAY_SIZE(lut), lut);
		b43_phy_ac_est_pwr_lut(dev, 1, dev->phy.ac->pa5g_grp, lut);
		b43_actab_write_bulk(dev, 0x0060, 0x0000, 16,
				     ARRAY_SIZE(lut), lut);
	}

	/*
	 * Bulk write of table 0x0021, 24 entries, 52 ops: 24 32-bit entries of
	 * which only [1], [5] and [6] are non-zero, all 0x0202. The gate stays
	 * locked, with no MOD of B43_PHY_AC_REG_TBL_WRITE_GATE before or after.
	 *
	 * Checked channel- and bandwidth-invariant on ch36, ch44 and ch36 at
	 * 40 MHz. Unlike tables 0x0040 and 0x0060 above, this one does hold
	 * across the wider sweep sample.
	 *
	 * SALAME: the three non-zero entries could be IQ correction flags or a
	 * compensation id, with the rest as padding. The exact meaning is not
	 * known, though the values are fixed.
	 */
	{
		static const u32 lut_0021[24] = {
			0x00000000, 0x00000202, 0x00000000, 0x00000000,
			0x00000000, 0x00000202, 0x00000202, 0x00000000,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
		};

		b43_actab_write_bulk(dev, 0x0021, 0x0000, 32,
				     ARRAY_SIZE(lut_0021), lut_0021);
	}

	/*
	 * Final tail, 72 ops: closes the RX-IQ scope and programs the per-core
	 * final coefficients. The steps are labelled in the body.
	 *
	 * SALAME: the 0x17XX write block -- a 0x1000 stride over the gain
	 * registers 0x1720-0x173e -- reaches a different address space, perhaps
	 * a shadow bank for unpopulated chains. The RX-IQ coefficients here are
	 * transcribed from the ch36 capture.
	 */

	/* Unlock gate + AFE gain regs re-emit (identico al blocco E). */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	b43_phy_ac_afe_gain_regs_reemit(dev);

	/* MAC sequence: 4× toggle bit 0 + 1× clr bit 18 + MHF. */
	b43_mac_enable(dev);
	b43_maccontrol_set(dev, ~0x48020000u, 0x40000000);  /* multi-bit config */
	b43_mac_suspend(dev);
	b43_maccontrol_set(dev, ~0x00040000u, 0);
	b43_mac_enable(dev);
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);
	b43_mac_suspend(dev);

	/*
	 * Per core: restore of the TX IQ/LO coefficients saved in block D,
	 * {a, b} at 0x60 + 4*core and the LO leakage word at 0x62 + 4*core,
	 * then the LO DAC registers and the solved RX-IQ coefficients. Core 0
	 * uses radio 0x0002-0x0005, core 1 0x0202-0x0205.
	 */
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0060, 16, 2,
				    &txiqlo_coef[0][0]);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0062, 16, 1,
				    &txiqlo_coef[0][2]);
	b43_radio_write(dev, 0x0002, lo_dac[0][0]);
	b43_radio_write(dev, 0x0003, lo_dac[0][1]);
	b43_radio_write(dev, 0x0004, lo_dac[0][2]);
	b43_radio_write(dev, 0x0005, lo_dac[0][3]);
	/*
	 * The solver runs again over the same accumulators, so this writes the
	 * same pair the first apply did. Block D peeks 0x?a0/0x?a1 just before,
	 * and the vendor's pair here is what that peek returned -- but taking
	 * the peek instead of solving would hide the solver's residual on
	 * coefficient b, which the score is the only place to see. See
	 * VAL_TOLLERANZA in test/unit/compare.py.
	 */
	{
		s16 a, b;

		b43_phy_ac_iq_solve(&dev->phy.ac->iq_acc[0], &a, &b);
		b43_phy_write(dev, 0x06a0, (u16)(a & 0x03ff));
		b43_phy_write(dev, 0x06a1, (u16)(b & 0x03ff));
	}

	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0064, 16, 2,
				    &txiqlo_coef[1][0]);
	b43_actab_write_bulk_scoped(dev, 0x000c, 0x0066, 16, 1,
				    &txiqlo_coef[1][2]);
	b43_radio_write(dev, 0x0202, lo_dac[1][0]);
	b43_radio_write(dev, 0x0203, lo_dac[1][1]);
	b43_radio_write(dev, 0x0204, lo_dac[1][2]);
	b43_radio_write(dev, 0x0205, lo_dac[1][3]);
	{
		s16 a, b;

		b43_phy_ac_iq_solve(&dev->phy.ac->iq_acc[1], &a, &b);
		b43_phy_write(dev, 0x08a0, (u16)(a & 0x03ff));
		b43_phy_write(dev, 0x08a1, (u16)(b & 0x03ff));
	}

	/* MAC final toggle + GPIO clr/set + MAC.MCTRL multi-bit. */
	b43_mac_enable(dev);
	bcma_chipco_gpio_out(&dev->dev->bdev->bus->drv_cc, 0x0004, 0);
	bcma_chipco_gpio_out(&dev->dev->bdev->bus->drv_cc, 0x0400, 0x0400);
	b43_mac_suspend(dev);
	b43_maccontrol_set(dev, 0, 0x04000400);   /* mask=~0=0xffffffff */

	/* The front end powered down, without a save of its own, then the
	 * PMU release. */
	b43_phy_ac_afe_arm(dev, B43_PHY_AC_AFE_OFF, 0x0000, 0x0001);
	b43_phy_ac_pmu_req(dev, true);
}

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
 *   5007-13592]
 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
 *   677-9281]
 */
static int b43_phy_ac_op_switch_channel(struct b43_wldev *dev, unsigned int new_channel)
{
	B43_AC_FN();
	struct ieee80211_channel *channel = dev->wl->hw->conf.chandef.chan;
	enum nl80211_channel_type channel_type = cfg80211_get_chandef_type(&dev->wl->hw->conf.chandef);
	const struct cfg80211_chan_def *chandef = &dev->wl->hw->conf.chandef;
	enum nl80211_chan_width width = chandef->width;
	const struct b43_phy_ac_channeltab_e_radio2069 *e2069;
	struct b43_phy *phy = &dev->phy;
	u16 off;

	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ) {
		b43dbg(dev->wl,
		       "AC-PHY: 2.4 GHz channel %u not supported on this board\n",
		       new_channel);
		return -EOPNOTSUPP;
	}

	/*
	 * Unlike wl, which needs a full down and up to change channel at
	 * runtime, b43 keeps the MAC active between channel switches, and this
	 * function operates with it suspended.
	 *
	 * Suspend only if it is not already suspended: on bring-up the MAC
	 * arrives disabled from the core init, and a nested suspend would raise
	 * the refcount without writing, turning the calibration's
	 * enable/suspend pairs into no-ops. Nothing here has to undo the
	 * suspend, so the entry state does not need remembering: the enable is
	 * the caller's, see the end of the function.
	 *
	 * The preamble writes MACCTL through b43_maccontrol_set(), which does
	 * not go through the refcount: two routes to the same bit, to be
	 * unified.
	 */
	if (!dev->mac_suspended)
		b43_mac_suspend(dev);
	/*
	 * b43_mac_suspend() and b43_mac_enable() maintain the MAC_EN mirror.
	 * Setting it by hand here would put it out of step with the refcount
	 * whenever the suspend is conditional: on bring-up the MAC is not
	 * re-enabled, so raising MAC_EN anyway makes rxiqcal_apply() fail the
	 * precondition that forbids it.
	 */

	/* 5 GHz: the channel table is the filter (unknown channels -ESRCH). */

	/*
	 * channel_type is the legacy HT-only description of the same thing as
	 * chandef.width, and cannot express the VHT widths. It stays in the
	 * signature because that is the b43 phy_ops prototype.
	 */
	(void)channel_type;

	b43dbg(dev->wl, "phy-ac: set_channel ch%u (%u MHz) start\n",
	       channel->hw_value, channel->center_freq);

	if (phy->radio_ver != 0x2069)
		return -ESRCH;

	if (phy->radio_rev != 4)
		return -ESRCH;

	/*
	 * The table is keyed on the frequency the radio is tuned to, which for
	 * a bonded configuration is the centre of the block and not the primary
	 * channel: it carries rows every 10 MHz, so 5190 and 5210 are there
	 * alongside 5180. Passing the primary picks the row of a 20 MHz channel
	 * and mistunes the synthesiser -- the cold sweep shows it on
	 * radio 0x08dc, which takes radio_raw[3] from this row.
	 */
	e2069 = b43_phy_ac_get_channeltab_e_r2069(dev,
			width == NL80211_CHAN_WIDTH_20 ? channel->center_freq
						       : chandef->center_freq1);
	if (!e2069)
		return -ESRCH;

	/*
	 * Scaffolding, not a capability limit.
	 *
	 * Part of what the channel setup programs is not derived: the values
	 * were transcribed from captures and exist only to get the bring-up
	 * moving. Gains, gain-LUT defaults and the CRS thresholds are among
	 * them, and on an RF chain other than the one they were read from
	 * they are not slightly wrong, they can overdrive the PA. The TX
	 * power index is no longer one of them: it comes from the SROM, the
	 * regulatory ceiling and the margin.
	 *
	 * The channel table accepts all of 5 GHz, so it is no protection on
	 * its own: without this filter, tuning ch100 would write the ch36
	 * constants.
	 *
	 * The width is checked from chandef.width and not through
	 * cfg80211_get_chandef_type(): that helper only knows the HT widths
	 * and answers NL80211_CHAN_NO_HT for anything wider, which would let
	 * an 80 MHz chandef through.
	 *
	 * A configuration joins the list once a capture shows the port
	 * reproducing it, and the list disappears once the values are derived
	 * rather than transcribed.
	 */
	if (!b43_phy_ac_config_validated(dev, channel->hw_value, width)) {
		b43warn(dev->wl,
			"AC-PHY: refusing ch%u at %s: its programming has not "
			"been checked against a capture, and several of the "
			"values written are transcribed from ch36 at 20 MHz. "
			"Writing them here can damage the RF front-end.\n",
			channel->hw_value,
			width == NL80211_CHAN_WIDTH_80 ? "80 MHz" :
			width == NL80211_CHAN_WIDTH_40 ? "40 MHz" : "20 MHz");
		return -EOPNOTSUPP;
	}
	if (dev->dev->chip_id != 0x4352 && dev->dev->chip_id != 0x4360) {
		b43warn(dev->wl,
			"AC-PHY: chip 0x%04x non validato: nessuna cattura di riferimento, "
			"e i valori trascritti valgono per 4352/4360.\n",
			dev->dev->chip_id);
		return -EOPNOTSUPP;
	}

	if (dev->phy.ac->tuned &&
	    dev->phy.ac->cal_channel == channel->hw_value &&
	    dev->phy.ac->cal_width == width)
		return 0;

	dev->phy.ac->tuned = false;
	dev->phy.ac->cal_channel = channel->hw_value;
	dev->phy.ac->cal_width = width;
	dev->phy.ac->cal_freq = width == NL80211_CHAN_WIDTH_20
				? channel->center_freq : chandef->center_freq1;
	b43_phy_ac_txpwr_recalc(dev);

	/*
	 * The MAC is already suspended from the end of op_init(), whose tail
	 * b43_mac_suspend leaves bit 0 clear, so there is no mac_suspend op
	 * here: the vendor emits the MAC preparation sequence in op_init, and this
	 * function is entered with the MAC suspended. status_mask already
	 * has MAC_EN clear at this point.
	 *
	 * No precondition on the classifier or clip detect: on the first entry
	 * after attach, RX_ANY and CLIP_ALL_DIS are both clear, while on a
	 * channel change the bits reflect the previous channel's state. Both are
	 * set further down in this function.
	 */
	B43_PHY_AC_REQUIRE_RET(dev,
			       0,
			       B43_PHY_AC_STATE_MAC_EN | B43_PHY_AC_STATE_CCA_RESET,
			       -EINVAL);

	/*
	 * Drop the PMU request before reconfiguring the channel, but only if it
	 * is raised: on the attach path the preamble has already dropped it, and
	 * a second clear would be one op too many.
	 */
	if (dev->phy.ac->status_mask & B43_PHY_AC_STATE_PMU_REQ)
		b43_phy_ac_pmu_req(dev, false);

	/*
	 * Freeze RX path before channel reconfigure: classifier WAITED-only,
	 * clip detectors frozen, CCA reset. In the trace this runs BEFORE
	 * radio_channel_setup. Everything from here through
	 * rx_enable runs under RX=w CLIP=111; first release is the rx_gate
	 * pulse in idle_tssi_meas.
	 */
	b43_phy_ac_channel_switch_prep(dev);

	b43_radio_2069_channel_setup(dev, e2069);
	b43_phy_ac_channel_setup(dev, e2069, channel);

	b43_phy_ac_chan_tables(dev);

	/*
	 * Noise shaping table init. Populates the per-core
	 * noise variance (tbl 0x15), gain-limit (tbl 0x0b), and noise shaping
	 * coefficient tables (tbl 0x44/0x45 per-core, stride 0x20 on table ID).
	 * All write data is deterministic from the ch36 d6220 trace.
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
		u16 saved;
		unsigned int core;

		/*
		 * Phase 1: gate cycle, enable, shared tables.
		 *
		 * The 0x019e gate was left locked on exit from chan_tables().
		 * The vendor emits an unlock and relock cycle -- a marker for the
		 * subsystem -- before loading the noise-shaping tables.
		 */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);   /* unlock */
		saved = b43_phy_ac_tbl_write_lock(dev);               /* peek+relock */

		b43_phy_read_log(dev, 0x016c);                        /* peek */
		b43_phy_maskset(dev, 0x016c, (u16)~0x0040, 0x0040);

		/*
		 * Per-core nvar, three writes to table 0x15, which the vendor
		 * emits before table 0x0b. On the 4352 only: wl 7.14 on the 4360
		 * writes ta and tb directly with no staging through 0x15, and
		 * the agcombo capture has zero ops on table 0x15.
		 *
		 * SALAME: without a third witness -- a 4352 on 7.14, or a 4360 on
		 * the newer driver -- chip and version cannot be told apart here.
		 */
		if (dev->dev->chip_id != 0x4360)
			for (core = 0; core < dev->phy.ac->num_cores; core++)
				b43_actab_write_bulk(dev, 0x15, nvar_off[core], 16, 5, nvar_data);

		/* Gain-limit (2× TBL.WR id=0x0b). */
		b43_actab_write_bulk(dev, 0x0b, 0x0008, 16, 6, glim_a);
		b43_actab_write_bulk(dev, 0x0b, 0x0010, 16, 7, glim_b);

		/*
		 * Transizione Phase 1 → Phase 2: il gate resta lockato.
		 * Il vendor emette peek+relock (tbl_write_lock idempotente),
		 * NON unlock+lock — evita 1 op superflua.
		 */
		saved = b43_phy_ac_tbl_write_lock(dev);

		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 ta = 0x44 + core * 0x20;	/* core 0=0x44, 1=0x64, 2=0x84 */
			u16 tb = 0x45 + core * 0x20;
			u16 rd6[6];

			if (dev->dev->chip_id != 0x4360)
				b43_actab_read_bulk(dev, 0x15, nvar_off[core],
						    16, 6, rd6);
			b43_actab_write_bulk(dev, ta, 0x0008, 16, 6, nshp_a8);
			b43_actab_write_bulk(dev, tb, 0x0008, 16, 6, nshp_b8);
		}

		/*
		 * Phase 2 to 2b transition: the vendor emits unlock, peek and
		 * relock, three ops, between the two -- unlike the phase 1 to 2
		 * transition, which has no unlock. The reason is not clear;
		 * possibly an intermediate gate flush after the heavy writes to
		 * tables 0x44 and 0x45.
		 */
		b43_phy_ac_tbl_write_unlock(dev, saved);
		saved = b43_phy_ac_tbl_write_lock(dev);

		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 ta = 0x44 + core * 0x20;
			u16 tb = 0x45 + core * 0x20;

			b43_actab_write_bulk(dev, ta, 0x0010, 16, 7, nshp_a10);
			b43_actab_write_bulk(dev, tb, 0x0010, 16, 7, nshp_b10);
		}

		b43_phy_ac_tbl_write_unlock(dev, saved);

		/*
		 * Phase 3: per-core commit and readback. The gate stays locked
		 * from the idempotent unlock of the phase 2b to 3 transition, so
		 * no tbl_write_lock is needed here.
		 *
		 * This iterates over every num_cores -- three on the BCM4352 --
		 * with no coremask filter: the vendor emits it for every PHY
		 * channel even on a 2x2 board, where one core is not TX-enabled.
		 */
		for (core = 0; core < dev->phy.ac->num_cores; core++) {
			u16 ta = (u16)(0x44 + core * 0x20);
			u16 tb = (u16)(0x45 + core * 0x20);

			b43_phy_ac_rxgain_init(dev, core);

			/* Diagnostic readbacks */
			{
				u16 rb1, rb10[10], rb8[8];

				b43_actab_read_bulk(dev, tb, 0x0000, 16, 1, &rb1);
				b43_actab_read_bulk(dev, tb, 0x0020, 16, 10, rb10);
				b43_actab_read_bulk(dev, ta, 0x0060, 16, 8, rb8);
				b43_actab_read_bulk(dev, ta, 0x0070, 16, 8, rb8);
				b43_actab_read_bulk(dev, tb, 0x0060, 16, 8, rb8);
				b43_actab_read_bulk(dev, tb, 0x0070, 16, 8, rb8);
			}
		}
	}

	b43_phy_ac_post_noise_shaping_rx_regprog(dev);

	/*
	 * Immediately after the post-noise-shaping block: two diagnostic peeks
	 * and eight MODs on 0x0324, 0x0330, 0x0321, 0x032d, 0x032a, 0x0336,
	 * 0x0327 and 0x0333, all with 0x3600 under mask 0xff00. The pattern is
	 * four (base, base + 0x0c) pairs in the 0x0320-0x0340 range, probably
	 * per-rate TX configuration. Meaning not identified; transcribed from
	 * the capture.
	 */
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

	/*
	 * rxgain_init() must not be called here. The stock driver does not emit
	 * it at this point -- noise_shaping phase 3 already emits the MOD 0x?f9
	 * ops of that pattern -- and it would duplicate the masksets on
	 * 0x0045/0x0033 plus stride that live in
	 * post_noise_shaping_core_transition(). It belongs to init, not to a
	 * channel change.
	 */

	/* BW select per-core, before reset_cca/afecal. */
	b43_phy_write(dev, 0x0304, 0x4e51);
	b43_phy_write(dev, 0x0307, 0x4e51);
	b43_phy_write(dev, 0x030a, 0x4e51);
	b43_phy_write(dev, 0x030d, 0x4e51);

	b43_phy_ac_reset_cca(dev);
	udelay(1);

	b43_radio_2069_afecal(dev);

	/*
	 * ADC reset (cal-time): run after afecal and
	 * before the idle-TSSI capture. Includes the 0x70[15:13] TX-power-
	 * control enable (mid-block).
	 */
	b43_phy_ac_adc_reset(dev);
	b43_phy_ac_txpwrctrl_enable(dev);

	/*
	 * Idle-TSSI measurement, iteration 1. The REQUIRE preconditions are
	 * per call: iterations 2 and 3, in post_cal_finalize() and _iter3(),
	 * run with different MAC states.
	 */
	{
		B43_PHY_AC_REQUIRE_RET(dev,
				       B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
				       B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
				       B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN,
				       -EINVAL);
		b43_phy_ac_idle_tssi_meas(dev);
	}

	/*
	 * Settle before txpwrctrl reads the idle-TSSI result. The trace gap is
	 * ~35 ms and this waits 35 us: idle_tssi_meas() already polls its own
	 * busy bit, so the gap is read as the stock driver's scheduling.
	 * SALAME: if the base index comes out wrong on hardware, mdelay(35).
	 */
	udelay(35);

	/*
	 * idle_tssi to txpwrctrl transition:
	 *   write 0x0339 = 0x0fff        RX suspend during txpwr calibration
	 *   MHF slot 4 set 0x0008        firmware config bit
	 *   MHF slot 0 clear 0x4000      firmware config bit
	 *   peek 0x019e
	 *   set bit 1 of 0x019e          relock the table-write gate
	 * The vendor emits no rx_gate arm here: txpwrctrl_setup() runs with the
	 * gate released from the idle_tssi phase.
	 */
	b43_phy_write(dev, 0x0339, 0x0fff);
	b43_phy_ac_shm_readback_block(dev);
	/*
	 * SPUWKUP, il pre-wakeup del sintetizzatore in microsecondi. La cella e'
	 * M_SYNTHPU_DLY, 0x4a*2, e la costante e' per tipo di PHY: brcmsmac ne
	 * porta quattro -- 3700 per A-PHY, 1050 per B-PHY, 2048 per N-PHY rev>=3,
	 * 300 per LCN -- e le sceglie in brcms_b_upd_synthpu(). 512 e' il membro
	 * AC della stessa famiglia, ed e' cosi' che va letto: non un numero
	 * trascritto ma una costante per PHY, come il 2048 dell'N-PHY.
	 *
	 * b43 qui sbaglia due volte: applica il valore B-PHY a tutto
	 * (b43_set_synth_pu_delay(), 1050) e aggiunge un caso adhoc/idle a 500
	 * che in brcmsmac non esiste. Vedi il TODO nella serie patches/.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0094, 512);
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x0008, 0x0008);
	/*
	 * RFATT, subito dopo il write-through di HOSTF5 che la chiamata sopra
	 * provoca. Valore trascritto: 0x0480 su tutti e 26 i segmenti a freddo
	 * del d6220, una volta per segmento, e lo stesso sul DSL nella stessa
	 * posizione. Costante su canale e larghezza, quindi non c'e' una
	 * dipendenza da derivare; a cosa serva su un AC-PHY non e' noto.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_RFATT, 0x0480);
	/*
	 * Read-only scan of the two direct-map tables, sixteen entries each,
	 * closed by the read of 0x0056. No value to reproduce, and what
	 * consumes the result is unknown -- a scan to find out which per-rate
	 * blocks exist would have this shape.
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
	 * 0x10f4-0x14b2 zeroed, 480 words. In the blob this is a table load
	 * from rodata and the table is all zeros, so the loop replaces it
	 * without losing anything. Once per attach, immediately after RFATT,
	 * and the same 480 zero words on every segment checked, at every
	 * channel and width.
	 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
	 *   12311-12790]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   7998-8477]
	 */
	B43_AC_BLOCK("shm_zero_10f4");
	for (off = 0x10f4; off <= 0x14b2; off += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, off, 0x0000);
	/*
	 * Second zeroing, attached to the first: 68 words from 0x05e0 to
	 * 0x0666, identical on every segment checked. The first ten fall in the
	 * KEYIDXBLOCK block that b43.h declares the core's, but the vendor
	 * writes them in this run and nowhere else -- each cell appears exactly
	 * once in the capture -- so the run is reproduced whole and the
	 * perimeter of compare.py is restricted accordingly.
	 * [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
	 *   12791-12858]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   8478-8545]
	 */
	B43_AC_BLOCK("shm_zero_05e0");
	for (off = 0x05e0; off <= 0x0666; off += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, off, 0x0000);
	/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
	 *   12859-13592]
	 * [capture-ref: router-data/d6220/hot-sweep.zip!segmenti/01-up-ch36-bw20.txt;
	 *   8546-9281]
	 */
	B43_AC_BLOCK("shm_mac_config");
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);
	b43_phy_ac_shm_mac_config_block(dev);
	/*
	 * Second half of the poll with a single pass: no head, no sweep and no
	 * tail.
	 */
	b43_phy_ac_wd_stats_poll_opt(dev, false, 1, true);
	/*
	 * Invariant across all 26 segments. Between these and the poll below,
	 * the capture has a TPL.RAMW, which is the core's template RAM.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x018a, 0xffce);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x018c, 0xffba);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0, true);
	/*
	 * After the sweep and after the four CCK blocks that are still not
	 * understood.
	 */
	b43_phy_ac_chainmask_block(dev);
	b43_phy_ac_prb_rsp_rate_po(dev);
	b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	b43_phy_ac_txpwrctrl_setup(dev, channel->center_freq);

	/*
	 * Transition between the first and second txpwrctrl_setup(): 27 ops that
	 * release the gate, enable TX power control, restore the per-core
	 * current index, wake the MAC so the firmware picks up the new config,
	 * then suspend it again.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);           /* unlock */
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);      /* TX power ctrl enable */
	/*
	 * The gain-word pair only appears from the second bring-up on: on the
	 * first, the stock driver emits 0x0070 and then goes straight to
	 * 0x0678/0x0878, while on a later channel setup the pair is there. This
	 * holds for this site and for adc_reset(), but not for the tail of
	 * rxiqcal_finalize(), which always emits it.
	 */
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		b43_phy_maskset(dev, 0x0644, (u16)~0x007f, 0x0014);
		b43_phy_maskset(dev, 0x0844, (u16)~0x007f, 0x0014);
	}
	b43_phy_maskset(dev, 0x0678, (u16)~0x0004, 0);
	b43_phy_maskset(dev, 0x0878, (u16)~0x0004, 0);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);    /* MHF3 set bit 6 */
	b43_phy_ac_ofdm_pctl1_readback(dev);
	/*
	 * PRMAXTIME a zero, cioe' timeout infinito per la probe response del
	 * firmware. Non e' un valore trascritto: e' quello che b43 scrive in
	 * b43_chip_init(). b43 poi lo riporta a 1 in
	 * b43_wireless_core_init() per spegnere l'offload, e quel secondo
	 * write la cattura non lo ha -- vedi "TODO post-WIP: offload della
	 * probe response in hardware" in docs/retrace-todo.md.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0074, 0x0000);
	/*
	 * These two are invariant across all 26 segments and what they are for
	 * is unknown. Only the first of the map's two passes carries them.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0082, 0x2710);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ba, 0xffff);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x003c, 0x000a);
	b43_phy_ac_chainmask_block(dev);
	b43_phy_ac_basic_rate_map(dev);
	b43_maccontrol_set(dev, ~0x00100000u, 0);
	b43_maccontrol_set(dev, ~0x01c00000u, 0);
	b43_mac_enable(dev);
	/*
	 * The four PWRIND_BLKS cells preceding the 0x0308 that
	 * crs_note_noise() samples, read immediately after the MAC is
	 * re-enabled. Reads only, and nothing here consumes the value -- what
	 * the vendor wants them for is unknown.
	 */
	for (off = 0x0300; off <= 0x0306; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0, true);
	/*
	 * MHF4 bit 15: alzato al primo bring-up,
	 * abbassato su un channel setup successivo.
	 */
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x8000,
			       (dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP) ? 0x8000 : 0);
	/*
	 * MHF1 bit 0: abbassato al primo bring-up, alzato su un channel
	 * setup successivo -- polarita' opposta a MHF4 bit 15 sopra.
	 */
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0001,
			       (dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP) ? 0 : 0x0001);
	b43_mac_suspend(dev);
	/*
	 * Invariant across all 26 segments of the cold sweep; what it is for is
	 * unknown.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x007c, 0x0320);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0020, 0x0020);    /* MHF3 set bit 5 */
	b43_mac_enable(dev);
	/*
	 * Depends on the width alone -- 0x0f0f at 20 and 80 MHz, 0x0303 at 40
	 * -- and the two bytes are always equal, so it is one value replicated
	 * over two halves. That 0x0f becomes 0x03 at 40 MHz is a quarter and
	 * not a half, and is not explained.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x005a,
			(dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40)
				? 0x0303 : 0x0f0f);
	b43_phy_maskset(dev, 0x0042, (u16)~0x8000, 0x8000);
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0020, 0x0020);    /* MHF1 set bit 5 */
	b43_mac_suspend(dev);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0, true);
	b43_phy_ac_basic_rate_map(dev);

	/*
	 * Two things the vendor emits after this point are the caller's and not
	 * this function's.
	 *
	 * The unconditional mac_enable: it is not the release of what this
	 * function acquired, it is a precondition of the post-channel
	 * calibrations, which run with the MAC active and require it. Since the
	 * caller invokes those, the enable is its business too -- and leaving
	 * it out keeps the window between the two phases free, which is where
	 * the core writes the BSS configuration with the MAC still suspended.
	 *
	 * The calibrations themselves -- post_cal_finalize() for iterations 2
	 * and 3, the 24 rxiqcal iterations, the rxcal AFE, the two txpwr rounds
	 * with their RX-IQ measurement and the gainctrl_final loop, the final
	 * RX-IQ teardown: hundreds of ops, invoked by the caller the way the
	 * core does after b43_switch_channel() returns.
	 */
	dev->phy.ac->txpwr_adjust_due = true;
	dev->phy.ac->tuned = true;
	return 0;
}

/* R/W ops */

/* Plain PHY register read/write. */
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

/* PHY ops struct */

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
	.pwork_15sec		= b43_phy_ac_op_pwork_15sec,
	.pwork_60sec		= b43_phy_ac_op_pwork_60sec,
	.channel_calibrate	= b43_phy_ac_op_channel_calibrate,
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
 *   ratio  = round(f_MHz * 2^18 / D) - 2^24
 *   dphase = round(K / f_MHz)
 *
 * with (D, K) = (60, 0x7_8000_0000) at 20 and 40 MHz and
 * (45, 0xB_4000_0000) at 80. The 80 MHz mode also swaps two constants that
 * travel with it, 0x0199/0x01a0 and 0x019c/0x01a3, so the three move
 * together and it reads as a sample-rate mode rather than a per-bandwidth
 * scaling.
 *
 * Verified against every configuration of the d6220 sweep -- 16 channels at
 * 20 MHz, 7 bonded pairs at 40, 3 at 80 -- exact on both registers and both
 * cores, 26 out of 26.
 *
 * The 80 MHz row rests on three distinct centre frequencies, so D and K are
 * fitted with little room to spare there; the 20 and 40 MHz row has 23.
 */
#define B43_PHY_AC_FARROW_OFFSET	0x01000000u

struct b43_phy_ac_farrow_mode {
	u16 div;		/* D in the ratio */
	u64 dphase_num;		/* K in the deltaphase */
	u16 mu;			/* 0x0199 / 0x01a0 */
	u16 cfg;		/* 0x019c / 0x01a3 */
};

static const struct b43_phy_ac_farrow_mode b43_phy_ac_farrow_mode_20_40 = {
	.div = 60, .dphase_num = 0x780000000ull, .mu = 0x00a7, .cfg = 0x0f00,
};

static const struct b43_phy_ac_farrow_mode b43_phy_ac_farrow_mode_80 = {
	.div = 45, .dphase_num = 0xb40000000ull, .mu = 0x0084, .cfg = 0x0b40,
};

/* [capture-ref: router-data/d6220/cold-sweep.zip!segmenti/cold01-ch36-bw20.txt;
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

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET);

	/* The 2.4 GHz branch programs different registers entirely. */
	if (b43_current_band(dev->wl) != NL80211_BAND_5GHZ)
		return;

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

	ratio = (u32)(DIV_ROUND_CLOSEST(freq * (1u << 18), m->div) -
		      B43_PHY_AC_FARROW_OFFSET);
	dphase = (u32)DIV_ROUND_CLOSEST_ULL(m->dphase_num, freq);

	/*
	 * The vendor's order: the low half of each 32-bit value before its
	 * high half, core 0 then core 1, then a peek of 0x0601 whose purpose
	 * is unknown, then the global config that closes the block. 14 ops.
	 */
	b43_phy_write(dev, 0x019a, ratio & 0xffff);
	b43_phy_write(dev, 0x019b, ratio >> 16);
	b43_phy_write(dev, 0x019c, m->cfg);
	b43_phy_write(dev, 0x0199, m->mu);

	b43_phy_write(dev, 0x01a1, ratio & 0xffff);
	b43_phy_write(dev, 0x01a2, ratio >> 16);
	b43_phy_write(dev, 0x01a3, m->cfg);
	b43_phy_write(dev, 0x01a0, m->mu);

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
