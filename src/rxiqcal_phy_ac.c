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
 *   - b43_phy_ac_rx_iq_comp_update() is bit-exact on three
 *     measurement-to-coefficient vectors: the sum of two rounds of 0x4000
 *     samples.
 *   - the iteration and tone-mode sequence is a fixed schedule, not a
 *     hill-climb, and the measurement gain is never altered: the gain
 *     registers are only saved and restored around the measurement.
 *
 * Not filled in: rxcal_phy_setup, radio_setup and cleanup, some 300 RMW ops
 * to be done in pieces checked against the correlator.
 *
 * b43_phy_ac_rxiqcal() returns -EOPNOTSUPP while REGMAP_FILLED is 0 and has
 * no callers: the RX-IQ path in use is the transcribed one in phy_ac.c. See
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

/* Set to 1 once the hardware stubs below are filled in from a capture. */
#define B43_PHY_AC_RXIQCAL_REGMAP_FILLED	0

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
 * Still stubs: rxcal_phy_setup, rxcal_radio_setup, rxcal_cleanup and
 * rxcal_radio_cleanup, some 300 RMW ops to be done in verified pieces.
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
static int b43_phy_ac_rxiq_est(struct b43_wldev *dev,
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
		b43err(dev->wl, "phy-ac: rx_iq_est timeout\n");
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

static void b43_phy_ac_rxiq_coeffs(struct b43_wldev *dev, u8 write,
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

/* PHY-side setup of the measurement loopback, after
 * wlc_phy_rxcal_physetup_nphy. */
static void b43_phy_ac_rxcal_phy_setup(struct b43_wldev *dev, u8 rx_core)
{
	/* TODO: the PHY loopback sequence is not transcribed yet. */
}

/*
 * PHY-side setup of the calibration tone generator, run once before rxcal
 * rather than per core. 26 ops:
 *
 *   1. peek the table gate 0x019e, peek and clear bit 9 of 0x040f, peek
 *      0x0394 and 0x0393.
 *   2. pass 1, for each active core: peek plus write of three registers
 *      (0x0739+s, 0x073a+s, 0x0725+s) with {0x00fa, 0x01d3, 0x07e6}, cores
 *      in forward order.
 *   3. pass 2, writes only, six ops with the dithered values
 *      {0x007a, 0x01d3, 0x07e2}, cores in reverse order and registers in
 *      reverse order too (0x0725 then 0x073a then 0x0739, per core).
 *   4. peek 0x0393, write 0x0394 = 0x0110, write 0x0393 = 0x8000 to arm the
 *      generator.
 */
void b43_phy_ac_rxcal_tone_setup(struct b43_wldev *dev)
{
	B43_AC_FN();
	static const u16 reg_off[3] = { 0x0039, 0x003a, 0x0025 }; /* relative to 0x0700 */
	static const u16 pass1_vals[3] = { 0x00fa, 0x01d3, 0x07e6 };
	static const u16 pass2_vals[3] = { 0x007a, 0x01d3, 0x07e2 };
	u8 c, num_cores = dev->phy.ac->num_cores;
	u8 mask = dev->phy.ac->coremask;
	int i;

	b43_phy_read_log(dev, 0x019e);                       /* #39709 */
	b43_phy_read_log(dev, 0x040f);                       /* #39710 */
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);       /* #39711 clr bit 9 */
	b43_phy_read_log(dev, 0x0394);                       /* #39712 */
	b43_phy_read_log(dev, 0x0393);                       /* #39713 */

	/* Pass 1: forward core, forward reg */
	for (c = 0; c < num_cores; c++) {
		u16 s = (u16)(c * 0x200);
		if (!((mask >> c) & 1))
			continue;
		for (i = 0; i < 3; i++) {
			b43_phy_read_log(dev, 0x0700 + reg_off[i] + s);
			b43_phy_write(dev,    0x0700 + reg_off[i] + s, pass1_vals[i]);
		}
	}

	/* Pass 2: reverse core, reverse reg — dithered values */
	for (c = num_cores; c-- > 0; ) {
		u16 s = (u16)(c * 0x200);
		if (!((mask >> c) & 1))
			continue;
		for (i = 2; i >= 0; i--) {
			b43_phy_write(dev, 0x0700 + reg_off[i] + s, pass2_vals[i]);
		}
	}
	/* The arm sequence (peek 0x0393, write 0x0394, write 0x0393 = 0x8000)
	 * is emitted separately by b43_phy_ac_rxcal_tone_arm(), called once
	 * per core with a slightly different 0x0394 value. */
}

/*
 * Arm the tone generator for the calibration of rx_core: peek 0x0393,
 * write 0x0394 = 0x0110 | core, write 0x0393 = 0x8000. The capture shows
 * 0x0110 for core 0 and 0x0111 for core 1.
 */
void b43_phy_ac_rxcal_tone_arm(struct b43_wldev *dev, u8 rx_core)
{
	B43_AC_FN();
	b43_phy_read_log(dev, 0x0393);
	b43_phy_write(dev,    0x0394, (u16)(0x0110 | rx_core));
	b43_phy_write(dev,    0x0393, 0x8000);
}

/*
 * One step of the gainctrl sweep: four radio maskset calls, which the tracer
 * expands to twelve ops, plus eight settling peeks, so twenty ops per step.
 * The two control bits on radio 0x000e arrive as e_bit1_val and e_bit2_val,
 * already masked to 0x0002/0 and 0x0004/0. Bits 0 and 1 of 0x016e + core
 * stride are toggled idempotently.
 *
 * The eight values read from PHY 0x0013 are stored in
 * rxcal_imbalance[rx_core][step_idx][0..7]. On real hardware they hold the
 * accumulator once the configuration has settled; in the trace harness they
 * are undefined and the peeks only exist for the op-for-op match.
 */
static void rxcal_gainctrl_step(struct b43_wldev *dev, u8 rx_core,
				u8 step_idx,
				u16 e_bit1_val, u16 e_bit2_val)
{
	u16 s = (u16)(rx_core * 0x200);
	int i;

	b43_radio_maskset(dev, 0x016e + s, (u16)~0x0002, 0x0002);
	b43_radio_maskset(dev, 0x000e + s, (u16)~0x0002, e_bit1_val);
	b43_radio_maskset(dev, 0x016e + s, (u16)~0x0001, 0x0001);
	b43_radio_maskset(dev, 0x000e + s, (u16)~0x0004, e_bit2_val);
	for (i = 0; i < 8; i++) {
		u16 v = b43_phy_read_log(dev, 0x0013);
		if (rx_core < ARRAY_SIZE(dev->phy.ac->rxcal_imbalance) &&
		    step_idx < ARRAY_SIZE(dev->phy.ac->rxcal_imbalance[0]))
			dev->phy.ac->rxcal_imbalance[rx_core][step_idx][i] = v;
	}
}

/*
 * Radio-side setup of the loopback, after wlc_phy_rxcal_radio_setup_nphy for
 * the 2056. 34 ops per core: seven opening peeks that save the seven
 * registers below, then nine masksets, each of which the tracer expands to a
 * MOD/RD/WR triplet -- 7 + 9 * 3.
 *
 * Saved by setup, rewritten by cleanup, in the stock driver's order.
 */
static const u16 b43_phy_ac_rxcal_radio_regs[7] = {
	0x016e, 0x000e, 0x0161, 0x0017, 0x015f, 0x0024, 0x0025,
};

void b43_phy_ac_rxcal_radio_setup(struct b43_wldev *dev, u8 rx_core)
{
	B43_AC_FN();
	u16 s = (u16)(rx_core * 0x200);

	/* Peek the seven values to be preserved. */
	{
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxcal_radio_regs); i++) {
			u16 v = b43_radio_read(dev,
					b43_phy_ac_rxcal_radio_regs[i] + s);

			if (rx_core < B43_PHY_AC_MAX_CORES)
				dev->phy.ac->rxcal_radio_saved[rx_core][i] = v;
		}
	}

	/* Nine programming masksets, each expanded to MOD+RD+WR by the wrap. */
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

/* Defined further down, used by the gainctrl. */
static void b43_phy_ac_tx_tone(struct b43_wldev *dev, u32 freq_hz, u16 amp);
static void b43_phy_ac_stopplayback(struct b43_wldev *dev);

/*
 * Sweep four loopback configurations for rx_core, 80 ops. Each step programs
 * two control bits on radio 0x000e + core stride, through 0x016e + stride
 * which drives the bit's gate, then waits eight reads of 0x0013 for
 * settling. The order of the four steps is a fixed schedule, not driven by
 * the measurements.
 *
 * The measurement gain is not a ladder, so the N-PHY model's idx parameter
 * maps to nothing here. Only the main setting is transcribed, with its
 * 0x07e6 -> 0x07e2 and 0x00fa -> 0x007a micro-settle; an alternative
 * setting (0x0725 = 0x0600, 0x0739 = 0x0000, 0x073a = 0x0180) appears twice
 * in the capture and is not ported, because the choice between the two does
 * not follow from the measurements -- the schedule is fixed -- and what does
 * select it is not established.
 *
 * A cross-core reading, one core injecting while the others receive, was
 * considered and is not supported: nowhere in the capture is one core's
 * accumulator read while another holds the tone. See
 * docs/rxiq-cal-analysis.md before starting from that premise again.
 */
void b43_phy_ac_rxcal_gainctrl(struct b43_wldev *dev, u8 rx_core)
{
	/* The four {bit1, bit2} combinations of 0x000e + stride, in the order
	 * the vendor emits them. Readings land in
	 * phy.ac->rxcal_imbalance[core][step]. */
	rxcal_gainctrl_step(dev, rx_core, 0, 0x0002, 0x0000);   /* (1, 0) */
	rxcal_gainctrl_step(dev, rx_core, 1, 0x0000, 0x0000);   /* (0, 0) baseline */
	rxcal_gainctrl_step(dev, rx_core, 2, 0x0002, 0x0004);   /* (1, 1) */
	rxcal_gainctrl_step(dev, rx_core, 3, 0x0000, 0x0004);   /* (0, 1) */
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
 * The tone frequency and amplitude are encoded in 0x0463/0x0461/0x0462
 * below, not in the parameters.
 */
static void b43_phy_ac_tx_tone(struct b43_wldev *dev, u32 freq_hz, u16 amp)
{
	B43_AC_FN();
	b43_phy_mask(dev, 0x0471, (u16)~0x0001);	/* #82499 and 0xfffe */
	b43_phy_write(dev, 0x0463, 0x0027);		/* #82500 */
	b43_phy_write(dev, 0x0461, 0xffff);		/* #82501 */
	b43_phy_write(dev, 0x0462, 0x003c);		/* #82502 */
	b43_phy_set(dev, 0x0400, 0x0001);		/* #82504 or */
	b43_phy_mask(dev, 0x0460, (u16)~0x0004);	/* #82505 and 0xfffb */
	b43_phy_mask(dev, 0x0460, (u16)~0x0001);	/* #82506 and 0xfffe */
	b43_phy_mask(dev, 0x0382, (u16)~0xc000);	/* #82507 and 0x3fff */
	b43_phy_set(dev, 0x0460, 0x0001);		/* #82508 or */
	udelay(1);					/* #82510 */
	b43_phy_write(dev, 0x0400, 0x0000);		/* #82512 */
	(void)freq_hz;
	(void)amp;
}

/* Stop the tone after the estimate. Transcribed from the capture; the
 * and/or split is the same heuristic as above. */
static void b43_phy_ac_stopplayback(struct b43_wldev *dev)
{
	B43_AC_FN();
	b43_phy_set(dev, 0x0460, 0x0002);		/* #82558 or */
	b43_phy_mask(dev, 0x0460, (u16)~0x0004);	/* #82559 and 0xfffb */
}

/*
 * Per-core PHY-side cleanup after the measurement, after
 * rxcal_cleanup_nphy: reset the 14 gain-control registers to their idle
 * values. The caller loops over cores, so all of core 0's writes come out
 * before any of core 1's.
 */
void b43_phy_ac_rxcal_cleanup(struct b43_wldev *dev, u8 rx_core)
{
	B43_AC_FN();
	static const struct { u16 off; u16 val; } wr[14] = {
		{ 0x073e, 0x0000 }, { 0x0727, 0x0004 }, { 0x073c, 0x0000 },
		{ 0x0721, 0x5000 }, { 0x0729, 0x1000 }, { 0x0720, 0x0180 },
		{ 0x0728, 0x0880 }, { 0x0724, 0x0000 }, { 0x0736, 0x0000 },
		{ 0x0725, 0x0600 }, { 0x0739, 0x0000 }, { 0x073a, 0x0180 },
		{ 0x0722, 0x0000 }, { 0x0734, 0x0000 },
	};
	u16 s = (u16)(rx_core * 0x200);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wr); i++)
		b43_phy_write(dev, wr[i].off + s, wr[i].val);
}

/*
 * Per-core radio-side cleanup: restore the seven radio registers that
 * rxcal_radio_setup saved. Same per-core ordering as above.
 */
void b43_phy_ac_rxcal_radio_cleanup(struct b43_wldev *dev, u8 rx_core)
{
	B43_AC_FN();
	u16 s = (u16)(rx_core * 0x200);
	unsigned int i;

	if (rx_core >= B43_PHY_AC_MAX_CORES)
		return;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_rxcal_radio_regs); i++)
		b43_radio_write(dev, b43_phy_ac_rxcal_radio_regs[i] + s,
				dev->phy.ac->rxcal_radio_saved[rx_core][i]);
}

/* ===================== DEBUG: MEASURE-ONLY HELPER ======================== */

/*
 * Tone-mode values observed in the ch36 trace: the per-core "tone enable"
 * register (0x0734 + core*0x200) cycles through {4, 2, 1, 0} across four
 * consecutive 1024-sample rxiq_est calls. Core 2 (if present) follows its
 * own sequence {4, 1, 0, -}. The tone frequency (0x0730) and max (0x0731)
 * are constant at 0x00b0 and 0x0004 across all four.
 */
static const u8 rxiq_tone_modes_c01[] = { 4, 2, 1, 0 };
static const u8 rxiq_tone_modes_c2[]  = { 4, 1, 0, 0 };

/*
 * Per-core tone engine: program frequency, max amplitude, and tone-enable.
 * Registers at 0x0730/0x0731/0x0734 + core * 0x200. Additionally set the
 * three AFE override bits in 0x0722 + core * 0x200 (trace #50529-50531).
 */
static void b43_phy_ac_rxiq_set_tone(struct b43_wldev *dev, u8 core,
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
 * Two-step write sequence from trace (#50767-50784): first the "armed"
 * values (0x00fa / 0x01d3 / 0x07e6), then the settled values (0x007a /
 * 0x01d3 / 0x07e2). The read-before-write is a save — we skip it here
 * because the caller saves and restores the registers.
 */
static void b43_phy_ac_rxiq_apply_gain(struct b43_wldev *dev, u8 core)
{
	B43_AC_FN();
	u16 s = (u16)(core * 0x200);

	b43_phy_write(dev, 0x0739 + s, 0x00fa);
	b43_phy_write(dev, 0x073a + s, 0x01d3);
	b43_phy_write(dev, 0x0725 + s, 0x07e6);
	/* micro-settle: same sequence both cores, trace #50779-50784 */
	b43_phy_write(dev, 0x0725 + s, 0x07e2);
	b43_phy_write(dev, 0x073a + s, 0x01d3);
	b43_phy_write(dev, 0x0739 + s, 0x007a);
}

/*
 * Debug-only RX-IQ estimation: runs the 4-tone-mode measurement sequence
 * observed in the ch36 trace (#50526-51118) and prints the raw accumulator
 * values. This does NOT compute or apply compensation coefficients — it
 * only reads what the hardware reports, so the results can validate (or
 * falsify) the register-map and accumulator-layout assumptions.
 *
 * Call point: after txpwr_by_index, before rxgainctrl_regs in set_channel
 * (trace #50523 sits between the txpwr tail at ~#50128 and the rxgainctrl
 * block at ~#52337).
 */
void b43_phy_ac_rxiq_est_debug(struct b43_wldev *dev)
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
	for (core = 0; core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;

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

	/* CCA reset pulse (trace #50752-50753). */
	b43_phy_set(dev, B43_PHY_AC_BBCFG, B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_mask(dev, B43_PHY_AC_BBCFG, (u16)~B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;

	/* Arm tone engine (reuse existing helper). */
	b43_phy_ac_tx_tone(dev, 0, 0);

	/*
	 * Apply gain override on all present cores. Vendor arms all 3 (core 2
	 * included, see #50784 WR 0x0b39/b3a/b25); we did not before, and the
	 * estimator polled the status of an un-armed core and timed out.
	 */
	for (core = 0; core < num_cores; core++)
		b43_phy_ac_rxiq_apply_gain(dev, core);

	/* Sweep four tone modes, 1024 samples each. */
	for (tm = 0; tm < ARRAY_SIZE(rxiq_tone_modes_c01); tm++) {
		for (core = 0; core < num_cores; core++) {
			const u8 *modes = (core < 2) ? rxiq_tone_modes_c01
						      : rxiq_tone_modes_c2;
			b43_phy_ac_rxiq_set_tone(dev, core, 0x00b0, 0x0004,
						 modes[tm]);
		}

		err = b43_phy_ac_rxiq_est(dev, est, 0x0400, 32);
		if (err) {
			b43dbg(dev->wl,
			       "phy-ac: rxiq_est_debug tone_mode=%u — timeout\n",
			       rxiq_tone_modes_c01[tm]);
			continue;
		}

		for (core = 0; core < num_cores; core++) {
			b43dbg(dev->wl,
			       "phy-ac: rxiq_est_debug tm=%u core=%u "
			       "i_pwr=0x%08x q_pwr=0x%08x iq_prod=0x%08x\n",
			       rxiq_tone_modes_c01[tm], core,
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
	for (core = 0; core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;

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
int b43_phy_ac_rx_iq_comp_update(struct b43_wldev *dev, u8 core_mask)
{
	struct b43_phy_ac_iq_est est[3], est2[3];
	struct b43_phy_ac_iq_comp old_comp, new_comp;
	unsigned int core;
	uint retry = 0;
	int err;

	if (!core_mask)
		return 0;

	b43_phy_ac_rxiq_coeffs(dev, 0, &old_comp);
	memset(&new_comp, 0, sizeof(new_comp));
	b43_phy_ac_rxiq_coeffs(dev, 1, &new_comp);

retry_cal:
	/*
	 * Two estimates of 0x4000 samples, summed. The vendor measures two
	 * rounds per core and solves on the sum of the accumulators, not on
	 * the mean of the coefficients: on agcombo core 0 the rounds give
	 * a = +7 and a = -12, and the coefficient written is -3, which is
	 * solve(round1 + round2).
	 */
	err = b43_phy_ac_rxiq_est(dev, est, 0x4000, 32);
	if (err)
		return err;
	err = b43_phy_ac_rxiq_est(dev, est2, 0x4000, 32);
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

	b43_phy_ac_rxiq_coeffs(dev, 1, &new_comp);
	return 0;
}

/* The orchestrator: structure only, the stubs above are not filled in. */

/*
 * cal_type 0 or 2 selects RX-IQ, 1 or 2 the RC-cal LPF, which is a separate
 * path and not handled here. The skeleton follows
 * wlc_phy_cal_rxiq_nphy_rev3: quiesce, save the gain bank, then per core
 * {phy and radio setup; if IQ: gainctrl, tone, measure and solve, stop;
 * cleanup; RESET2RX}, then restore.
 */
int b43_phy_ac_rxiqcal(struct b43_wldev *dev, u8 cal_type)
{
	B43_AC_FN();
	u16 orig_bbcfg;
	u16 gain_save[3];
	unsigned int rx_core;
	u8 coremask = dev->phy.ac->coremask;

	/* Not filled in: no silicon access while the stubs are empty. */
	if (!B43_PHY_AC_RXIQCAL_REGMAP_FILLED)
		return -EOPNOTSUPP;

	/* Quiesce the PHY: clear 0x01 bit 15 and stay in carrier search. */
	orig_bbcfg = b43_phy_read_log(dev, B43_PHY_AC_BBCFG);
	b43_phy_mask(dev, B43_PHY_AC_BBCFG, (u16)~0x8000);
	dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_PHY_RUN;

	/* Save the RF-seq gain bank, N-PHY table RFSEQ offset 0x110.
	 * TODO: the AC-PHY table id and offset still come from a capture. */
	b43_actab_read_bulk(dev, 7 /* TODO id */, 0x110 /* TODO off */,
			    16, dev->phy.ac->num_cores, gain_save);

	for (rx_core = 0; rx_core < dev->phy.ac->num_cores; rx_core++) {
		bool active = (coremask >> rx_core) & 1;

		b43_phy_ac_rxcal_phy_setup(dev, rx_core);
		b43_phy_ac_rxcal_radio_setup(dev, rx_core);

		if (active && (cal_type == 0 || cal_type == 2)) {
			b43_phy_ac_rxcal_gainctrl(dev, rx_core);
			b43_phy_ac_tx_tone(dev, 4000000 /* TODO freq */,
					   0 /* TODO amp */);
			b43_phy_ac_rx_iq_comp_update(dev, (u8)(1 << rx_core));
			b43_phy_ac_stopplayback(dev);
		}

		/* cal_type 1 and 2, the RC-cal LPF, is a separate path and is
		 * not ported. */

		b43_phy_ac_rxcal_cleanup(dev, rx_core);
		b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RST2RX,
					     B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);
	}

	/* Restore the gain bank, BBCFG and CCA, then a closing RESET2RX. */
	b43_actab_write_bulk(dev, 7 /* TODO id */, 0x110 /* TODO off */,
			     16, dev->phy.ac->num_cores, gain_save);
	b43_phy_write(dev, B43_PHY_AC_BBCFG, orig_bbcfg);
	/* Mirror BBCFG bit 15 (PHY_RUN) and bit 14 (CCA_RESET) from the
	 * restored value: the write is atomic on both. */
	dev->phy.ac->status_mask = (dev->phy.ac->status_mask &
				    ~(B43_PHY_AC_STATE_PHY_RUN | B43_PHY_AC_STATE_CCA_RESET)) |
				   ((orig_bbcfg & 0x8000) ? B43_PHY_AC_STATE_PHY_RUN : 0) |
				   ((orig_bbcfg & 0x4000) ? B43_PHY_AC_STATE_CCA_RESET : 0);
	b43_phy_ac_reset_cca(dev);
	b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RST2RX,
				     B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);

	b43dbg(dev->wl, "phy-ac: rxiqcal skeleton (cal_type %u) -- register map not filled in\n",
	       (unsigned int)cal_type);
	return 0;
}
