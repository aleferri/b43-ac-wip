/* SPDX-License-Identifier: GPL-2.0
 * Test driver: build a minimal mock b43_wldev matching the D6220 board
 * (BCM4352 2x2, r2069 rev 4, coremask=0x3) and invoke one of the
 * scratch entry points. Emitted trace on stdout should match the
 * corresponding wl-diag capture after normalisation.
 *
 * Usage:
 *   ./rxiq_trace [flow] [board]
 *     flow  = rxiq_est_debug (default) | rxiqcal | op_init | set_channel
 *     board = d6220 (default) | agcombo
 *
 * set_channel drives the whole b43_phy_ac_op_switch_channel pipeline and
 * is the broadest flow (~22k HW ops on d6220 ch36); the others exercise
 * narrower slices. The full scratch driver (phy_ac.c + radio_2069.c +
 * rxiqcal_phy_ac.c + tables_phy_ac.c) links and runs; see the Makefile
 * SCRATCH_SRCS_FULL list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b43.h"
#include "phy_common.h"
#include "phy_ac.h"
#include "rxiqcal_phy_ac.h"
#include "test_harness.h"

extern enum nl80211_band b43_test_band;

/* Board profile. Extend when adding more targets (DSL-3580L is identical
 * to D6220 chip-wise; agcombo needs num_cores=3, coremask=0x7). */
struct board_profile {
	const char *name;
	u16 chip_id;
	u8  radio_rev;
	u16 radio_ver;
	u8  phy_rev;
	u8  num_cores;
	u8  coremask;
	u8  rxchain;
	u8  subband5gver;
	/* pa5ga per-core (3 core), 12 u16 = 4 gruppi (5g band) × 3 (a1,b0,b1).
	 * Dal file NVRAM del router, keys pa5ga0/pa5ga1/pa5ga2. Se tutti 0,
	 * mount_board lascia pa5ga = 0 (il caller cade sui pwrdet_def).
	 */
	u16 pa5ga[3][12];
	/* maxp5ga per-core (3 core), 4 sub-band u8. NVRAM keys maxp5ga{0,1,2}.
	 * Drives the per-core max TX index (maxp5ga[grp] - margin). */
	u8 maxp5ga[3][4];
	/* rxgains_5gl per-core (3 core). NVRAM keys rxgains5gelnagaina{0,1,2}
	 * e rxgains5gtrisoa{0,1,2}. Usati per computare hdr = (elnagain+3)<<1
	 * e gainctx = ((triso+4)<<1)+2 nel body Phase 3 di noise-shaping. */
	u8 rxgains_5gl_elnagain[3];
	u8 rxgains_5gl_triso[3];
};

/*
 * D6220 SPROM values from router-data/d6220/wl1_nvram.txt:
 *   subband5gver=0x4
 *   pa5ga0=0xff33,0x175b,0xfd32,0xff23,0x1672,0xfd36,0xff25,0x161d,0xfd4b,0xff2d,0x16c3,0xfd3b
 *   pa5ga1=0xff2e,0x1702,0xfd39,0xff23,0x16ae,0xfd30,0xff44,0x180e,0xfd36,0xff36,0x16b0,0xfd55
 *   pa5ga2=0xff6e,0x15dc,0xfd61,0xff59,0x15be,0xfd49,0xff5d,0x15f7,0xfd4a,0xff3d,0x1560,0xfd33
 * Per ch36 (5180 MHz) con subband5gver=4 → grp=0, a1/b0/b1 = pa5ga0[0..2].
 */
static const struct board_profile PROFILE_D6220 = {
	.name = "d6220", .chip_id = 0x4352, .radio_rev = 4,
	.radio_ver = 0x2069, .phy_rev = 1,
	.num_cores = 3, .coremask = 0x3, .rxchain = 3,
	.subband5gver = 0x4,
	.pa5ga = {
		{ 0xff33, 0x175b, 0xfd32, 0xff23, 0x1672, 0xfd36,
		  0xff25, 0x161d, 0xfd4b, 0xff2d, 0x16c3, 0xfd3b },
		{ 0xff2e, 0x1702, 0xfd39, 0xff23, 0x16ae, 0xfd30,
		  0xff44, 0x180e, 0xfd36, 0xff36, 0x16b0, 0xfd55 },
		{ 0xff6e, 0x15dc, 0xfd61, 0xff59, 0x15be, 0xfd49,
		  0xff5d, 0x15f7, 0xfd4a, 0xff3d, 0x1560, 0xfd33 },
	},
	/* rxgains5gelnagaina{0,1,2}=3, rxgains5gtrisoa{0,1,2}=6 (NVRAM d6220). */
	.rxgains_5gl_elnagain = { 3, 3, 3 },
	.rxgains_5gl_triso    = { 6, 6, 6 },
	/* maxp5ga{0,1,2} (NVRAM d6220). */
	.maxp5ga = {
		{ 72, 70, 86, 0 },
		{ 72, 70, 86, 0 },
		{ 76, 76, 76, 76 },
	},
};

static const struct board_profile PROFILE_AGCOMBO = {
	.name = "agcombo", .chip_id = 0x4360, .radio_rev = 4,
	.radio_ver = 0x2069, .phy_rev = 1,
	.num_cores = 3, .coremask = 0x7, .rxchain = 7,
	/* Same 5gl values as d6220 (NVRAM agcombo). */
	.rxgains_5gl_elnagain = { 3, 3, 3 },
	.rxgains_5gl_triso    = { 6, 6, 6 },
	/* maxp5ga{0,1,2} (NVRAM agcombo). */
	.maxp5ga = {
		{ 74, 74, 82, 82 },
		{ 74, 74, 82, 82 },
		{ 74, 74, 82, 82 },
	},
};

/* One-shot mock storage. Lives for the whole run. */
static struct b43_phy_ac       g_ac;
static struct b43_wl           g_wl;
static struct ieee80211_hw     g_hw;
static struct ieee80211_channel g_chan;
static struct ssb_sprom        g_sprom;
static struct bcma_bus         g_bcma_bus;
static struct bcma_device      g_bcma_dev;
static struct b43_bus_dev      g_bus_dev;
static struct b43_wldev        g_wldev;

static void mount_board(const struct board_profile *p)
{
	memset(&g_ac, 0, sizeof(g_ac));
	g_ac.num_cores  = p->num_cores;
	g_ac.coremask   = p->coremask;
	/*
	 * dacbuf_cap: chip vero d6220 ha RCCAL_G=0x0009 → dacbuf_cap=0.
	 * Vedi commento in analog_on_reset dacbuf loop.
	 */
	g_ac.dacbuf_cap = 0x00;
	/*
	 * lpf_cap0/1: valore prodotto da b43_r2069_rccal_lpf sul chip vero.
	 * Formula: cap = (u8)(((f - e) * 193) >> 8) con e/f letti dai
	 * registri radio R2069_RCCAL_E (0x0414) / R2069_RCCAL_F (0x0415).
	 *
	 * Valori reali osservati sul d6220 (dmesg boot):
	 *   E = 0x0ac7  (R2069_RCCAL_E, comparator A pass 0)
	 *   F = 0x0baa  (R2069_RCCAL_F, comparator B pass 0)
	 *   → cap = ((0x0baa - 0x0ac7) * 193) >> 8 = 227 * 193 / 256 = 0xab
	 *
	 * Il boot log ha mostrato a volte 0xaa: jitter LSB della misura RCcal.
	 * Il campo utile iniettato nelle celle table 7 sembra essere il top-5-
	 * bit (0xa8), ma l'interazione con il pre-state della cella non è ancora
	 * chiara — servono i log [TXLPFLOG] dal boot per invertire la formula.
	 */
	g_ac.lpf_cap0   = 0xab;
	g_ac.lpf_cap1   = 0xab;

	memset(&g_sprom, 0, sizeof(g_sprom));
	g_sprom.rxchain = p->rxchain;
	g_sprom.subband = 0;
	g_sprom.subband5gver = p->subband5gver;
	memcpy(g_sprom.rxgains_5gl.elnagain, p->rxgains_5gl_elnagain,
	       sizeof(g_sprom.rxgains_5gl.elnagain));
	memcpy(g_sprom.rxgains_5gl.triso, p->rxgains_5gl_triso,
	       sizeof(g_sprom.rxgains_5gl.triso));
	for (unsigned int c = 0; c < 3; c++)
		memcpy(g_sprom.core_pwr_info[c].pa5ga, p->pa5ga[c],
		       sizeof(g_sprom.core_pwr_info[c].pa5ga));
	for (unsigned int c = 0; c < 3; c++)
		memcpy(g_sprom.core_pwr_info[c].maxp5ga, p->maxp5ga[c],
		       sizeof(g_sprom.core_pwr_info[c].maxp5ga));

	g_bcma_dev.bus = &g_bcma_bus;
	g_bus_dev.bus_type = B43_BUS_BCMA;
	g_bus_dev.chip_id  = p->chip_id;
	g_bus_dev.bus_sprom = &g_sprom;
	g_bus_dev.bdev     = &g_bcma_dev;

	g_chan.band = NL80211_BAND_5GHZ;
	g_chan.hw_value = 36;
	g_chan.center_freq = 5180;
	g_hw.conf.chandef.chan  = &g_chan;
	g_hw.conf.chandef.width = NL80211_CHAN_WIDTH_20;
	g_wl.hw = &g_hw;

	memset(&g_wldev, 0, sizeof(g_wldev));
	g_wldev.dev = &g_bus_dev;
	g_wldev.wl  = &g_wl;
	g_wldev.phy.rev        = p->phy_rev;
	g_wldev.phy.radio_ver  = p->radio_ver;
	g_wldev.phy.radio_rev  = p->radio_rev;
	g_wldev.phy.dacbuf_cap = g_ac.dacbuf_cap;
	g_wldev.phy.lpf_cap    = g_ac.lpf_cap0;
	g_wldev.phy.ac         = &g_ac;

	b43_test_band = NL80211_BAND_5GHZ;

	/* Preconditions the rxiqcal REQUIRE gates want to see. */
	g_ac.status_mask = B43_PHY_AC_STATE_RX_WAITED |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS;
}

/*
 * Read plans specific to the rxiq flow on any board with the r2069-rev4
 * PHY. The correlator command register 0x0270 is polled by
 * b43_phy_ac_rxiq_start after every SET-START: the code loops reading
 * until bit 0 clears. We script three "busy" (bit0=1) reads then a
 * "done" (bit0=0), repeated enough times to cover all estimator
 * invocations in Phase 1 (4 estimators). Phase 2/3 add more estimators;
 * extend the array to (num_estimators * 4) if you enable those flows.
 *
 * The values below reproduce the vendor pattern where an estimator
 * takes ~3 poll iterations to complete on the wl-diag capture. The
 * exact count is not verifiable (val=UNDEFINED in the trace), but the
 * op-order between estimators is: N reads showing bit0=1, one read
 * showing bit0=0, break. As long as the last-read bit is clear the
 * flow proceeds; the diff should tolerate poll-count differences via
 * compare.py's --squash-poll.
 */
static const u16 rxiq_poll_0x0270[] = {
	/* est#1 */ 0x0001, 0x0001, 0x0001, 0x0000,
	/* est#2 */ 0x0001, 0x0001, 0x0001, 0x0000,
	/* est#3 */ 0x0001, 0x0001, 0x0001, 0x0000,
	/* est#4 */ 0x0001, 0x0001, 0x0001, 0x0000,
	/* Phase 2/3 (only used if rxiqcal REGMAP_FILLED is 1) */
	/* est#5, est#5b, est#5c, est#6, est#6b, est#6c */
	0x0001, 0x0001, 0x0001, 0x0000,
	0x0001, 0x0001, 0x0001, 0x0000,
	0x0001, 0x0001, 0x0001, 0x0000,
	0x0001, 0x0001, 0x0001, 0x0000,
	0x0001, 0x0001, 0x0001, 0x0000,
	0x0001, 0x0001, 0x0001, 0x0000,
};

/*
 * Table-access lock probe. Every b43_actab_* prologue reads 0x019e to
 * observe the "table write in progress" bit before touching 0x000d..
 * 0x000f. In real HW the bit is momentarily set by the previous access
 * and reads back clear once the internal state machine has advanced.
 * We script a single "already clear" read; any subsequent poll after a
 * mid-sequence write is served the same value.
 */
static const u16 tblacc_0x019e[] = {
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

static void register_rxiq_read_plans(void)
{
	b43_test_plan_phy_reads(0x0270, rxiq_poll_0x0270,
				(int)(sizeof(rxiq_poll_0x0270) / sizeof(u16)));
	b43_test_plan_phy_reads(0x019e, tblacc_0x019e,
				(int)(sizeof(tblacc_0x019e) / sizeof(u16)));
}

/*
 * Board-specific read seeds. The wl-diag capture logs reads as val=UNDEFINED,
 * so read-return values come from each board's reset defaults / NVRAM, not the
 * trace. Most radio POR defaults seeded in main() are r2069-rev4 properties
 * (identical on 0x4352 and 0x4360), but a few reads differ per chip and must be
 * overridden here so an agcombo run is not served d6220 read values. Called
 * after the shared seeds, so these win. Add agcombo values here as they are
 * derived from the agcombo dumps (RCCAL E/F 0x0414/0x0415 still fall back to
 * the shared d6220 seed — unverified for agcombo).
 */
static void register_board_read_plans(const struct board_profile *p)
{
	if (!strcmp(p->name, "agcombo")) {
		/* CLASSCTL 0x0140: BCM4360 powers this register with bit 0x0800
		 * set at reset (0x4352 leaves it clear). channel_switch_prep
		 * preserves the peeked bit: (cur & 0x0800) | 0x05f4 -> 0x0df4 on
		 * agcombo, 0x05f4 on d6220. Both agcombo captures confirm 0x0df4. */
		b43_test_mirror_phy_set(0x0140, 0x0800);
		/* Radio 0x0433 (core-2 0x0033 shadow): agcombo reads 0x0161 like
		 * cores 0/1, not the d6220 core-2 default 0x0160 seeded in main(). */
		b43_test_mirror_radio_set(0x0433, 0x0161);
		/* Core-2 shadows of seeds already present for cores 0/1 in
		 * main(): 0x041a pre-state bit 0x0004 (agcombo #60570 folds it
		 * into WR 0x0014) and 0x0521 AFE_CAL_CLK bit 0x2000 (agcombo
		 * #63946 RMW yields 0x2000/0x3000). */
		b43_test_mirror_radio_set(0x041a, 0x0004);
		b43_test_mirror_radio_set(0x0521, 0x2000);
	}
}

int main(int argc, char **argv)
{
	const char *flow  = (argc > 1) ? argv[1] : "rxiq_est_debug";
	const char *board = (argc > 2) ? argv[2] : "d6220";

	const struct board_profile *p = &PROFILE_D6220;
	if (!strcmp(board, "agcombo")) p = &PROFILE_AGCOMBO;

	fprintf(stderr, "test: board=%s flow=%s\n", p->name, flow);
	mount_board(p);
	b43_test_plans_reset();
	b43_test_trace_to(stdout);

	/*
	 * Pre-populate radio-mirror slots set by earlier init flows we don't
	 * re-execute here. The RCCAL comparators (R2069_RCCAL_E/F) are
	 * written by the RC-cal engine during b43_radio_2069_init and later
	 * read by b43_r2069_rccal_lpf to derive dev->phy.ac->lpf_cap0/1.
	 * Real d6220 boot: E=0x0ac7, F=0x0baa → cap=0xab.
	 */
	b43_test_mirror_radio_set(0x0414, 0x0ac7);  /* R2069_RCCAL_E */
	b43_test_mirror_radio_set(0x0415, 0x0baa);  /* R2069_RCCAL_F */

	/*
	 * R2069 0x040b (probabilmente un enable/misc control): il vendor
	 * lo trova con bit 0 set al momento di b43_phy_ac_radio_percore_setup_1
	 * (post rfseq_tbl_init). d6220 ch36 vendor: RAD.RD → 0x0169
	 * (o simile con bit 0 = 1), poi maskset clear bit 0 → RAD.WR 0x0168.
	 * Setto lo stesso pre-state osservato per riprodurre.
	 */
	b43_test_mirror_radio_set(0x040b, 0x0169);

	/*
	 * R2069 0x001a/0x021a (per-core radio control): hardware default =
	 * 0x0004 (bit 2 set). Il vendor legge questo valore all'inizio del
	 * b43_phy_ac_radio_percore_setup_1 (d6220 ch36 #34708-#34719 per
	 * core 0, #34729-#34740 per core 1). Dedotto dai bit "out-of-mask"
	 * dei RAD.WR finali dei maskset osservati.
	 */
	b43_test_mirror_radio_set(0x001a, 0x0004);
	b43_test_mirror_radio_set(0x021a, 0x0004);

	/*
	 * R2069 0x001e/0x021e = 0x0014 (per-core radio, HW default board).
	 * Il vendor arriva a #41190 (B2f di rxiqcal_apply) con 0x0014 già
	 * presente sul registro; il porting non tocca 0x001e prima, quindi
	 * il pre-seed è necessario per il maskset ~0x0004, 0 di B2f che
	 * produce WR = 0x0010 (= 0x0014 & 0xfffb).
	 */
	b43_test_mirror_radio_set(0x001e, 0x0014);
	b43_test_mirror_radio_set(0x021e, 0x0014);

	/*
	 * R2069 0x0033/0x0233/0x0433 (per-core radio 0x0033 shadow):
	 * hardware default = 0x0161 per core 0/1, 0x0160 per core 2
	 * (d6220 solo, agcombo mostra 0x0161 per core 2 pure). Dedotto
	 * dai RAD.WR finali del maskset in #34760/#34765/#34770.
	 * LSB bit 0 diverso per core 2 su d6220 suggerisce silicon-fuse
	 * o factory default per core inattivi.
	 */
	b43_test_mirror_radio_set(0x0033, 0x0161);
	b43_test_mirror_radio_set(0x0233, 0x0161);
	b43_test_mirror_radio_set(0x0433, 0x0160);

	/*
	 * Radio afecal registers per-core (0x06ea/0x08ea/0x0aea + stride):
	 * il vendor a #38081-#38083 (core 0), #38118-#38120 (core 1), #...
	 * emette RAD.MOD ~0x0080, 0x0080 e WR val=0x00bf. Il pre-state HW
	 * ha bit 0-5 set (0x003f), bit 7 clear.
	 *
	 * Radio afecal 0x0121/0x0321 (per-core): HW pre-state 0x2000 (bit 13 set).
	 */
	b43_test_mirror_radio_set(0x06ea, 0x003f);
	b43_test_mirror_radio_set(0x08ea, 0x003f);
	b43_test_mirror_radio_set(0x0aea, 0x003f);
	b43_test_mirror_radio_set(0x0121, 0x2000);
	b43_test_mirror_radio_set(0x0321, 0x2000);

	/*
	 * R2069 0x004e/0x024e (RF gain-block per-core, bit 15 set nel HW POR).
	 * Vendor #38385-#38387 (core 0), #38414-#38416 (core 1): RAD.MOD ~0x0e00, 0
	 * e WR val=0x8000. Il pre-state HW ha bit 15 set (0x8000).
	 */
	b43_test_mirror_radio_set(0x004e, 0x8000);
	b43_test_mirror_radio_set(0x024e, 0x8000);

	/*
	 * Radio pre-state per rxcal_radio_setup (vendor #39641-#39708).
	 * Derivati dalle triplette MOD+RD+WR: il WR emette (cur & mask) | set.
	 *
	 * Solo i registri SENZA op vendor precedenti hanno pre-seed statico
	 * qui — 0x0017/0x0217 hanno op RAD.MOD/WR a #34720-#34722 e #38296-
	 * #38298 (val=0x0000) che i pre-seed romperebbero. Il valore 0x0010
	 * osservato a #39657 (per far uscire WR=0x0011 dopo set bit 0) arriva
	 * da un meccanismo HW auto-set tra #38298 e #39657, non da op
	 * tracciate. Sul test framework quel WR emetterà 0x0001 invece di
	 * 0x0011 — divergenza inevitabile senza modellare il side-effect HW.
	 */
	b43_test_mirror_radio_set(0x0161, 0x0100);
	b43_test_mirror_radio_set(0x0361, 0x0100);
	b43_test_mirror_radio_set(0x0024, 0x0003);
	b43_test_mirror_radio_set(0x0224, 0x0003);

	/* Board-specific read overrides (win over the shared seeds above). */
	register_board_read_plans(p);

	if (!strcmp(flow, "rxiq_est_debug")) {
		register_rxiq_read_plans();
		b43_phy_ac_rxiq_est_debug(&g_wldev);
	} else if (!strcmp(flow, "rxiq_comp")) {
		/*
		 * Vettori misura->coeff dalla cattura agcombo
		 * rescan-to-bss-ch36 (retval): per core, round 1 e round 2
		 * degli accumulatori (it4-it9 della finestra #29845-#31965).
		 * Output atteso (vendor #32043-#32048):
		 *   0x06a0=0x03fd 0x06a1=0x006e
		 *   0x08a0=0x0052 0x08a1=0x003c
		 *   0x0aa0=0x0092 0x0aa1=0x005c
		 */
		static const u16 acc_06c0[] = { 0x2b25, 0x8c91 };
		static const u16 acc_06c1[] = { 0xfffb, 0x0008 };
		static const u16 acc_06c2[] = { 0x0df4, 0x1d2a };
		static const u16 acc_06c3[] = { 0x02d7, 0x02d8 };
		static const u16 acc_06c4[] = { 0xceef, 0x6971 };
		static const u16 acc_06c5[] = { 0x037a, 0x037d };
		static const u16 acc_08c0[] = { 0x089d, 0x3fa2 };
		static const u16 acc_08c1[] = { 0xffc1, 0xffc6 };
		static const u16 acc_08c2[] = { 0xbee8, 0x721e };
		static const u16 acc_08c3[] = { 0x02f3, 0x02f1 };
		static const u16 acc_08c4[] = { 0xc11c, 0xbd95 };
		static const u16 acc_08c5[] = { 0x0352, 0x0350 };
		static const u16 acc_0ac0[] = { 0x3b1d, 0xf6da };
		static const u16 acc_0ac1[] = { 0xffb0, 0xffb4 };
		static const u16 acc_0ac2[] = { 0x3f95, 0x8e52 };
		static const u16 acc_0ac3[] = { 0x021e, 0x021e };
		static const u16 acc_0ac4[] = { 0x453a, 0x7f05 };
		static const u16 acc_0ac5[] = { 0x0290, 0x028e };
		static const u16 poll_done[] = { 0x0000, 0x0000 };

		b43_test_plan_phy_reads(0x0270, poll_done, 2);
		b43_test_plan_phy_reads(0x06c0, acc_06c0, 2);
		b43_test_plan_phy_reads(0x06c1, acc_06c1, 2);
		b43_test_plan_phy_reads(0x06c2, acc_06c2, 2);
		b43_test_plan_phy_reads(0x06c3, acc_06c3, 2);
		b43_test_plan_phy_reads(0x06c4, acc_06c4, 2);
		b43_test_plan_phy_reads(0x06c5, acc_06c5, 2);
		b43_test_plan_phy_reads(0x08c0, acc_08c0, 2);
		b43_test_plan_phy_reads(0x08c1, acc_08c1, 2);
		b43_test_plan_phy_reads(0x08c2, acc_08c2, 2);
		b43_test_plan_phy_reads(0x08c3, acc_08c3, 2);
		b43_test_plan_phy_reads(0x08c4, acc_08c4, 2);
		b43_test_plan_phy_reads(0x08c5, acc_08c5, 2);
		b43_test_plan_phy_reads(0x0ac0, acc_0ac0, 2);
		b43_test_plan_phy_reads(0x0ac1, acc_0ac1, 2);
		b43_test_plan_phy_reads(0x0ac2, acc_0ac2, 2);
		b43_test_plan_phy_reads(0x0ac3, acc_0ac3, 2);
		b43_test_plan_phy_reads(0x0ac4, acc_0ac4, 2);
		b43_test_plan_phy_reads(0x0ac5, acc_0ac5, 2);

		int r = b43_phy_ac_rx_iq_comp_update(&g_wldev, 0x07);
		fprintf(stderr, "test: rx_iq_comp_update returned %d\n", r);
	} else if (!strcmp(flow, "rxiqcal")) {
		register_rxiq_read_plans();
		int r = b43_phy_ac_rxiqcal(&g_wldev, 0);
		fprintf(stderr, "test: rxiqcal returned %d\n", r);
	} else if (!strcmp(flow, "op_init")) {
		/*
		 * On the vendor attach-to-bss trace, num_cores is already
		 * cached from a prior fresh attach, so probe_cores hits its
		 * early-return path and emits no PHY.RD 0x000b. We keep the
		 * mount_board() defaults (num_cores=3, coremask=3) to mirror
		 * that state; a fresh-attach flow would clear them here to
		 * exercise probe_cores' PHY read.
		 */
		g_ac.status_mask = 0;	/* op_init has no REQUIRE gates */
		/*
		 * pre_init_frontend RAD 0x0X33 (#51692/51697/51702): nibble
		 * 0xf000 -> 0x4000 over HW background 0x0060 gives 0x4060.
		 */
		{
			static const u16 r33[] = { 0x4060 };
			u16 co;
			for (co = 0; co <= 0x400; co += 0x200)
				b43_test_plan_radio_reads(0x0033 + co, r33,
							  ARRAY_SIZE(r33));
		}
		int r = b43_phyops_ac.init(&g_wldev);
		fprintf(stderr, "test: op_init returned %d\n", r);
	} else if (!strcmp(flow, "rfkill")) {
		/*
		 * software_rfkill(false): radio bring-up (2069 init/pwron/rccal),
		 * afe_lpf_stage, GPIO frontend, PA bias and the radio-ON front-end
		 * switch. Runs before op_init in the real driver; here in isolation.
		 */
		g_ac.status_mask = 0;

		/*
		 * rccal done-bit poll (R2069_RCCAL_STAT 0x0413, bit 4). Three
		 * passes; vendor ch36 polls 2, 6, 6 times before done (ep
		 * 32611-12, 32640-45, 32708-13). Done value = 0x0010 on the last
		 * read of each run.
		 */
		{
			static const u16 rccal_stat[] = {
				0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0010,
				0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0010,
				0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0010,
			};
			b43_test_plan_radio_reads(0x0413, rccal_stat,
						  ARRAY_SIZE(rccal_stat));
		}

		/*
		 * 0x0407 bit 7 (0x0080) reads back clear after the prefregs write
		 * of 0x8382, so the set-bit1 RMW writes 0x8302 (vendor #51319-21).
		 * The write-mirror can't model the self-clear.
		 */
		{
			static const u16 rad_0407[] = { 0x8302 };
			b43_test_plan_radio_reads(0x0407, rad_0407,
						  ARRAY_SIZE(rad_0407));
		}
		/*
		 * 0x040c reads back 0x0200 (bit 9 set) at the epilogue bit-4
		 * toggle (vendor: mask ->0x0200, set ->0x0210); the bit is a HW
		 * default the write-mirror doesn't hold.
		 */
		{
			static const u16 rad_040c[] = { 0x0200, 0x0201 };
			b43_test_plan_radio_reads(0x040c, rad_040c,
						  ARRAY_SIZE(rad_040c));
		}
		/*
		 * EN2 / power-kick / RCCAL cfg+kick registers hold silicon
		 * background bits that the write-mirror (starting at 0)
		 * cannot reproduce. Seed each read with the vendor's post-RMW
		 * value: for a maskset read==WR yields WR, since the vendor WR
		 * already carries the set bits under the mask. Ordered per
		 * read; vendor down-to-bss-up #51262-.
		 */
		{
			static const u16 rad_08ed[] = { 0x4124, 0x4124, 0x4524 };
			static const u16 rad_040b[] = {
				0x0000, 0x0001,		/* kick clear, set */
				0x0168, 0x0168,		/* pon0/pon1 readback */
				0x0168,			/* final mask reads risen reg */
			};
			static const u16 rad_0410[] = {
				0x1f80, 0x1f80, 0x1f80, 0x1f81, 0x1f80,
				0x0f80, 0x0f90, 0x0f90, 0x0f91, 0x0f90,
				0x0f90, 0x0f88, 0x0f88, 0x0f89,
			};
			static const u16 rad_0411[] = {
				0x1c54, 0x1c55, 0x1c54, 0x7054, 0x7055,
				0x7054, 0x4054, 0x4055, 0x4054,
			};
			/* measured RC code read by apply_code (pass 1) then the
			 * dacbuf read (pass 2); low 5 bits = 0x09 (vendor #51517). */
			static const u16 rad_0416[] = { 0x0009, 0x0009 };
			b43_test_plan_radio_reads(0x08ed, rad_08ed,
						  ARRAY_SIZE(rad_08ed));
			b43_test_plan_radio_reads(0x040b, rad_040b,
						  ARRAY_SIZE(rad_040b));
			b43_test_plan_radio_reads(0x0410, rad_0410,
						  ARRAY_SIZE(rad_0410));
			b43_test_plan_radio_reads(0x0411, rad_0411,
						  ARRAY_SIZE(rad_0411));
			b43_test_plan_radio_reads(0x0416, rad_0416,
						  ARRAY_SIZE(rad_0416));
			/*
			 * afe_lpf_stage per-core (after rccal, #51609-): 0x0045
			 * set-0x0080 over HW background 0x3000; 0x0049 six clears
			 * over background 0x0030 (no clr49 touches bits 4:5, so the
			 * mirror carries it after the first seeded read).
			 */
			{
				static const u16 r45[] = { 0x3080 };
				static const u16 r49[] = { 0x0030 };
				u16 co;
				for (co = 0; co <= 0x400; co += 0x200) {
					b43_test_plan_radio_reads(0x0045 + co,
							r45, ARRAY_SIZE(r45));
					b43_test_plan_radio_reads(0x0049 + co,
							r49, ARRAY_SIZE(r49));
				}
			}
		}

		b43_phyops_ac.software_rfkill(&g_wldev, false);
		fprintf(stderr, "test: software_rfkill(false) done\n");
	} else if (!strcmp(flow, "set_channel")) {
		/*
		 * Stato entrante a set_channel (post-op_init): MAC sospeso,
		 * classifier/clip-detector state non toccato (RX_ANY = 0,
		 * CLIP_ALL_DIS = 0). set_channel imposterà RX_WAITED e
		 * CLIP_ALL_DIS via classifier()/clip_det() prima delle
		 * sub-routine che REQUIRE quello stato.
		 *
		 * op_init non è re-run qui: il suo trace è validato dal
		 * flow separato; eseguirlo back-to-back double-conta le
		 * register writes.
		 */
		g_ac.status_mask = 0;

		/*
		 * NB: nessun read plan per 0x019e (table-write gate) in questo
		 * flow. Il mirror riflette correttamente lo stato del gate:
		 * tbl_write_lock scrive 0x0002, i sub-lock rileggono 0x0002 e
		 * al tbl_write_unlock finale il saved contiene lo stato del
		 * lock precedente (0 al primo lock, 0x0002 ai nested). Un plan
		 * a valore fisso 0x0000 rompeva questa sequenza.
		 */

		/*
		 * rxiq_est_debug is invoked from set_channel's tail; reuse
		 * the same 0x0270 completion plan as the standalone flow.
		 */
		b43_test_plan_phy_reads(0x0270, rxiq_poll_0x0270,
					(int)(sizeof(rxiq_poll_0x0270) / sizeof(u16)));

		/*
		 * Table-7 cell pre-state per il RMW di analog_on_reset. I
		 * b43_actab_read_bulk emettono phy_read(0x000f) dopo aver
		 * scritto TABLE_ID/TABLE_OFFSET; senza plan il mirror ritorna
		 * l'ultimo write generico a 0x000f che non ha significato per
		 * la cella indirizzata.
		 *
		 * Approssimazione: come pre-state usiamo il VALORE VENDOR
		 * SCRITTO (assumendo che il chip faccia RMW identity-like col
		 * pre-state già impostato dall'init). Se la formula del scratch
		 * fosse identity con questi input, matcherebbe. In pratica non
		 * lo è (vedi TODO in README), ma il flow avanza e i log
		 * [TXLPFLOG] dal chip vero ci daranno lo_read/hi_read/cur reali.
		 *
		 * Ordine di consumo del plan (per il call site channel_setup):
		 *  - TX-LPF: core 0 stage 0..8 (18 read: lo,hi × 9), poi core 1
		 *    (altre 18) = 36 read totali.
		 *  - Dacbuf: core 0 stage 0..8 (9 read), core 1 (9) = 18 read.
		 *  - RX-LPF: core 0 stage 0..2 (6 read: lo,hi × 3), core 1 (6)
		 *    = 12 read totali.
		 * Totale: 66 read.
		 */
		{
			static const u16 t7_pre_seed[] = {
				/* TX-LPF core 0: stage 0..8, (lo, hi) */
				0x50db, 0x0151,  0x50db, 0x0151,  0x50db, 0x0151,
				0x5123, 0x0151,  0x5123, 0x0151,  0x5123, 0x0151,
				0x516b, 0x0151,  0x516b, 0x0151,  0x50db, 0x0151,
				/* TX-LPF core 1 */
				0x50db, 0x0151,  0x50db, 0x0151,  0x50db, 0x0151,
				0x5123, 0x0151,  0x5123, 0x0151,  0x5123, 0x0151,
				0x516b, 0x0151,  0x516b, 0x0151,  0x50db, 0x0151,
				/* Dacbuf core 0: stage 0..8 (add={b,b,c,c,e,e,f,f,a}).
				 * Stage 0-1 leggono/scrivono stessa cella 0x3fb; il
				 * pre-state esposto qui è quello che il chip ha PRIMA
				 * del primo RMW di stage. */
				0x0b2e, 0x0b2e, 0x0b2e, 0x0b2e, 0x0b2e,
				0x0b2e, 0x0b2e, 0x0b2e, 0x002e,
				/* Dacbuf core 1 */
				0x0b2e, 0x0b2e, 0x0b2e, 0x0b2e, 0x0b2e,
				0x0b2e, 0x0b2e, 0x0b2e, 0x002e,
				/* RX-LPF, ordine vendor stage-then-core.
				 * stage 0: core 0 lo/hi, core 1 lo/hi
				 * stage 1: core 0 lo/hi, core 1 lo/hi
				 * stage 2: core 0 lo/hi, core 1 lo/hi */
				0x2440, 0x0150,  0x2440, 0x0150,
				0x2349, 0x0150,  0x2349, 0x0150,
				0x2352, 0x0150,  0x2352, 0x0150,
			};
			b43_test_plan_phy_reads(0x000f, t7_pre_seed,
				(int)(sizeof(t7_pre_seed) / sizeof(u16)));
		}

		/*
		 * post_rfseq_misc_setup: run_rfseq_cmd fa poll di 0x0403 fino a
		 * done bit set. Vendor d6220 ch36: prima chiamata 2 peek (0 poi
		 * 1 = done), seconda chiamata 1 peek (già 1 = done). Plan
		 * riproduce il pattern.
		 */
		{
			static const u16 rfseq_done_poll[] = {
				0x0000, 0x0001,  /* prima chiamata: 0, poi done */
				0x0001,          /* seconda chiamata: done subito */
			};
			b43_test_plan_phy_reads(0x0403, rfseq_done_poll,
				(int)(sizeof(rfseq_done_poll) / sizeof(u16)));
		}

		/*
		 * PHY pre-seed: 0x0401 (RF_SEQ_MODE) al boot ha bit 0-2 = coremask
		 * e bit 12-14 = coremask<<12. Il vendor legge questi bit come
		 * saved_401 dentro rxcore_setstate e li ripristina alla fine
		 * (vendor #34553: MOD val=0x0003 mask=0x0007; #34554: MOD
		 * val=0x7000 mask=0x7000). Il test framework parte da mirror=0
		 * quindi il restore fallirebbe con val=0. Pre-seed = 0x7003 per
		 * D6220 (coremask=0x03).
		 */
		b43_test_mirror_phy_set(0x0401, 0x7003);

		/*
		 * Radio-side pre-existing state per il primo maskset di
		 * b43_radio_2069_channel_setup. Su tutte le catture vendor
		 * (d6220 e agcombo) i registri hanno bit già settati prima
		 * del channel switch:
		 *   0x0645: bit 7 (0x0080) — bias/enable stabilito da un
		 *           init precedente (mode_init/pwron).
		 *   0x08c9: bit 1-2 (0x0006) — configurazione PLL persistente
		 *           tra i channel switch.
		 * Il read plan riporta questi valori così il peek del
		 * radio_maskset produce lo stesso RAD.WR val=... del vendor.
		 */
		{
			static const u16 rad_0645[] = { 0x0080 };
			static const u16 rad_08c9[] = { 0x0006 };
			static const u16 rad_090b[] = { 0x0100 };	/* PLL lock immediato */
			/*
			 * Radio bias registers per-core (0x0045/0x0245/0x0445)
			 * hanno bit non azzerati prima del radio_set(0x0080) in
			 * afe_lpf_stage loop2. Vendor #33037/33058/... scrive
			 * val=0x70bf → cur = 0x70bf & ~0x0080 = 0x703f. Presente
			 * in tutte le catture d6220 e agcombo.
			 *
			 * Per 0x0445 (core 2 inattivo su d6220 coremask=0x03) la
			 * seconda lettura (durante post_noise_shaping_core_transition
			 * a #38058) restituisce 0x7080 nel vendor — bit 0-6 clear a
			 * eccezione del bit 7 — così il maskset ~0x0300, 0x0300
			 * produce 0x7380 (invece di 0x73bf per i core attivi). Il
			 * meccanismo interno del blob che porta il registro a 0x7080
			 * per il core inattivo non è visibile nel trace (nessuna WR
			 * intermedia); simuliamo con un secondo valore nel read plan.
			 */
			static const u16 rad_0045[] = { 0x703f };
			static const u16 rad_0245[] = { 0x703f };
			static const u16 rad_0445[] = { 0x703f, 0x7080 };
			b43_test_plan_radio_reads(0x0645, rad_0645, 1);
			b43_test_plan_radio_reads(0x08c9, rad_08c9, 1);
			b43_test_plan_radio_reads(0x090b, rad_090b, 1);
			b43_test_plan_radio_reads(0x0045, rad_0045, 1);
			b43_test_plan_radio_reads(0x0245, rad_0245, 1);
			b43_test_plan_radio_reads(0x0445, rad_0445, 2);

			/*
			 * 0x0017 / 0x0217 (radio bit 4 auto-set/clear HW).
			 *
			 * Fase set_channel matched (rxcal_radio_setup):
			 *   [0] #34721/34742: bit 4=0 (WR=0x0000 precedente)
			 *   [1] #38297/38318: idem
			 *   [2] #39644/39678: bit 4 auto-SET → 0x0010
			 *   [3] #39658/39692: idem 0x0010
			 *   [4] #39661/39695: 0x0011 (dopo set bit 0 al [3])
			 *
			 * Fase post_cal_finalize iter 2 A3 (#39988/40009):
			 *   [5] Post rxcal_radio_cleanup WR=0x0011 a #39930/37, ma HW
			 *       auto-BOUNCE a 0x0002 tra #39930/37 e #39988/40009
			 *       (bit 4 clear + bit 1 set: side-effect non tracciato,
			 *       probabile "cal complete" flag HW).
			 *
			 * Iter 3 (#40584+) legge dal mirror stabile a 0 (slot [6] = 0
			 * come guard; se rimosso, il mirror fornisce comunque 0).
			 *
			 * Fase rxiqcal_finalize radio setup (#52737+):
			 *   [7] Baseline RAD.RD 0x0017/0x0217 nel loop — non usato per
			 *       calcoli (val letto solo per peek).
			 *   [8] MOD 0x0017 mask=0x0001 val=0x0001 → RD interno = 0x0010
			 *       → WR = 0x0011 (HW auto-set bit 4 di nuovo dopo iter 3).
			 *   [9] MOD 0x0017 mask=0x0002 val=0x0000 → RD interno = 0x0011
			 *       → WR = 0x0011 (bit 1 già clear).
			 */
			static const u16 rad_0017[] = {
				0, 0, 0x0010, 0x0010, 0x0011, 0x0002, 0,
				0, 0x0010, 0x0011,
			};
			static const u16 rad_0217[] = {
				0, 0, 0x0010, 0x0010, 0x0011, 0x0002, 0,
				0, 0x0010, 0x0011,
			};
			b43_test_plan_radio_reads(0x0017, rad_0017,
						  ARRAY_SIZE(rad_0017));
			b43_test_plan_radio_reads(0x0217, rad_0217,
						  ARRAY_SIZE(rad_0217));

			/*
			 * 0x0024 / 0x0224 (radio bit 0-1 HW-sticky).
			 * Il vendor a #52770/#52803 emette WR val=0x0303 (MOD mask=
			 * 0x0700 val=0x0300 → RD_val=0x0003). L'ultimo WR sul reg
			 * era val=0x0000 (#50117), ma il HW mantiene bit 0-1 set.
			 * Slot 0-3: matcha mirror sequence delle 4 letture pre-setup.
			 * Slot 4: baseline setup peek (non usato per calcoli).
			 * Slot 5: MOD interno RD → 0x0003 per WR=0x0303.
			 */
			static const u16 rad_0024[] = {
				0x0003, 0x0003, 0x0003, 0x0000, 0x0000, 0x0003,
			};
			static const u16 rad_0224[] = {
				0x0003, 0x0003, 0x0003, 0x0000, 0x0000, 0x0003,
			};
			b43_test_plan_radio_reads(0x0024, rad_0024,
						  ARRAY_SIZE(rad_0024));
			b43_test_plan_radio_reads(0x0224, rad_0224,
						  ARRAY_SIZE(rad_0224));
		}

		/*
		 * B5 RX AFE calibration polls (vendor #41909+): registro 0x0380
		 * viene armato con un comando (bit 15 set) e poi pollato finché
		 * il bit 15 torna clear. Poll counts per iter 1..24 hardcoded
		 * dal trace d6220 ch36.
		 */
		{
			#define B(n) 0x8000
			#define D 0x0000
			#define B5 B(0),B(0),B(0),B(0),B(0)
			#define B10 B5,B5
			#define B20 B10,B10
			#define B38 B20,B10,B5,B(0),B(0),B(0)
			#define B41 B38,B(0),B(0),B(0)
			#define B68 B38,B20,B10
			static const u16 poll_0x0380[] = {
				/* Gruppo core 0 (iter 1-6): 39 42 42 69 11 39 */
				B38, D, B41, D, B41, D, B68, D, B10, D, B38, D,
				/* Gruppo core 1 (iter 7-12): 41 40 13 47 61 43 */
				B38, B(0), B(0), D,
				B38, B(0), D,
				B10, B(0), B(0), D,
				B38, B5, B(0), B(0), B(0), D,
				B38, B20, B(0), B(0), D,
				B41, B(0), D,
				/* Gruppo core 2 (iter 13-18): 44 31 5 10 59 42 */
				B41, B(0), B(0), D,
				B20, B10, D,
				B(0), B(0), B(0), B(0), D,
				B5, B(0), B(0), B(0), B(0), D,
				B38, B20, D,
				B41, D,
				/* Gruppo 4 RXIQ measurement (iter 19-24): 43 60 35 12 8 58 */
				B38, B(0), B(0), B(0), B(0), D,
				B38, B20, B(0), D,
				B20, B10, B(0), B(0), B(0), B(0), D,
				B10, B(0), D,
				B5, B(0), B(0), D,
				B38, B10, B(0), B(0), B(0), B(0),
				B(0), B(0), B(0), B(0), B(0), D,
			};
			#undef B
			#undef D
			#undef B5
			#undef B10
			#undef B20
			#undef B38
			#undef B41
			#undef B68

			b43_test_plan_phy_reads(0x0380, poll_0x0380,
				(int)(sizeof(poll_0x0380) / sizeof(u16)));
		}

		int r = b43_phyops_ac.switch_channel(&g_wldev, 36);
		fprintf(stderr, "test: switch_channel returned %d\n", r);
	} else {
		fprintf(stderr, "test: unknown flow '%s'\n", flow);
		return 2;
	}

	fprintf(stderr, "test: plan consumption ---\n");
	b43_test_plans_report(stderr);
	return 0;
}
