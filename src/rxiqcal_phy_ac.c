// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 AC-PHY -- RX I/Q calibration.
 *
 * The setup -> gainctrl -> measure -> solve -> apply -> cleanup skeleton and
 * the solve itself come from the brcmsmac N-PHY calibration
 * (brcm80211/brcmsmac/phy/phy_n.c: wlc_phy_cal_rxiq_nphy_rev3,
 * wlc_phy_calc_rx_iq_comp_nphy, wlc_phy_rx_iq_est_nphy). The solve is
 * PHY-independent; the register map is adapted to AC-PHY rev 1 with the
 * 2069 radio.
 *
 * Settled by the captured read values:
 *   - accumulator layout: +3/+2 is i_pwr, +5/+4 is q_pwr, +1/+0 is iq_prod
 *     (signed and small). The solve only reproduces the stock driver's
 *     coefficients with this mapping.
 *   - the a/b coefficients live in 0x?a0/0x?a1 per chain, s10 format.
 *   - b43_phy_ac_rxiqcal_comp_update() is bit-exact on three
 *     measurement-to-coefficient vectors: the sum of two rounds of 0x4000
 *     samples.
 *   - the iteration and tone-mode sequence is a fixed schedule, not a
 *     hill-climb, and the measurement gain is never altered: the gain
 *     registers are only saved and restored around the measurement.
 *
 * The RX-IQ path in use is the transcribed one in phy_ac.c, orchestrated by
 * b43_phy_ac_set_channel_calibrations(); its leaves (rxcal_radio_setup,
 * rxcal_tone_setup/arm, rxcal_gainctrl, rxcal_cleanup, rxcal_radio_cleanup)
 * live there too. What remains in this file is the harness-only
 * estimator/validator -- rxiqcal_est/rxiqcal_coeffs, the tone play (tx_tone,
 * stopplayback, rxiqcal_set_tone, rxiqcal_apply_gain) and the two entry points
 * rxiqcal_est_debug and rxiqcal_comp_update -- reachable only from the
 * `ac_trace rxiq_est_debug`/`rxiq_comp` flows, never from the driver. It
 * validates the RX-IQ math against the captures. The one call it shares with
 * the production path is b43_phy_ac_rxgain_bw(), which stays in phy_ac.c. See
 * docs/rxiq-cal-analysis.md.
 */
#include <linux/kernel.h>	/* int_sqrt */
#include "b43.h"
#include "phy_ac.h"
#include "tables_phy_ac.h"
#include "radio_2069.h"
#include "rxiqcal_phy_ac.h"

/* Per-core estimator output: I/Q powers and cross product from the
 * correlator. Same shape as brcmsmac's struct phy_iq_est. */
struct b43_phy_ac_iq_est {
	s32 iq_prod;
	u32 i_pwr;
	u32 q_pwr;
};

/* Per-core RX compensation coefficients. Like nphy_iq_comp but extended to
 * three cores: on the 3x3 agcombo the vendor programs core 2 as well. */
struct b43_phy_ac_iq_comp {
	s16 a[3], b[3];
};

/*
 * Minimum I+Q power below which the measurement is not trustworthy, the
 * N-PHY NPHY_MIN_RXIQ_PWR value kept as a placeholder. The AC-PHY
 * accumulator scale still has to be confirmed on hardware.
 */
#define B43_PHY_AC_MIN_RXIQ_PWR		0x100
#define B43_PHY_AC_RXIQ_CAL_RETRY	2
#define B43_PHY_AC_RXCAL_NUM_SAMPS	1024	/* 0x400 in the capture */

/*
 * RX-IQ correlator registers, confirmed against the capture (the
 * down-to-bss-up trace, one block of 24 episodes plus seven more). These
 * are writes and commands, so they are captured verbatim; only the read
 * values, the accumulators, are undefined in the trace, and the silicon
 * supplies those at runtime.
 */
#define B43_PHY_AC_RXIQ_CMD		0x0270	/* bit0 = start, bit1 = iqMode */
#define  B43_PHY_AC_RXIQ_START		0x0001
#define  B43_PHY_AC_RXIQ_IQMODE		0x0002
#define B43_PHY_AC_RXIQ_WAIT		0x0271	/* [7:0] = wait_time */
#define B43_PHY_AC_RXIQ_NSAMP		0x0272	/* num_samps */
/* Per-core accumulators: three Hi/Lo pairs based at 0x06c0 + core * 0x200,
 * read in the order +3,+2 / +5,+4 / +1,+0. */
#define B43_PHY_AC_RXIQ_ACC(core)	(u16)(0x06c0 + (core) * 0x200)

/*
 * Register map taken from the capture.
 *
 * Filled in and confirmed: rxiq_est, rxiq_coeffs, rxcal_tone_setup/arm,
 * rxcal_gainctrl and its step helper, rxcal_apply_gain, tx_tone,
 * stopplayback.
 *
 * Still stubs: rxcal_radio_setup, rxcal_cleanup and rxcal_radio_cleanup,
 * some 300 RMW ops to be done in verified pieces. They are called from
 * channel_setup_tail2() in phy_ac.c, so what they do not emit is missing
 * from the live path.
 */

/*
 * RX-IQ correlator: arm num_samps samples, wait for completion, read the
 * per-core i_pwr/q_pwr/iq_prod accumulators. Modelled on
 * wlc_phy_rx_iq_est_nphy. The register sequence is confirmed by the
 * capture:
 *   WR 0x0272 = num_samps; MOD 0x0271[7:0] = wait_time (32);
 *   MOD 0x0270 clear iqMode; MOD 0x0270 set start; poll RD 0x0270;
 *   read the accumulators at 0x06c0 + core * 0x200, so 0x08c0 for core 1.
 *
 * The accumulator mapping is confirmed by the agcombo rescan capture, which
 * records the read values: the +3,+2 pair is i_pwr, +5,+4 is q_pwr and
 * +1,+0 is iq_prod, high half first. It agrees with the measured values
 * (i and q large and similar, iq small and signed) and with the solve: only
 * with this mapping does rx_iq_comp_update reproduce the coefficients the
 * vendor writes.
 */
static int b43_phy_ac_rxiqcal_est(struct b43_wldev *dev,
			       struct b43_phy_ac_iq_est *est,
			       u16 num_samps, u8 wait_time)
{
	B43_AC_FN();
	unsigned int core, i;
	bool timed_out = true;

	b43_phy_write(dev, B43_PHY_AC_RXIQ_NSAMP, num_samps);
	b43_phy_maskset(dev, B43_PHY_AC_RXIQ_WAIT, (u16)~0x00ff, wait_time);
	b43_phy_mask(dev, B43_PHY_AC_RXIQ_CMD, (u16)~B43_PHY_AC_RXIQ_IQMODE);
	b43_phy_set(dev, B43_PHY_AC_RXIQ_CMD, B43_PHY_AC_RXIQ_START);

	for (i = 0; i < 5000; i++) {
		if (!(b43_phy_read(dev, B43_PHY_AC_RXIQ_CMD) &
		      B43_PHY_AC_RXIQ_START)) {
			timed_out = false;
			break;
		}
		udelay(10);
	}
	if (timed_out) {
		b43err(dev->wl, "phy-ac: rxiqcal_est timeout\n");
		return -ETIMEDOUT;
	}

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 b = B43_PHY_AC_RXIQ_ACC(core);

		est[core].i_pwr =
			(b43_phy_read(dev, b + 3) << 16) | b43_phy_read(dev, b + 2);
		est[core].q_pwr =
			(b43_phy_read(dev, b + 5) << 16) | b43_phy_read(dev, b + 4);
		est[core].iq_prod =
			(b43_phy_read(dev, b + 1) << 16) | b43_phy_read(dev, b + 0);
	}
	return 0;
}

/*
 * Read (write = 0) or write (write = 1) the per-core RX compensation
 * coefficients. The N-PHY equivalent is wlc_phy_rx_iq_coeffs_nphy, which
 * uses PHY registers 0x9a-0x9d.
 *
 * The AC-PHY map is confirmed by the agcombo rescan-to-bss-ch36 capture,
 * which records the read values: a at 0x06a0 + core * 0x200, b at
 * 0x06a1 + core * 0x200, s10 in field [9:0]. rx_iq_comp_update() below
 * reproduces that capture's three measurement-to-coefficient vectors
 * bit-exactly.
 */
#define B43_PHY_AC_RXIQ_COMP_A(core)	(u16)(0x06a0 + (core) * 0x200)
#define B43_PHY_AC_RXIQ_COMP_B(core)	(u16)(0x06a1 + (core) * 0x200)

static void b43_phy_ac_rxiqcal_coeffs(struct b43_wldev *dev, u8 write,
				   struct b43_phy_ac_iq_comp *comp)
{
	B43_AC_FN();
	unsigned int core;

	for (core = 0; core < dev->phy.ac->num_cores && core < 3; core++) {
		if (write) {
			b43_phy_write(dev, B43_PHY_AC_RXIQ_COMP_A(core),
				      comp->a[core] & 0x3ff);
			b43_phy_write(dev, B43_PHY_AC_RXIQ_COMP_B(core),
				      comp->b[core] & 0x3ff);
		} else {
			comp->a[core] = sign_extend32(
				b43_phy_read(dev,
					     B43_PHY_AC_RXIQ_COMP_A(core)), 9);
			comp->b[core] = sign_extend32(
				b43_phy_read(dev,
					     B43_PHY_AC_RXIQ_COMP_B(core)), 9);
		}
	}
}

/*
 * Inject the calibration tone and set up the loopback, transcribed verbatim
 * from a 14-op block of the capture.
 *
 * The ops that carry mask=0x0000 in the trace are register and/or, which the
 * tracer does not distinguish. The choice made here is inferred from the
 * value -- a 0xff.. clear-mask reads as and, a small value as or -- so it is
 * a heuristic and still to be confirmed.
 *
 * The three writes below are the sample-play control (num_samps, loops,
 * wait), not the tone shape: frequency and amplitude live in the CORDIC table
 * loaded into 0x000e, so the freq_hz/amp parameters are unused here. NSAMP is
 * the 20 MHz value (0x27); the correlator-kick sites use the bandwidth-aware
 * b43_phy_ac_rxiqcal_kick_len() instead.
 */
static void b43_phy_ac_tx_tone(struct b43_wldev *dev, u32 freq_hz, u16 amp)
{
	B43_AC_FN();
	b43_phy_mask(dev, 0x0471, (u16)~0x0001);	/* and 0xfffe */
	b43_phy_write(dev, B43_PHY_AC_SAMP_PLAY_NSAMP, 0x0027);
	b43_phy_write(dev, B43_PHY_AC_SAMP_PLAY_LOOPS, 0xffff);
	b43_phy_write(dev, B43_PHY_AC_SAMP_PLAY_WAIT, 0x003c);
	b43_phy_set(dev, 0x0400, 0x0001);		/* or */
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);	/* and 0xfffb */
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~B43_PHY_AC_SAMP_PLAY_START);	/* and 0xfffe */
	b43_phy_mask(dev, 0x0382, (u16)~0xc000);	/* and 0x3fff */
	b43_phy_set(dev, B43_PHY_AC_SAMP_PLAY_CTL, B43_PHY_AC_SAMP_PLAY_START);		/* or */
	udelay(1);
	b43_phy_write(dev, 0x0400, 0x0000);
	(void)freq_hz;
	(void)amp;
}

/* Stop the tone after the estimate. Transcribed from the capture; the
 * and/or split is the same heuristic as above. */
static void b43_phy_ac_stopplayback(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_set(dev, B43_PHY_AC_SAMP_PLAY_CTL, B43_PHY_AC_SAMP_PLAY_STOP);		/* or */
	b43_phy_mask(dev, B43_PHY_AC_SAMP_PLAY_CTL, (u16)~0x0004);	/* and 0xfffb */
}

/* ===================== DEBUG: MEASURE-ONLY HELPER ======================== */

/*
 * Tone-mode values observed in the ch36 trace: the per-core "tone enable"
 * register (0x0734 + core*0x200) cycles through {4, 2, 1, 0} across four
 * consecutive 1024-sample rxiq_est calls. Core 2 (if present) follows its
 * own sequence {4, 1, 0, -}. The tone frequency (0x0730) and max (0x0731)
 * are constant at 0x00b0 and 0x0004 across all four.
 */
static const u8 rxiqcal_tone_modes_c01[] = { 4, 2, 1, 0 };
static const u8 rxiqcal_tone_modes_c2[]  = { 4, 1, 0, 0 };

/*
 * Per-core tone engine: program frequency, max amplitude, and tone-enable.
 * Registers at 0x0730/0x0731/0x0734 + core * 0x200. Additionally set the
 * three AFE override bits in 0x0722 + core * 0x200.
 */
static void b43_phy_ac_rxiqcal_set_tone(struct b43_wldev *dev, u8 core,
				     u16 freq, u16 fmax, u8 tone_mode)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);

	b43_phy_write(dev, 0x0730 + s, freq);
	b43_phy_write(dev, 0x0731 + s, fmax);
	b43_phy_write(dev, 0x0734 + s, tone_mode);
	b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0004, 0x0004);
	b43_phy_maskset(dev, 0x0722 + s, (u16)~0x0008, 0x0008);
}

/*
 * Gain override: apply the measurement gain on <core>, then micro-settle.
 * Two-step write sequence from the trace: first the "armed"
 * values (0x00fa / 0x01d3 / 0x07e6 a 20 MHz), then the settled values
 * (0x007a / 0x01d3 / 0x07e2). Le due parole su 0x0739 e 0x073a seguono la
 * larghezza; vedi b43_phy_ac_rxgain_bw(). The read-before-write is a save — we skip it here
 * because the caller saves and restores the registers.
 */
static void b43_phy_ac_rxiqcal_apply_gain(struct b43_wldev *dev, u8 core)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);

	const struct b43_phy_ac_rxgain_bw *g = b43_phy_ac_rxgain_bw(dev);
	u16 w73a = (u16)(0x0190 | g->f73a_07 | g->f73a_08 | g->f73a_60);

	b43_phy_write(dev, 0x0739 + s, (u16)(0x0080 | g->f739_7e));
	b43_phy_write(dev, 0x073a + s, w73a);
	b43_phy_write(dev, 0x0725 + s, 0x07e6);
	/* micro-settle: same sequence both cores */
	b43_phy_write(dev, 0x0725 + s, 0x07e2);
	b43_phy_write(dev, 0x073a + s, w73a);
	b43_phy_write(dev, 0x0739 + s, g->f739_7e);
}

/*
 * Debug-only RX-IQ estimation: runs the 4-tone-mode measurement sequence
 * observed in the ch36 trace and prints the raw accumulator
 * values. This does NOT compute or apply compensation coefficients — it
 * only reads what the hardware reports, so the results can validate (or
 * falsify) the register-map and accumulator-layout assumptions.
 *
 * Call point: after txpwr_by_index, before rxgainctrl_regs in op_switch_channel:
 * in the trace it sits between the txpwr tail and the rxgainctrl block.
 */
void b43_phy_ac_rxiqcal_est_debug(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac_iq_est est[3];
	u16 save_gain[B43_PHY_AC_MAX_CORES * 3];	/* 0x0739, 0x073a, 0x0725 */
	u16 save_tone[B43_PHY_AC_MAX_CORES * 3];	/* 0x0730, 0x0731, 0x0734 */
	u16 saved_040f;
	unsigned int core, tm;
	int err;
	u8 num_cores = dev->phy.ac->num_cores;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	b43dbg(dev->wl, "phy-ac: rxiq_est_debug — start (%u cores)\n",
	       num_cores);

	/* Save gain and tone registers. */
	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		u16 s = (u16)(core * 0x200);


		save_gain[core * 3 + 0] = b43_phy_read(dev, 0x0739 + s);
		save_gain[core * 3 + 1] = b43_phy_read(dev, 0x073a + s);
		save_gain[core * 3 + 2] = b43_phy_read(dev, 0x0725 + s);
	}
	for (core = 0; core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		save_tone[core * 3 + 0] = b43_phy_read(dev, 0x0730 + s);
		save_tone[core * 3 + 1] = b43_phy_read(dev, 0x0731 + s);
		save_tone[core * 3 + 2] = b43_phy_read(dev, 0x0734 + s);
	}
	saved_040f = b43_phy_read_log(dev, 0x040f);

	/*
	 * Tone engine init: a 46-op scalar sequence that programs the RX-IQ
	 * classifier and clears the per-core IQ coefficients before the
	 * measurement. The op list is the same on ch36 BW20 and ch36 BW40,
	 * down to the values written.
	 *
	 * Left out: the two table writes to id 0x000c offsets 0x0060 and
	 * 0x0064, two words each, that the vendor issues in the middle of
	 * this sequence. Their values depend on channel and bandwidth
	 * (ch36 BW20 gives {0x0062,0xfffd}/{0x0023,0x0003}, ch36 BW40 gives
	 * {0x0060,0xfffe}/{0x0025,0x0002}, ch44 BW20 gives
	 * {0x0061,0xfffe}/{0x0022,0x0002}), and without the formula that
	 * derives them, writing nothing beats writing another channel's
	 * values.
	 *
	 * Also left out, for the same reason of scope: the large gain
	 * override, some 230 MODs over 0x072x-0x074x per core, and the
	 * 40-word table write to id 0x000e offset 0. Both are outside this
	 * measure-only helper.
	 */
	b43_phy_set(dev, 0x0400, 0x0003);
	b43_phy_set(dev, 0x0402, 0x0020);
	(void)b43_phy_read_log(dev, 0x0403);
	b43_phy_write(dev, 0x0400, 0x0000);
	b43_phy_write(dev, 0x019e, 0x03d0);
	b43_phy_write(dev, 0x06a0, 0x0000);
	b43_phy_write(dev, 0x06a1, 0x0000);
	b43_phy_write(dev, 0x08a0, 0x0000);
	b43_phy_write(dev, 0x08a1, 0x0000);
	b43_phy_mask(dev, 0x0211, (u16)~0x0001);
	b43_phy_mask(dev, 0x040f, (u16)~0x0200);

	/* CCA reset pulse. */
	b43_phy_set(dev, B43_PHY_AC_BBCFG, B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_mask(dev, B43_PHY_AC_BBCFG, (u16)~B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;

	/* Arm tone engine (reuse existing helper). */
	b43_phy_ac_tx_tone(dev, 0, 0);

	/*
	 * Apply gain override on all present cores. The vendor arms all 3, core
	 * 2 included (WR 0x0b39/b3a/b25). Arming fewer leaves the estimator
	 * polling the status of an un-armed core until it times out.
	 */
	for (core = 0; core < num_cores; core++)
		b43_phy_ac_rxiqcal_apply_gain(dev, core);

	/* Sweep four tone modes, 1024 samples each. */
	for (tm = 0; tm < ARRAY_SIZE(rxiqcal_tone_modes_c01); tm++) {
		for (core = 0; core < num_cores; core++) {
			const u8 *modes = (core < 2) ? rxiqcal_tone_modes_c01
						      : rxiqcal_tone_modes_c2;
			b43_phy_ac_rxiqcal_set_tone(dev, core, 0x00b0, 0x0004,
						 modes[tm]);
		}

		err = b43_phy_ac_rxiqcal_est(dev, est, 0x0400, 32);
		if (err) {
			b43dbg(dev->wl,
			       "phy-ac: rxiq_est_debug tone_mode=%u — timeout\n",
			       rxiqcal_tone_modes_c01[tm]);
			continue;
		}

		for (core = 0; core < num_cores; core++) {
			b43dbg(dev->wl,
			       "phy-ac: rxiq_est_debug tm=%u core=%u "
			       "i_pwr=0x%08x q_pwr=0x%08x iq_prod=0x%08x\n",
			       rxiqcal_tone_modes_c01[tm], core,
			       est[core].i_pwr, est[core].q_pwr,
			       (u32)est[core].iq_prod);
		}
	}

	/* Teardown tone. */
	b43_phy_ac_stopplayback(dev);

	/* Restore tone registers. */
	for (core = 0; core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		b43_phy_write(dev, 0x0730 + s, save_tone[core * 3 + 0]);
		b43_phy_write(dev, 0x0731 + s, save_tone[core * 3 + 1]);
		b43_phy_write(dev, 0x0734 + s, save_tone[core * 3 + 2]);
	}
	/* Restore gain registers. */
	for_each_set_bit(core, &dev->phy.ac->coremask, num_cores) {
		u16 s = (u16)(core * 0x200);


		b43_phy_write(dev, 0x0739 + s, save_gain[core * 3 + 0]);
		b43_phy_write(dev, 0x073a + s, save_gain[core * 3 + 1]);
		b43_phy_write(dev, 0x0725 + s, save_gain[core * 3 + 2]);
	}

	/*
	 * Cleanup the way the vendor does it: restore the 0x040f gate, then a
	 * CCA pulse to commit the state. The capture issues that pulse at the
	 * end of the RX-IQ block, before moving to the next phase.
	 *
	 * The vendor's other restores, to table id 0x000c offsets
	 * 0x0063/0x0067/0x0073/0x0077, undo the large gain override that is
	 * not ported here, so there is nothing to restore.
	 */
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, saved_040f & 0x0200);
	b43_phy_set(dev, B43_PHY_AC_BBCFG, B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_mask(dev, B43_PHY_AC_BBCFG, (u16)~B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;

	b43dbg(dev->wl, "phy-ac: rxiq_est_debug — done\n");
}

/* The generic algorithm, ported as-is. */

/*
 * Measure with the correlator and solve for the IQ compensation
 * coefficients: from the estimate (ii = I^2, qq = Q^2, iq = I*Q), derive the
 * (a, b) pair that cancels the gain and phase imbalance. Same shape as
 * wlc_phy_calc_rx_iq_comp_nphy; the AC-specific details -- two rounds
 * summed, round-to-nearest, s10 format in 0x?a0/0x?a1 -- are confirmed
 * bit-exactly against the three measurement-to-coefficient vectors of the
 * agcombo capture. See docs/rxiq-cal-analysis.md.
 *
 * Still to verify on AC-PHY: the scale of B43_PHY_AC_MIN_RXIQ_PWR. It holds
 * the N-PHY value as a placeholder, and the captured powers are orders of
 * magnitude above it, so the guard has never been exercised.
 */
int b43_phy_ac_rxiqcal_comp_update(struct b43_wldev *dev, u8 core_mask)
{
	B43_AC_FN();
	struct b43_phy_ac_iq_est est[3], est2[3];
	struct b43_phy_ac_iq_comp old_comp, new_comp;
	unsigned int core;
	uint retry = 0;
	int err;

	if (!core_mask)
		return 0;

	b43_phy_ac_rxiqcal_coeffs(dev, 0, &old_comp);
	memset(&new_comp, 0, sizeof(new_comp));
	b43_phy_ac_rxiqcal_coeffs(dev, 1, &new_comp);

retry_cal:
	/*
	 * Two estimates of 0x4000 samples, summed. The vendor measures two
	 * rounds per core and solves on the sum of the accumulators, not on
	 * the mean of the coefficients: on agcombo core 0 the rounds give
	 * a = +7 and a = -12, and the coefficient written is -3, which is
	 * solve(round1 + round2).
	 */
	err = b43_phy_ac_rxiqcal_est(dev, est, 0x4000, 32);
	if (err)
		return err;
	err = b43_phy_ac_rxiqcal_est(dev, est2, 0x4000, 32);
	if (err)
		return err;

	new_comp = old_comp;

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		s64 iq, num;
		u64 ii, qq, v;
		s32 a, b;

		if (!((core_mask >> core) & 1))
			continue;

		iq = (s64)est[core].iq_prod + est2[core].iq_prod;
		ii = (u64)est[core].i_pwr + est2[core].i_pwr;
		qq = (u64)est[core].q_pwr + est2[core].q_pwr;

		if (ii + qq < B43_PHY_AC_MIN_RXIQ_PWR || ii == 0) {
			if (retry < B43_PHY_AC_RXIQ_CAL_RETRY) {
				retry++;
				goto retry_cal;
			}
			new_comp = old_comp;	/* give up, keep the old ones */
			break;
		}

		/*
		 * a = -(iq/ii), b = sqrt(qq/ii - a^2) - 1.0, both in Q10, with
		 * round-to-nearest on each. The three agcombo vectors tell
		 * that apart from the N-PHY floor: a = -2.61 becomes -3,
		 * b = 109.97 becomes 110, b = 59.83 becomes 60. What happens
		 * at an exact half is not observed in any capture.
		 */
		num = -(iq << 10);
		a = (s32)div64_s64(num + (num < 0 ? -(s64)(ii >> 1)
					          : (s64)(ii >> 1)), ii);

		v = div64_u64((qq << 20) + (ii >> 1), ii) - (u64)(a * a);
		b = (s32)int_sqrt64(v);
		if (v - (u64)b * b > (u64)b)
			b++;		/* rounded sqrt, not floor */
		b -= 1 << 10;

		new_comp.a[core] = (s16)a;
		new_comp.b[core] = (s16)b;
	}

	b43_phy_ac_rxiqcal_coeffs(dev, 1, &new_comp);
	return 0;
}
