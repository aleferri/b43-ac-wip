/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_PHY_AC_H_
#define B43_PHY_AC_H_

#include "phy_common.h"

struct ieee80211_channel;


/* PHY register offsets, relative to PHY MMIO space. */

#define B43_PHY_AC_BBCFG			0x001
#define  B43_PHY_AC_BBCFG_RSTCCA		0x4000	/* Reset CCA */
#define B43_PHY_AC_BANDCTL			0x003	/* Band control */
#define  B43_PHY_AC_BANDCTL_5GHZ		0x0001
#define B43_PHY_AC_TABLE_ID			0x00d
#define B43_PHY_AC_TABLE_OFFSET			0x00e
#define B43_PHY_AC_TABLE_DATA1			0x00f
#define B43_PHY_AC_TABLE_DATA2			0x010
#define B43_PHY_AC_TABLE_DATA3			0x011

/* PHY-table access registers, used in pairs for bulk writes. */
#define B43_PHY_AC_TABLE_DATA_LO		0x00f
#define B43_PHY_AC_TABLE_DATA_HI		0x010
#define B43_PHY_AC_TABLE_DATA_2		0x011
#define B43_PHY_AC_CLASSCTL			0x140	/* Classifier control */
#define  B43_PHY_AC_CLASSCTL_CCKEN		0x0001	/* CCK enable */
#define  B43_PHY_AC_CLASSCTL_OFDMEN		0x0002	/* OFDM enable */
#define  B43_PHY_AC_CLASSCTL_WAITEDEN		0x0004	/* Waited enable */


/* RF Control & RF Sequencer. */
#define B43_PHY_AC_RFCTL1			0x400	/* override save/restore + OR 0x3 */
#define B43_PHY_AC_RF_SEQ_MODE			0x401
#define B43_PHY_AC_RF_SEQ_TRIG			0x402
#define B43_PHY_AC_RF_SEQ_STATUS		0x403

/*
 * Trigger values for RF_SEQ_TRIG (and corresponding bits read from
 * RF_SEQ_STATUS).
 */
#define  B43_PHY_AC_RF_SEQ_RST2RX		0x0020	/* force_rfseq cmd 2 */
/* force_rfseq cmd->bit: 0=0x01 1=0x02 2=0x20(RST2RX) 3=0x04 4=0x08 5=0x10 */
/* Per-channel PHY resampler and bandwidth registers 0x371-0x376. The values
 * come from chan_tuning u16[52..57], the phy_bw[] field, not from the radio's
 * chan_raw6. */
#define B43_PHY_AC_BW1A				0x371
#define B43_PHY_AC_BW2				0x372
#define B43_PHY_AC_BW3				0x373
#define B43_PHY_AC_BW4				0x374
#define B43_PHY_AC_BW5				0x375
#define B43_PHY_AC_BW6				0x376
#define B43_PHY_AC_RFCTL_CMD			0x408

/* Analog Front End (AFE), per RF core. */
#define B43_PHY_AC_AFE_C1			0x725
#define B43_PHY_AC_AFE_C1_OVER			0x739
#define B43_PHY_AC_AFE_C2			0x925	/* by stride from C1 */
#define B43_PHY_AC_AFE_C2_OVER			0x939

#define B43_PHY_AC_C1_CLIP			0x6d4
#define  B43_PHY_AC_C1_CLIP_DIS			0x4000
#define B43_PHY_AC_C2_CLIP			0x8d4
#define  B43_PHY_AC_C2_CLIP_DIS			0x4000
#define B43_PHY_AC_C3_CLIP			0xad4
#define  B43_PHY_AC_C3_CLIP_DIS			0x4000

/* PHY-table-write gate. */
#define B43_PHY_AC_REG_TBL_WRITE_GATE		0x19E
#define  B43_PHY_AC_TBL_WRITE_GATE_LOCK		0x0002
/*
 * Bit 0 of the same register enables the radio tuning sequence; it is not a
 * second lock. The stock driver raises it before the 2069's PLL bank and
 * lowers it after the last write, then reinitialises the register with
 * 0x01c0, 0x0200 and 0x003c.
 */
#define  B43_PHY_AC_TBL_WRITE_GATE_RADIO_TUNE	0x0001
#define  B43_PHY_AC_RF_SEQ_OVERRIDE_GATE	0x0001

/*
 * Software mirror of hardware enable/gate bits that gate correctness of
 * downstream PHY sequences. Every mutation of one of the tracked HW
 * registers must update status_mask in the same code path so the mirror
 * stays in sync. Precondition checks read this field via
 * B43_PHY_AC_REQUIRE() below; a failed check sets STATE_FAULTED sticky
 * and every subsequent tracked entry point bails immediately, keeping
 * the driver from touching HW in an inconsistent state.
 *
 * Bit assignments mirror the raw HW bits tracked by
 * reverse-tools/annotate_enables.py, so the trace annotator and the
 * driver agree on the state model.
 */
#define B43_PHY_AC_STATE_MAC_EN		0x0001	/* MAC.MCTRL bit 0: MAC running */
#define B43_PHY_AC_STATE_RX_CCK		0x0002	/* CLASSCTL bit 0: CCK classifier on */
#define B43_PHY_AC_STATE_RX_OFDM	0x0004	/* CLASSCTL bit 1: OFDM classifier on */
#define B43_PHY_AC_STATE_RX_WAITED	0x0008	/* CLASSCTL bit 2: WAITED classifier on */
#define B43_PHY_AC_STATE_RX_ANY		(B43_PHY_AC_STATE_RX_CCK  | \
					 B43_PHY_AC_STATE_RX_OFDM | \
					 B43_PHY_AC_STATE_RX_WAITED)
#define B43_PHY_AC_STATE_CLIP_C0_DIS	0x0010	/* PHY 0x6d4 bit 0x4000 */
#define B43_PHY_AC_STATE_CLIP_C1_DIS	0x0020	/* PHY 0x8d4 bit 0x4000 */
#define B43_PHY_AC_STATE_CLIP_C2_DIS	0x0040	/* PHY 0xad4 bit 0x4000 */
#define B43_PHY_AC_STATE_CLIP_ALL_DIS	(B43_PHY_AC_STATE_CLIP_C0_DIS | \
					 B43_PHY_AC_STATE_CLIP_C1_DIS | \
					 B43_PHY_AC_STATE_CLIP_C2_DIS)
#define B43_PHY_AC_STATE_PHY_RUN	0x0080	/* BBCFG bit 0x8000: 1=running, 0=quiesced */
#define B43_PHY_AC_STATE_CCA_RESET	0x0100	/* BBCFG bit 0x4000 (RSTCCA active) */
#define B43_PHY_AC_STATE_AFE_ON		0x0200	/* RF front-end armed (enable_afe ON) */
/*
 * First bring-up. b43_phy_init() clears phy->do_full_init between ops->init
 * and set_channel, so from set_channel on that flag is always false. op_init
 * latches it here while it is still valid, for the constants the stock driver
 * selects by phase.
 */
#define B43_PHY_AC_STATE_FIRST_BRINGUP	0x0800
/*
 * The PMU resource request, regctl 0 bit 1, as raised by the driver. The
 * hardware bit cannot be read back through the bcma API, so it is tracked
 * here to tell "needs lowering" from "already low", the way the refcount does
 * for the MAC.
 */
#define B43_PHY_AC_STATE_PMU_REQ	0x0400
#define B43_PHY_AC_STATE_FAULTED	0x8000	/* sticky: a precondition failed */

/* Per-device PHY state. */

/*
 * Hard maximum of RF chains the driver programs. Every per-core array here
 * and in phy_ac.c / rxiqcal_phy_ac.c is sized on this, and num_cores is
 * clamped to it in b43_phy_ac_probe_cores.
 */
#define B43_PHY_AC_MAX_CORES		3

/*
 * Quante passate di misura la finestra tiene: due fino a 40 MHz, sei a 80 sul
 * d6220 e sette su agcombo. Otto copre il caso osservato piu' ampio.
 */
#define B43_PHY_AC_IQ_ROUNDS	8

/*
 * Per-core RX-IQ accumulators. The solve averages the measurement passes; with
 * only one the coefficients do not match, which is how that was established.
 * The reads of 0x?c0-0x?c5 happen inside the measurement, which runs several
 * times, and the values are kept here because the solve lives in a different
 * function.
 */

struct b43_phy_ac_iq_acc {
	/* Finestra scorrevole sulle passate; [0] e' la piu' recente. */
	u32 ii[B43_PHY_AC_IQ_ROUNDS];
	u32 qq[B43_PHY_AC_IQ_ROUNDS];
	s32 iq[B43_PHY_AC_IQ_ROUNDS];
	/* Passate di misura nella finestra; la ricerca in loopback non conta. */
	unsigned int rounds;
	bool measuring;
	/* The solved coefficients, computed once and reapplied: the stock driver
	 * rewrites the same values on the second apply rather than recomputing
	 * them. */
	s16 a;
	s16 b;
	bool solved;
};

#define B43_PHY_AC_NUM_RATES_CCK		4
#define B43_PHY_AC_NUM_RATES_OFDM		8
#define B43_PHY_AC_NUM_RATES_MCS		8

/*
 * Per-rate TX power limits, in quarter-dBm.
 *
 * Same shape and units as struct txpwr_limits in
 * brcm80211/brcmsmac/phy/phy_hal.h, because the computation that fills it is
 * the one brcmsmac already carries as wlc_phy_txpower_recalc_target(). The
 * quarter-dB unit is that driver's BRCMS_TXPWR_DB_FACTOR of 4; the register
 * this eventually feeds, 0x0646, is in the same unit while the SROM and
 * regulatory values are in whole dB.
 *
 * The limits are segmented, and not only between OFDM and legacy: each
 * modulation, bandwidth and stream count has its own row. A single scalar
 * cannot stand in for them, which is why fitting one constant against the
 * captured 0x0646 values could never close.
 *
 * The AC-PHY reduces this to two things:
 *   - the maximum over rates, written per core to 0x0646[7:0];
 *   - the per-rate distance from that maximum, which lands in table 0x21,
 *     the array this driver calls ppr[24].
 * That split explains why ppr is channel-invariant across the sweep while
 * 0x0646 is not: moving channel moves the maximum, not the spacing.
 */
struct b43_phy_ac_txpwr_limits {
	u8 cck[B43_PHY_AC_NUM_RATES_CCK];
	u8 ofdm[B43_PHY_AC_NUM_RATES_OFDM];
	u8 ofdm_cdd[B43_PHY_AC_NUM_RATES_OFDM];
	u8 ofdm_40_siso[B43_PHY_AC_NUM_RATES_OFDM];
	u8 ofdm_40_cdd[B43_PHY_AC_NUM_RATES_OFDM];
	u8 mcs_20_siso[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_20_cdd[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_20_stbc[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_20_mimo[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_40_siso[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_40_cdd[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_40_stbc[B43_PHY_AC_NUM_RATES_MCS];
	u8 mcs_40_mimo[B43_PHY_AC_NUM_RATES_MCS];
};

/*
 * Flat rate index into the limit and target arrays, same segmentation and
 * same numbering as the TXP_* defines in brcmsmac/phy/phy_int.h. Kept
 * identical so the two can be read side by side; only the segments the
 * AC-PHY exercises are filled in at first.
 */
#define B43_PHY_AC_TXP_FIRST_CCK		0
#define B43_PHY_AC_TXP_LAST_CCK			3
#define B43_PHY_AC_TXP_FIRST_OFDM		4
#define B43_PHY_AC_TXP_LAST_OFDM		11
#define B43_PHY_AC_TXP_FIRST_OFDM_20_CDD	12
#define B43_PHY_AC_TXP_LAST_OFDM_20_CDD		19
#define B43_PHY_AC_TXP_FIRST_MCS_20_SISO	20
#define B43_PHY_AC_TXP_LAST_MCS_20_SISO		27
#define B43_PHY_AC_TXP_FIRST_MCS_20_CDD		28
#define B43_PHY_AC_TXP_LAST_MCS_20_CDD		35
#define B43_PHY_AC_TXP_FIRST_MCS_20_STBC	36
#define B43_PHY_AC_TXP_LAST_MCS_20_STBC		43
#define B43_PHY_AC_TXP_FIRST_MCS_20_SDM		44
#define B43_PHY_AC_TXP_LAST_MCS_20_SDM		51
#define B43_PHY_AC_TXP_FIRST_OFDM_40_SISO	52
#define B43_PHY_AC_TXP_LAST_OFDM_40_SISO	59
#define B43_PHY_AC_TXP_FIRST_OFDM_40_CDD	60
#define B43_PHY_AC_TXP_LAST_OFDM_40_CDD		67
#define B43_PHY_AC_TXP_FIRST_MCS_40_SISO	68
#define B43_PHY_AC_TXP_LAST_MCS_40_SISO		75
#define B43_PHY_AC_TXP_FIRST_MCS_40_CDD		76
#define B43_PHY_AC_TXP_LAST_MCS_40_CDD		83
#define B43_PHY_AC_TXP_FIRST_MCS_40_STBC	84
#define B43_PHY_AC_TXP_LAST_MCS_40_STBC		91
#define B43_PHY_AC_TXP_FIRST_MCS_40_SDM		92
#define B43_PHY_AC_TXP_LAST_MCS_40_SDM		99
#define B43_PHY_AC_TXP_NUM_RATES		101

/*
 * Chanspec, as the ucode reads it out of shared memory at 0x00a0. The low
 * byte is the *centre* channel, not the primary, and the high bits carry the
 * width. Verified across all 26 sweep configurations: ch36 gives 0xd024 at
 * 20 MHz, 0xd826 at 40 -- centre 38 of the 36+40 pair -- and 0xe02a at 80,
 * centre 42 of the 36..48 block.
 */
/* Campo di banda del chanspec: e' anche l'argomento che il driver stock passa
 * a b43_mac_bw_set(). 0xd000 & 0x3800 = 0x1000, 0xd800 -> 0x1800,
 * 0xe000 -> 0x2000. */
#define B43_PHY_AC_CHANSPEC_BW_MASK		0x3800
#define B43_PHY_AC_CHANSPEC_BW20		0xd000
#define B43_PHY_AC_CHANSPEC_BW40		0xd800
#define B43_PHY_AC_CHANSPEC_BW80		0xe000
#define B43_SHM_AC_CHANSPEC			0x00a0

void b43_phy_ac_write_chanspec(struct b43_wldev *dev);

/* MAC bandwidth register, written when the operating width changes. */
#define B43_MAC_BW_20				0x1000
#define B43_MAC_BW_40				0x1800
#define B43_MAC_BW_80				0x2000

/* Quarter-dBm conversion, brcmsmac's BRCMS_TXPWR_DB_FACTOR. */
#define B43_PHY_AC_QDB(n)			((n) * 4)

/*
 * Measurement field of PHY 0x0012, the idle-TSSI readback. Bit 11 is set on
 * every sample the captures contain; a sample whose measurement field is zero
 * carries no reading and the average skips it.
 */
#define B43_PHY_AC_IDLE_TSSI_MEAS		0x07ff
/* Bit 11 of the same readback, surviving the shift by two. */
#define B43_PHY_AC_IDLE_TSSI_BASE		0x0200

struct b43_phy_ac {
	/* active RF-chain count (PHY reg 0x0B & 0x07), set at op_init */
	u8 num_cores;
	/* populated-chain bitmask (1 bit per present core), set at op_init */
	u8 coremask;
	/* Analog LPF / DAC-buffer caps; attach defaults, set in op_allocate. */
	u8 lpf_cap0;	/* default 0x80 */
	u8 lpf_cap1;	/* default 0x80 */
	u8 dacbuf_cap;	/* default 0x0c */
	/* Software mirror of tracked HW gate bits; see B43_PHY_AC_STATE_*. */
	u16 status_mask;
	/*
	 * State of the 0x0520[3:2] toggle the probe cycles of
	 * rxiqcal_finalize() use. The stock driver alternates it at every group
	 * of peeks and never restarts it, not even across a channel switch.
	 *
	 * Verified over the 32 warm cycles of the d6220 sweep: the mode each
	 * cycle opens on always follows from the parity of the toggle count of
	 * the cycle before it, 31 transitions out of 31.
	 */
	u16 probe_mode;
	/*
	 * Channel of the last completed calibration, 0 when none has run.
	 *
	 * The first probe group of a calibration is irregular -- see
	 * b43_phy_ac_probe_cycle() -- and what selects it is whether the
	 * channel has changed since the previous calibration, not whether this
	 * is a first bring-up. The d6220 sweep settles it: across its 32 warm
	 * cycles, in which no bring-up is a first one, the irregular group
	 * appears in exactly the 16 cycles that follow a channel change,
	 * 32 out of 32.
	 */
	u16 last_cal_channel;
	/*
	 * Deadline of the probe/measure phase, in 1-second ticks, and the two
	 * ticks the periodic watchdog lands on within it (0xffff for none).
	 * Both stand in for a clock the trace harness does not have; see
	 * b43_phy_ac_rxiqcal_finalize().
	 */
	u16 probe_ticks;
	u16 probe_watchdog_tick[2];
	/*
	 * Count of calibration cycles this session, gating the cold bump in
	 * recalc_txpower()'s crsmin path: the blob bumps the ladder for the
	 * first two calibrations. It appears to saturate at two.
	 */
	u8 cal_cycles;
	/* RX-IQ accumulators gathered by the measurement, consumed by the
	 * solve. */
	struct b43_phy_ac_iq_acc iq_acc[B43_PHY_AC_MAX_CORES];
	/* Salvati da rxcal_radio_setup, riscritti da rxcal_radio_cleanup. */
	u16 rxcal_radio_saved[B43_PHY_AC_MAX_CORES][7];
	/*
	 * Results of the AFE cal's commit iterations, indexed by write offset.
	 * The tail of rxcal_afe_calibrate() duplicates them per antenna.
	 */
	struct {
		u16 off;
		u16 v[2];
		u8 n;
	} afe_res[6];
	/*
	 * Snapshot of afe_res at the end of b43_phy_ac_rxcal_afe_calibrate(),
	 * the cal's first pass. It is needed because the blob rewrites the same
	 * offsets with the second tone's results, applied once and temporarily,
	 * while the final reapplications -- the finalize kick and
	 * rxiqcal_finalize() -- use the first pass's results again. The attach
	 * capture shows exactly that: one write of the second pass, then two of
	 * the first.
	 */
	struct {
		u16 off;
		u16 v[2];
		u8 n;
	} afe_res_cal[6];
	/* pa5ga/maxp5ga sub-band group (0..3) for the current channel, cached
	 * by txpwrctrl_setup so later cal blocks can derive per-core power. */
	u8 pa5g_grp;
	/*
	 * Channel being programmed, cached by set_channel for the cal blocks
	 * that run after it. Not read from dev->phy.channel: b43 only updates
	 * that once ops->switch_channel has returned, so during the
	 * calibrations it still holds the previous channel.
	 */
	u16 cal_channel;
	/*
	 * Centre frequency of the same configuration, for the data that is
	 * keyed on frequency rather than on the channel number.
	 */
	u16 cal_freq;
	/*
	 * CRS minimum-power state: the ladder entry in force and the sub-band it
	 * was measured in. The threshold carries across channel changes within
	 * a sub-band, so it has to outlive a single set_channel.
	 */
	u8 crs_index;
	/* CRS value chanspec_tail() last wrote, reused by the Block E site. */
	u8 crs_written;
	/* Operating width the MAC was last told about, 0 when never. */
	enum nl80211_chan_width mac_width;
	u8 crs_subband;
	/* Operating width of the same configuration. */
	enum nl80211_chan_width cal_width;
	/*
	 * RX-IQ imbalance accumulator readings from the probe sweep in
	 * b43_phy_ac_rxcal_gainctrl(), indexed [core][step_idx][sample]:
	 *   step_idx is the vendor's order of the four {bit1, bit2} combinations
	 *     of radio 0x000e plus stride:
	 *       [0]: bit1 set, bit2 clear
	 *       [1]: both clear -- the baseline, injection off
	 *       [2]: both set
	 *       [3]: bit1 clear, bit2 set
	 *   sample 0 to 7 are the eight consecutive reads of PHY 0x0013, the
	 *     global accumulator, that the vendor uses for settling.
	 *
	 * Intended use: computing the I/Q compensation coefficients; the formula
	 * is still open, see the TODO in rxcal_gainctrl(). Not used for the
	 * op-for-op match, which only looks at the ops emitted. On real hardware
	 * these are the values the cal finds, to be consumed by the next phase,
	 * the RX-IQ compensation write.
	 */
	u16 rxcal_imbalance[B43_PHY_AC_MAX_CORES][4][8];
	/*
	 * Shadow of the five HOSTFn shared-memory words, and whether a change
	 * to it is written through to the cell.
	 *
	 * The stock driver keeps the same shadow -- its brcmsmac equivalent is
	 * brcms_b_mhf(), which writes the cell only under
	 *
	 *   wlc_hw->clk && band->mhfs[idx] != save && band == wlc_hw->band
	 *
	 * so a call that leaves the word unchanged emits nothing. The captures
	 * bear that out: not one read of 0x005e, 0x0060, 0x0062, 0x0078 or
	 * 0x00d4 in either witness, and the cell written on 5 calls out of 38
	 * on the d6220 and 4 out of 56 on the DSL, in both cases exactly the
	 * calls that change the word.
	 *
	 * @mhf_writethrough stands for the clk term. It is not b43's clock
	 * state, which is already up here: it is the point the captures put
	 * the transition at, between the slot 4 and slot 0 writes of the
	 * frontend GPIO block. The band term does not appear because every
	 * call in both captures is on the operating band.
	 */
	u16 mhfs[5];
	bool mhf_writethrough;
	/*
	 * Puntatori dei blocchi per-rate degli otto rate OFDM, presi durante la
	 * scansione delle direct-map in set_channel(). Il vendor non li rilegge
	 * dove costruisce la mappa dei basic rate -- quel blocco e' di sole
	 * scritture -- quindi li tiene in cache e qui si fa lo stesso.
	 */
	u16 rate_ptr[8];
};

/*
 * Precondition check. `want` bits MUST be set, `forbid` bits MUST be
 * clear. On mismatch: log an error, set FAULTED, and return. Once
 * FAULTED is set every subsequent REQUIRE returns immediately, so a
 * single broken invariant does not cascade into HW damage.
 *
 * `dev` is evaluated more than once (must be a plain lvalue).
 * `want` and `forbid` must be disjoint or the check is unsatisfiable;
 * a zero `want`/`forbid` means "no requirement on that side".
 *
 * REQUIRE()	   -> use in functions returning void
 * REQUIRE_RET()   -> use in functions returning a value
 *
 * NOTE on STATE_PHY_RUN: mirrors BBCFG[15], kept only for parity with the
 * annotator (annotate_enables.py tracks the same bit). No driver code sets or
 * clears it, by design: the vendor never writes BBCFG[15] in the captured
 * flows (attach-ch36 and down-to-bss-up show only RSTCCA / bit 0x4000 pulses
 * on BBCFG), so the baseband run-state is established at chip power-on and is
 * never toggled in the sequences the driver reproduces. Do not use
 * STATE_PHY_RUN in `want`/`forbid`: it has no mutator and reflects nothing
 * observable in these flows.
 */
#define B43_PHY_AC_REQUIRE(dev, want, forbid) do {			\
	struct b43_phy_ac *__ac = (dev)->phy.ac;			\
	u16 __sm = __ac->status_mask;					\
	if (__sm & B43_PHY_AC_STATE_FAULTED)				\
		return;							\
	if ((__sm & (want)) != (want) || (__sm & (forbid))) {		\
		b43err((dev)->wl,					\
		       "phy_ac: %s precondition failed: "		\
		       "status=0x%04x want=0x%04x forbid=0x%04x\n",	\
		       __func__, __sm, (u16)(want), (u16)(forbid));	\
		__ac->status_mask |= B43_PHY_AC_STATE_FAULTED;		\
		return;							\
	}								\
} while (0)

#define B43_PHY_AC_REQUIRE_RET(dev, want, forbid, ret) do {		\
	struct b43_phy_ac *__ac = (dev)->phy.ac;			\
	u16 __sm = __ac->status_mask;					\
	if (__sm & B43_PHY_AC_STATE_FAULTED)				\
		return (ret);						\
	if ((__sm & (want)) != (want) || (__sm & (forbid))) {		\
		b43err((dev)->wl,					\
		       "phy_ac: %s precondition failed: "		\
		       "status=0x%04x want=0x%04x forbid=0x%04x\n",	\
		       __func__, __sm, (u16)(want), (u16)(forbid));	\
		__ac->status_mask |= B43_PHY_AC_STATE_FAULTED;		\
		return (ret);						\
	}								\
} while (0)

extern const struct b43_phy_operations b43_phyops_ac;


bool b43_phy_ac_force_rf_sequence(struct b43_wldev *dev, u16 rf_seq, u16 gate);
u16  b43_phy_ac_classifier(struct b43_wldev *dev, u16 mask, u16 val);

/*
 * The wl-diag trace records the address of every register read but not the
 * value returned (the tracer only captures input args). So each read below is
 * a hole in the trace. Route value-consuming reads through these to capture
 * the real value on hardware; the log is compiled out when B43_DEBUG is 0
 * (dead branch elided), so release builds pay nothing. `dev` must be a plain
 * lvalue -- it is evaluated more than once.
 */
#define b43_phy_read_log(dev, reg) ({					\
	u16 __r = (reg), __v = b43_phy_read((dev), __r);		\
	if (B43_DEBUG)							\
		b43dbg((dev)->wl, "phy   rd 0x%04x = 0x%04x\n",		\
		       __r, __v);					\
	__v;								\
})
#define b43_radio_read_log(dev, reg) ({					\
	u16 __r = (reg), __v = b43_radio_read((dev), __r);		\
	if (B43_DEBUG)							\
		b43dbg((dev)->wl, "radio rd 0x%04x = 0x%04x\n",		\
		       __r, __v);					\
	__v;								\
})

void b43_phy_ac_reset_cca(struct b43_wldev *dev);

/*
 * Post-channel-setup calibrations, in the order
 * b43_phy_ac_set_channel_calibrations() calls them; that function documents
 * the rounds. Each of these is a phase transcribed from a capture, with its
 * internal structure, op counts and still-transcribed values documented next
 * to the code. Phase-to-op-range map: docs/vendor-op-map.md.
 */
void b43_phy_ac_post_cal_finalize(struct b43_wldev *dev);
void b43_phy_ac_post_cal_finalize_iter3(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply(struct b43_wldev *dev);
void b43_phy_ac_post_rxiqcal_stage2(struct b43_wldev *dev);
void b43_phy_ac_rxcal_afe_calibrate(struct b43_wldev *dev);
void b43_phy_ac_rxcal_afe_finalize_gain_luts(struct b43_wldev *dev);

/* Open-loop TX power; INDEX_DEFAULT is the capture's fixed value. */
#define B43_PHY_AC_TXPWR_INDEX_DEFAULT	0x40
void b43_phy_ac_txpwr_by_index(struct b43_wldev *dev, u8 idx);

void b43_phy_ac_rxgain_defaults_pulse(struct b43_wldev *dev);
void b43_phy_ac_radio_chain_range_setup(struct b43_wldev *dev, bool with_tune);
void b43_phy_ac_rxgain_perchan_config(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_dds_seed(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_prep_second_iter(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_run_meas_iters(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(struct b43_wldev *dev);
void b43_phy_ac_iqcal_coeff_tables_reset(struct b43_wldev *dev);
void b43_phy_ac_iqcal_apply_second_stage(struct b43_wldev *dev);
void b43_phy_ac_rxgain_config_readback(struct b43_wldev *dev);
void b43_phy_ac_rxgain_config_apply(struct b43_wldev *dev);
void b43_phy_ac_radio_iqcal_config(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_dds_seed_tone(struct b43_wldev *dev, int step);
void b43_phy_ac_iqcal_meas_post_dds_apply(struct b43_wldev *dev);
void b43_phy_ac_iqcal_meas_post_dds_apply_v2(struct b43_wldev *dev);
void b43_phy_ac_rxiq_apply_coefficients(struct b43_wldev *dev);
void b43_phy_ac_radio_iqcal_teardown(struct b43_wldev *dev);
void b43_phy_ac_rxiq_teardown_apply_defaults(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev);
/*
 * L'enable del MAC e le calibrazioni post-canale le invoca il chiamante di
 * op_switch_channel, come in b43 fa il core dopo b43_switch_channel(): tenerle
 * in coda alla callback impedirebbe di inserire fra le due fasi del setup la
 * configurazione BSS che il core scrive.
 */
void b43_phy_ac_channel_setup_tail2(struct b43_wldev *dev);
void b43_phy_ac_channel_setup_tail(struct b43_wldev *dev,
				   struct ieee80211_channel *channel);
void b43_phy_ac_prb_rsp_plcp_pass(struct b43_wldev *dev);
void b43_phy_ac_set_channel_calibrations(struct b43_wldev *dev);

/*
 * One AFE cal iteration: arm a command on 0x0380, wait on the busy bit, read
 * the result back and rewrite it at wr_off. Used by both iteration groups.
 * core_off is core * 0x200.
 */
void b43_phy_ac_rxcal_afe_iter(struct b43_wldev *dev,
			       u16 cmd, u16 core_off,
			       const u16 *pre_clear_offs, u8 n_pre_clear,
			       u16 rd_off, u8 rw_len, u16 wr_off);

/*
 * One round of the loopback gain search. core_mask selects the round's cores
 * and r734_vals[] is indexed by core number. The convergence criterion and the
 * caller are in phy_ac.c.
 */
void b43_phy_ac_gainctrl_final_apply(struct b43_wldev *dev,
				     bool with_peek_preamble,
				     u8 core_mask,
				     const u16 r734_vals[3]);

/*
 * Steady-state periodic watchdog: poll TSSI and the SHM statistics every tick,
 * with a single pass of the measure block, noise_cal true, on the longer
 * cadence. The op sequence follows the vendor's roughly 5 s tick from the
 * d6220 sweep; in this driver it hangs off pwork_15sec. See the comment in
 * phy_ac.c.
 */
void b43_phy_ac_watchdog(struct b43_wldev *dev, bool noise_cal);

/* Helper trasversali al confine MAC/PHY; razionale in helpers_phy_ac.c. */
void b43_phy_ac_mhf_maskset(struct b43_wldev *dev, u16 slot, u16 mask, u16 val);
void b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set);

/*
 * Function-boundary markers for the userspace test harness. B43_AC_FN() at the
 * top of a function makes the harness bracket the ops that follow with the
 * function name, so fn_map.py can segment the generated trace by exact
 * boundaries instead of guessing fingerprints from source. The exit
 * marker is emitted automatically on scope exit (any return) via GCC's
 * cleanup attribute, so nested calls nest correctly. No-op in the kernel
 * build; the harness defines B43_AC_FN_TRACE and provides the hooks, which
 * emit only when enabled at runtime, so the compare.py trace stays clean.
 */
#ifdef B43_AC_FN_TRACE
void b43_ac_fn_enter(const char *fn);
void b43_ac_fn_leave(const char *fn);
static inline void b43_ac_fn_cleanup(const char *const *fn) { b43_ac_fn_leave(*fn); }
#define B43_AC_FN() \
	const char *const __b43_fn __attribute__((cleanup(b43_ac_fn_cleanup))) = __func__; \
	b43_ac_fn_enter(__func__)
#else
#define B43_AC_FN() do { } while (0)
#endif

#endif /* B43_PHY_AC_H_ */
