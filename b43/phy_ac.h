/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_PHY_AC_H_
#define B43_PHY_AC_H_

#include "phy_common.h"
#include "ppr_ac.h"

struct ieee80211_channel;


/* PHY register offsets, relative to PHY MMIO space. */

#define B43_PHY_AC_BBCFG			0x001
#define  B43_PHY_AC_BBCFG_RSTCCA		0x4000	/* Reset CCA */
#define B43_PHY_AC_BANDCTL			0x003	/* Band control */
#define  B43_PHY_AC_BANDCTL_5GHZ		0x0001
#define B43_PHY_AC_TABLE_ID			0x00d
#define B43_PHY_AC_TABLE_OFFSET			0x00e
/*
 * The three data registers. LO and HI are written as a pair for a 32-bit
 * cell; DATA_2 is the alternate port tables 0x11, 0x14 and 0x20 go through.
 */
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
#define  B43_PHY_AC_RF_SEQ_RX2TX		0x0001	/* force_rfseq cmd 0 */
#define  B43_PHY_AC_RF_SEQ_TX2RX		0x0002	/* force_rfseq cmd 1 */
#define  B43_PHY_AC_RF_SEQ_RST2RX		0x0020	/* force_rfseq cmd 2 */
/* force_rfseq cmd->bit: 0=0x01 1=0x02 2=0x20(RST2RX) 3=0x04 4=0x08 5=0x10 */

/*
 * Status reads before giving up, 10 us apart: the vendor's limits for a
 * forced RF sequence and for a sample play.
 */
#define B43_PHY_AC_RF_SEQ_FORCE_TURNS		20001
#define B43_PHY_AC_RUN_SAMPLES_TURNS		101

/*
 * PHY-side sample-play (tone) engine. The CORDIC tone / IQ buffer lives in
 * table 0x000e; this block mirrors the N-PHY runsamples control registers
 * (brcmsmac 0xc3-0xc6) by behaviour, not by address -- same fields in the same
 * order below NSAMP. NSAMP is verified: it tracks the tone-table length across
 * the three bandwidths (0x27/0x4f/0x9f = num_samps-1), see
 * b43_phy_ac_rxiqcal_kick_len().
 */
#define B43_PHY_AC_SAMP_PLAY_CTL		0x460	/* start (bit0) / stop (bit1) */
#define  B43_PHY_AC_SAMP_PLAY_START		0x0001
#define  B43_PHY_AC_SAMP_PLAY_STOP		0x0002
/*
 * LOOPS and WAIT are named after the N-PHY block order, LOOPS = 0xffff as
 * its "continuous" convention; the WAIT value (0x3c) is not cross-checked.
 */
#define B43_PHY_AC_SAMP_PLAY_LOOPS		0x461
#define B43_PHY_AC_SAMP_PLAY_WAIT		0x462
#define B43_PHY_AC_SAMP_PLAY_NSAMP		0x463	/* num_samps - 1 */

/*
 * Per-channel PHY resampler and bandwidth registers 0x371-0x376, from
 * chan_tuning u16[52..57] (the phy_bw[] field), not from the radio's
 * chan_raw6.
 */
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

/*
 * Not used by name: b43_phy_ac_clip_det() writes 0x06d4 + core * 0x200 inline
 * so the correlator resolves the stride, see the comment there.
 */
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
 * Bit 0 of the same register is not a second lock, and the two names below are
 * the same bit. The stock driver raises it around the 2069's PLL bank, then
 * reinitialises the register with 0x01c0, 0x0200 and 0x003c, and also around
 * each RF-sequencer command in b43_phy_ac_force_rf_sequence(). Whether those
 * are one function or two is not established; only the second name is used.
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
#define B43_PHY_AC_STATE_AFE_OFF		0x0200	/* RF front-end powered down (enable_afe OFF); 0x1720 bit 9 */
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
/*
 * The bring-up part of the cold preamble -- the front-end GPIO block and the
 * host flags up to the PMU release -- has been emitted for this core init.
 * b43 calls switch_analog() from four sites and the vendor emits the preamble
 * once; see b43_phy_ac_cold_preamble_due().
 */
#define B43_PHY_AC_STATE_COLD_PREAMBLE	0x1000
#define B43_PHY_AC_STATE_FAULTED	0x8000	/* sticky: a precondition failed */

/* Per-device PHY state. */

/*
 * Hard maximum of RF chains the driver programs. Every per-core array here
 * and in phy_ac.c is sized on this, and num_cores is
 * clamped to it in b43_phy_ac_probe_cores.
 */
#define B43_PHY_AC_MAX_CORES		3

/*
 * Measurement passes the window keeps: two up to 40 MHz, six at 80 on the
 * d6220, seven on agcombo.
 */
#define B43_PHY_AC_IQ_ROUNDS	8

/*
 * Per-core RX-IQ accumulators. The reads of 0x?c0-0x?c5 happen inside the
 * measurement, which runs several times, and the solve, in another function,
 * averages the passes.
 */

struct b43_phy_ac_iq_acc {
	/* Sliding window over the passes; [0] is the newest. */
	u32 ii[B43_PHY_AC_IQ_ROUNDS];
	u32 qq[B43_PHY_AC_IQ_ROUNDS];
	s32 iq[B43_PHY_AC_IQ_ROUNDS];
	/* Measurement passes in the window, loopback search excluded. */
	unsigned int rounds;
	bool measuring;
	/*
	 * The solved coefficients, computed once per measurement and
	 * reapplied: the stock driver writes the same values again on the
	 * second apply. A new measurement window invalidates them.
	 */
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
 * Same shape and units as struct txpwr_limits in brcmsmac/phy/phy_hal.h,
 * since it is filled by the same computation. The register this feeds, 0x0646, is in
 * the same unit; the SROM and regulatory values are in whole dB.
 *
 * Each modulation, bandwidth and stream count has its own row, so no single
 * constant fits the captured 0x0646 values. The AC-PHY reduces this to the
 * maximum over rates, written per core to 0x0646[7:0], and the per-rate
 * distance from it, in table 0x21 (ppr[24]); hence ppr is channel-invariant
 * while 0x0646 is not.
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
 * Flat rate index into the limit and target arrays, with the segmentation
 * and numbering of the TXP_* defines in brcmsmac/phy/phy_int.h, so the two
 * can be read side by side.
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
 * Chanspec, as the AC microcode reads it out of shared memory at
 * B43_SHM_SH_CHAN: the *centre* channel in the low byte, the position of
 * the primary 20 MHz channel inside the block in bits 8-10, the width in
 * bits 11-13 and the band in bits 14-15, the same with wl 6.30 and 7.14.
 * ch36 gives 0xd024 at 20 MHz, 0xd826 at 40 -- centre 38 of the 36+40 pair
 * -- and 0xe02a at 80, centre 42 of the 36..48 block; ch157 at 80 is 0xe29b,
 * the third channel of the 149..161 block; ch1 at 20 is 0x1001.
 *
 * The width field is also the argument the stock driver passes to
 * b43_mac_bw_set().
 */
#define B43_PHY_AC_CHANSPEC_SB_SHIFT		8
#define B43_PHY_AC_CHANSPEC_BW_MASK		0x3800
#define B43_PHY_AC_CHANSPEC_BW20		0x1000
#define B43_PHY_AC_CHANSPEC_BW40		0x1800
#define B43_PHY_AC_CHANSPEC_BW80		0x2000
#define B43_PHY_AC_CHANSPEC_BAND_5G		0xc000

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
	/* physical core count (PHY reg 0x0b & 0x07), set by probe_cores */
	u8 num_cores;
	/*
	 * Wired-chain bitmask (SROM rxchain), set by probe_cores; unsigned long
	 * so that for_each_set_bit() walks it directly.
	 */
	unsigned long coremask;
	/* LPF and DAC-buffer caps, measured by b43_radio_2069_rccal(). */
	u8 lpf_cap0;
	u8 lpf_cap1;
	u8 dacbuf_cap;
	/* Software mirror of tracked HW gate bits; see B43_PHY_AC_STATE_*. */
	u16 status_mask;
	/*
	 * State of the 0x0520[3:2] walk of the probe cycles. The stock driver
	 * steps it at every group of peeks and never restarts it, not even
	 * across a channel switch (31 of 31 transitions over the d6220's 32
	 * warm cycles).
	 */
	u16 probe_mode;
	/*
	 * Channel of the last completed calibration, 0 when none has run. The
	 * irregular first probe group follows a channel change, not a first
	 * bring-up: across the d6220's 32 warm cycles it appears in exactly the
	 * 16 that follow a channel change.
	 */
	u16 last_cal_channel;
	/*
	 * Whether a channel availability check is outstanding on the current
	 * channel; the calibrations that transmit wait for it, see
	 * b43_phy_ac_may_calibrate_tx().
	 *
	 * mac80211 runs the check, and the core keeps its state in
	 * dev->cac_pending: set by the config call that tunes the channel with
	 * hw->conf.radar_enabled, cleared when beaconing starts.
	 * op_switch_channel() copies it, and the core's cac_done hook,
	 * b43_phy_ac_bss_up(), clears it.
	 */
	bool cac_pending;
	/*
	 * Watchdog turns since the bring-up. The tempsense and the region dump
	 * fall on this count, which a channel change does not reset: it is the
	 * driver's period, not the channel's, so on hot cycles the phase starts
	 * anywhere in it. @wd_switch_turns counts from the channel change.
	 */
	u16 wd_turns;
	u16 wd_switch_turns;
	/*
	 * CRS block E is due with the next noise sample: the bring-up tail sets
	 * this, b43_phy_ac_noise_sample_done() emits the block and clears it.
	 */
	bool crs_update_pending;
	/*
	 * A sample is in flight, armed and not consumed yet; no turn arms
	 * another meanwhile.
	 */
	bool noise_pending;
	/* A radar pulse arrived since the last b43_phy_ac_radar_poll(). */
	bool radar_pulses;
	/* One more turn without a peek, after the entry turn. */
	bool peek_skip_one;
	/*
	 * The first turn after the bring-up has the full shape (the four
	 * scattered cells in entry order, the tone peek, the mode change)
	 * instead of the sweep alone.
	 *
	 * Whether it does depends on when the first callback arrives, not on
	 * the driver: over the 87 segments of the two sweeps the full shape
	 * goes with a first callback within a millisecond of the bring-up tail
	 * (44 of 44 hot `up` segments, and cold01 only among the cold ones),
	 * while a callback 1.0-1.3 s later is the sweep alone. The caller sets
	 * it; reverse-tools/timeline.py reads it from the order of the four
	 * cells.
	 */
	bool wd_entry_turn;
	/*
	 * Calibration cycles this session, gating the cold bump of the crsmin
	 * path: the blob bumps the ladder for the first two calibrations.
	 */
	u8 cal_cycles;
	/*
	 * Low byte of the CRS min-power threshold as last written by one of the
	 * three sites, so that the periodic hook does not rewrite it unchanged;
	 * see b43_phy_ac_op_pwork_60sec(). Zero means never written: the ladder
	 * starts at 41.
	 */
	u16 crs_low;
	/* RX-IQ accumulators of the measurement, consumed by the solve. */
	struct b43_phy_ac_iq_acc iq_acc[B43_PHY_AC_MAX_CORES];
	/*
	 * Saved by tempsense_radio_setup(), restored by
	 * tempsense_radio_restore().
	 */
	u16 tempsense_radio_saved[B43_PHY_AC_MAX_CORES][7];
	/*
	 * TX baseband multiplier, IQLOCAL 0x63 + 4*core mirrored at 0x73 +
	 * 4*core. bbmult_cal[core] is the GAINCTRLBBMULT entry (table 0x20)
	 * read for the TX cal, bbmult_meas the entry at index 0 read for the
	 * RX-IQ measurement, bbmult_saved[core] the cell as read before a cal
	 * and written back after.
	 */
	u16 bbmult_cal[B43_PHY_AC_MAX_CORES];
	/*
	 * The rest of the same GAINCTRLBBMULT entry: the three TX gain code
	 * cells the cal writes into table 0x0007 at 0x100 + core + 3 * i.
	 */
	u16 gaincurve_coeff[B43_PHY_AC_MAX_CORES][3];
	u16 bbmult_meas;
	u16 bbmult_saved[B43_PHY_AC_MAX_CORES];
	/*
	 * The fourteen RX gain-control registers of each core that
	 * rx_gain_regs_program() reads before driving them, in
	 * b43_phy_ac_rxgain_regs[] order, written back by b43_phy_ac_tempsense().
	 */
	u16 rxgain_saved[B43_PHY_AC_MAX_CORES][14];
	/*
	 * Cal state saved in block D of rxiqcal_finalize() and written back by
	 * b43_phy_ac_down() at `wl down`: the LO DAC read-back of radio
	 * 0x?002-0x?005, and the TX IQ/LO coefficients of IQLOCAL (table
	 * 0x000c) per core at 0x60 + 4*core, {a, b} at +0/+1 and the LO leakage
	 * word at +2.
	 *
	 * @iqlo_saved says whether block D has run; the write-back depends on
	 * it, since before the first save the arrays hold zeroes. It is not
	 * b43_phy_ac_may_calibrate_tx(): above 5250 MHz a cold segment has
	 * neither the save nor the write-back, while a hot one has both with
	 * the calibration skipped (hot 09, ch52, writes radio 0x0002-0x0005
	 * where cold05 writes nothing). A cold sweep alone cannot tell the two
	 * apart.
	 *
	 * Then the RX gain configuration of each core across the RX-IQ cal: the
	 * 25 registers of b43_phy_ac_rxgain_cfg_regs[] plus 0x073e, and the
	 * three RFSEQ gain rows 0x0100/0x0103/0x0106 + core, read by
	 * rxgain_config_readback() and written back by
	 * rxiqcal_teardown_apply_defaults().
	 */
	u16 lo_dac[B43_PHY_AC_MAX_CORES][4];
	u16 txiqlo_coef[B43_PHY_AC_MAX_CORES][3];
	bool iqlo_saved;
	u16 rxgain_cfg_saved[B43_PHY_AC_MAX_CORES][26];
	u16 rfseq_gain_saved[B43_PHY_AC_MAX_CORES][3];
	/*
	 * Radio state saved across the RX-IQ cal: the chain range registers,
	 * saved by rxiqcal_apply() and by the full radio_chain_range_setup()
	 * and restored by the next radio_chain_range_setup(); the IQ-cal
	 * registers, saved by radio_iqcal_config() and restored by
	 * radio_iqcal_teardown().
	 */
	u16 chain_range_saved[B43_PHY_AC_MAX_CORES][7];
	u16 iqcal_radio_saved[B43_PHY_AC_MAX_CORES][6];
	/*
	 * The table gate and 0x040f as the RX-IQ cal steps find them, saved
	 * by rxiqcal_apply() and rxgain_perchan_config() and written back by
	 * the rxgain_defaults_pulse() after each; 0x040f alone saved by
	 * rxgain_config_readback() and written back by rxiqcal_finalize().
	 */
	u16 rxcal_gate_saved;
	u16 rxcal_040f_saved;
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
	 * the cal's first pass: the blob rewrites the same offsets with the
	 * second tone's results, applied once, while the final reapplications
	 * (the finalize kick and rxiqcal_finalize()) use the first pass's
	 * results again, as the attach capture shows.
	 */
	struct {
		u16 off;
		u16 v[2];
		u8 n;
	} afe_res_cal[6];
	/*
	 * pa5ga/maxp5ga sub-band group (0..3) of the current channel, cached by
	 * txpwrctrl_setup for the later cal blocks.
	 */
	u8 pa5g_grp;
	/*
	 * Channel being programmed, cached by set_channel for the cal blocks
	 * that run after it: b43 updates dev->phy.channel only once
	 * ops->switch_channel has returned.
	 */
	u16 cal_channel;
	/*
	 * Centre frequency of the same configuration, for the data that is
	 * keyed on frequency rather than on the channel number.
	 */
	u16 cal_freq;
	/*
	 * CRS minimum-power state: per chain, the ladder index of the last four
	 * noise samples, and the sub-band they were taken in. The rings carry
	 * across channel changes within a sub-band, so they have to outlive a
	 * single set_channel. See b43_phy_ac_crs_note_noise().
	 */
	u8 crs_ring[B43_PHY_AC_MAX_CORES][4];
	u8 crs_ring_head;
	u8 crs_ring_len;
	/*
	 * Per chain, the ladder index the hardware carries from the last CRS
	 * write, common threshold plus bank. See b43_phy_ac_crs_moved().
	 */
	u8 crs_prog[B43_PHY_AC_MAX_CORES];
	/* CRS value chanspec_tail() last wrote, reused by the Block E site. */
	u8 crs_written;
	/* Operating width the MAC was last told about, 0 when never. */
	enum nl80211_chan_width mac_width;
	/* Chanspec last written to B43_SHM_SH_CHAN, 0 when never. */
	u16 chanspec;
	u8 crs_subband;
	/* Operating width of the same configuration. */
	enum nl80211_chan_width cal_width;

	/*
	 * TX power target, the output of b43_phy_ac_txpwr_recalc(): the
	 * per-rate table after SROM, regulatory ceiling and margin, its
	 * maximum per core, and the inputs it was computed from, so the
	 * periodic hook can tell a change from a repeat.
	 */
	struct b43_ppr_ac txpwr_ppr;
	struct b43_ppr_ac txpwr_spacing;	/* distances, never saturated */
	u8 txpwr_max[B43_PHY_AC_MAX_CORES];
	/* maxp5ga of the lowest core, 0 when the SROM declares no power. */
	u8 txpwr_maxp;
	/* Limit on the legacy OFDM rates before the margin, 0 when none. */
	u16 txpwr_calc_chan;
	enum nl80211_chan_width txpwr_calc_width;
	u16 txpwr_calc_ceiling;
	/*
	 * A channel switch leaves the hardware power control behind even when
	 * the target did not move: the stock driver runs the whole txpwrctrl
	 * setup again after the core's BSS configuration. Set by
	 * op_switch_channel(), consumed by adjust_txpower().
	 */
	bool txpwr_adjust_due;
	/*
	 * The radio is tuned to the current configuration (op_switch_channel()
	 * ran to completion); cleared by op_init(). b43 asks for the same
	 * channel twice on the way up, from b43_phy_init() and from
	 * b43_op_config(), and the stock driver tunes once.
	 */
	bool tuned;
	/*
	 * The last temperature reading of each chain, from
	 * b43_phy_ac_tempsense_chain(), as [core][step][sample]: the four
	 * {bit1, bit2} configurations of radio 0x?00e, (1,0), (0,0), (1,1),
	 * (0,1), and the eight reads of PHY 0x0013 each. Not consumed yet: the
	 * conversion to degrees needs two calibration points from the stock
	 * driver's phy_tempsense.
	 */
	u16 tempsense_samples[B43_PHY_AC_MAX_CORES][4][8];
	/*
	 * Shadow of the five HOSTFn shared-memory words, and whether a change
	 * is written through to the cell.
	 *
	 * The vendor keeps the same shadow and writes the cell only when the
	 * word changes, with the clock up and on the current band, so an
	 * unchanged word emits nothing. The captures agree: no read of
	 * the five cells, and the cell written on exactly the calls that change
	 * the word (5 of 38 on the d6220, 4 of 56 on the DSL).
	 *
	 * @mhf_writethrough stands for the clk term: not b43's clock state, but
	 * the point the captures put the transition at, between the slot 4 and
	 * slot 0 writes of the front-end GPIO block. Every call is on the
	 * operating band, so the band term does not appear.
	 */
	u16 mhfs[5];
	bool mhf_writethrough;
	/*
	 * The attach part of the cold preamble has run (the AFE arm with the
	 * 0x02e4 field, the PMU request, three host flags). Once per probe, at
	 * the first switch_analog(dev, true) after op_allocate(), the attach
	 * reset.
	 */
	bool attach_preamble_done;
	/*
	 * Block pointers of the eight OFDM rates, from the direct-map scan in
	 * op_switch_channel(). The stock driver does not read them again where
	 * it builds the basic rate map, which is write-only, so they are
	 * cached.
	 */
	u16 rate_ptr[8];
};

/*
 * Precondition check. `want` bits must be set, `forbid` bits clear. On
 * mismatch: log an error, set FAULTED and return. Once FAULTED is set every
 * later REQUIRE returns immediately, so a broken invariant does not cascade
 * into the hardware.
 *
 * `dev` is evaluated more than once. A zero `want`/`forbid` means no
 * requirement on that side.
 *
 * REQUIRE() is for functions returning void, REQUIRE_RET() for the others.
 *
 * STATE_PHY_RUN has no mutator and must not appear in `want` or `forbid`:
 * it mirrors BBCFG[15] for parity with annotate_enables.py, and no captured
 * op touches that bit.
 *
 * STATE_MAC_EN is not stored: it is the core's dev->mac_suspended counter,
 * read at check time, since b43_mac_suspend()/b43_mac_enable() are the
 * core's.
 */
#define b43_phy_ac_status(dev)						\
	((dev)->phy.ac->status_mask |					\
	 ((dev)->mac_suspended == 0 ? B43_PHY_AC_STATE_MAC_EN : 0))

#define B43_PHY_AC_REQUIRE(dev, want, forbid) do {			\
	struct b43_phy_ac *__ac = (dev)->phy.ac;			\
	u16 __sm = b43_phy_ac_status(dev);				\
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
	u16 __sm = b43_phy_ac_status(dev);				\
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


bool b43_phy_ac_force_rf_sequence(struct b43_wldev *dev, u16 rf_seq);

/*
 * A read whose value nobody uses, a potential logic error: the log puts
 * address and value where they can be compared with the captured ones.
 * Compiled out when B43_DEBUG is 0. `dev` is evaluated more than once.
 */
#define b43_phy_read_log(dev, reg) ({					\
	u16 __r = (reg), __v = b43_phy_read((dev), __r);		\
	if (B43_DEBUG)							\
		b43info((dev)->wl, "phy   rd 0x%04x = 0x%04x\n",		\
		       __r, __v);					\
	__v;								\
})
#define b43_radio_read_log(dev, reg) ({					\
	u16 __r = (reg), __v = b43_radio_read((dev), __r);		\
	if (B43_DEBUG)							\
		b43info((dev)->wl, "radio rd 0x%04x = 0x%04x\n",		\
		       __r, __v);					\
	__v;								\
})
#define b43_shm_read16_log(dev, routing, offset) ({			\
	u16 __o = (offset);						\
	u16 __v = b43_shm_read16((dev), (routing), __o);		\
	if (B43_DEBUG)							\
		b43info((dev)->wl, "shm   rd 0x%04x = 0x%04x\n",		\
		       __o, __v);					\
	__v;								\
})
/* The same for @len 16-bit cells of a PHY table. */
#define B43_ACTAB_READ_LOG_MAX	16
#define b43_actab_read_log(dev, id, offset, len) do {			\
	u16 __id = (id), __off = (offset);				\
	u16 __buf[B43_ACTAB_READ_LOG_MAX];				\
	size_t __n = (len), __i;					\
									\
	if (B43_WARN_ON(__n > ARRAY_SIZE(__buf)))			\
		break;							\
	b43_actab_read_bulk((dev), __id, __off, 16, __n, __buf);	\
	for (__i = 0; B43_DEBUG && __i < __n; __i++)			\
		b43info((dev)->wl, "table rd 0x%02x:0x%04x = 0x%04x\n",	\
			__id, (u16)(__off + __i), __buf[__i]);		\
} while (0)

void b43_phy_ac_reset_cca(struct b43_wldev *dev);

/*
 * One turn of the radar detector poll: mac_suspend, the levels of the two
 * pulse FIFOs (PHY 0x0251 and 0x0252), draining the non-empty ones,
 * mac_enable. The core calls it every 150 ms while mac80211 asks for
 * detection, also after the check has closed. Returns whether a pulse
 * arrived since the last poll.
 */
bool b43_phy_ac_radar_poll(struct b43_wldev *dev);

/*
 * The BSS is up after a channel availability check: run the calibrations
 * that transmit, which the check held back. Called from the core's
 * cac_done hook; the core reopens the BSS AMT row. Nothing to do on a
 * channel without the radar duty, where the switch already calibrated.
 */
void b43_phy_ac_bss_up(struct b43_wldev *dev);

/*
 * Post-channel-setup calibrations, in the order
 * b43_phy_ac_calibration_block() calls them. Each is a phase transcribed
 * from a capture, documented at its definition.
 */
void b43_phy_ac_post_cal_finalize(struct b43_wldev *dev);
void b43_phy_ac_post_cal_finalize_iter3(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply(struct b43_wldev *dev);
void b43_phy_ac_post_rxiqcal_stage2(struct b43_wldev *dev);
void b43_phy_ac_rxcal_afe_calibrate(struct b43_wldev *dev);
void b43_phy_ac_rxcal_afe_finalize_gain_luts(struct b43_wldev *dev);

/* Open-loop TX power; INDEX_DEFAULT is the captured fixed index. */
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
void b43_phy_ac_rxiqcal_coeff_tables_reset(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply_second_stage(struct b43_wldev *dev);
void b43_phy_ac_rxgain_config_readback(struct b43_wldev *dev);
void b43_phy_ac_rxgain_config_apply(struct b43_wldev *dev);
void b43_phy_ac_radio_iqcal_config(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_dds_seed_tone(struct b43_wldev *dev, int step);
void b43_phy_ac_rxiqcal_meas_post_dds_apply(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_meas_post_dds_apply_v2(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply_coefficients(struct b43_wldev *dev);
void b43_phy_ac_radio_iqcal_teardown(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_teardown_apply_defaults(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev, u16 gate);

/*
 * One AFE cal iteration: arm a command on 0x0380, wait on the busy bit,
 * read the result back and rewrite it at @wr_off. @core_off is
 * core * 0x200.
 */
void b43_phy_ac_rxcal_afe_iter(struct b43_wldev *dev,
			       u16 cmd, u16 core_off,
			       const u16 *pre_clear_offs, u8 n_pre_clear,
			       u16 rd_off, u8 rw_len, u16 wr_off);

/*
 * One round of the loopback gain search. @core_mask selects the round's
 * cores and @r734_vals is indexed by core number.
 */
void b43_phy_ac_gainctrl_final_apply(struct b43_wldev *dev,
				     bool with_peek_preamble,
				     u8 core_mask,
				     const u16 r734_vals[3]);

/*
 * One watchdog turn, once a second from the bring-up to the radio going
 * down: sampling phase, statistics, the tempsense every temps_period
 * turns, the region dump every thirty. What the turn carries follows from
 * its own counters; the caller only ticks it.
 */
void b43_phy_ac_watchdog(struct b43_wldev *dev);
bool b43_phy_ac_txpwr_recalc(struct b43_wldev *dev);

/*
 * Completion of the noise sample, called by the core from its interrupt
 * path; see the definition.
 */
void b43_phy_ac_noise_sample_done(struct b43_wldev *dev);

/* Helpers across the MAC/PHY boundary; rationale in helpers_phy_ac.c. */
void b43_phy_ac_mhf_maskset(struct b43_wldev *dev, u16 slot, u16 mask, u16 val);
void b43_mac_bw_set(struct b43_wldev *dev, u32 bw);
void b43_phy_ac_force_clock(struct b43_wldev *dev, bool force);

/*
 * Trace points of the userspace harnesses in test/, which define them.
 * B43_AC_BLOCK() also names the sections of the bring-up in the kernel log,
 * so a hang shows the last one reached. Unlike b43info(), which is rate
 * limited once the core has started, it never drops a line: a dropped one
 * would point at the wrong section.
 */
#ifndef B43_AC_FN
#define B43_AC_FN() do { } while (0)
#endif
#ifndef B43_AC_BLOCK
#define B43_AC_BLOCK(dev, name) \
	wiphy_info((dev)->wl->hw->wiphy, "AC-PHY: %s: %s\n", __func__, name)
#endif
#ifndef B43_AC_CORE_SITE
#define B43_AC_CORE_SITE(dev, site) do { } while (0)
#endif

#endif /* B43_PHY_AC_H_ */
