/* SPDX-License-Identifier: GPL-2.0
 * Test driver: build a minimal mock b43_wldev matching the D6220 board
 * (BCM4352 2x2, r2069 rev 4, coremask=0x3) and invoke one of the
 * scratch entry points. Emitted trace on stdout should match the
 * corresponding wl-diag capture after normalisation.
 *
 * Usage:
 *   ./ac_trace [flow] [board]
 *     flow  = full (default) | up | down | switch_channel | periodic |
 *             op_init | rfkill | crsmin | rxiq_est_debug | rxiq_comp
 *     board = d6220 (default) | agcombo | dsl | tg789
 *
 * `full` is the broadest flow, ~28k HW ops on d6220 ch36: the analog preamble,
 * rfkill, op_init, then everything run_switch_channel() drives -- the channel
 * setup of b43_phy_ac_op_switch_channel(), the core's BSS configuration, the
 * second half of the setup, the post-channel calibrations and the bss-up
 * burst. `switch_channel` alone models a runtime channel change and the others
 * exercise narrower slices. The AC-PHY files of the driver link with the
 * harness's RX-IQ estimator, rxiqcal_phy_ac.c; see the Makefile SRCS list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b43.h"
#include "phy_common.h"
#include "phy_ac.h"
#include "radio_2069.h"
#include "rxiqcal_phy_ac.h"
#include "test_harness.h"
#include "readplan_0270.h"

extern enum nl80211_band b43_test_band;

#include "../board_profile.h"
#include "leds.h"

static struct b43_phy_ac       g_ac;
/* Le ricariche del template beacon: quante fatte, e quante allo start_ap. */
static unsigned int g_beacon_reloads;
static unsigned int g_beacon_reload_pre;
/* The timeline has a reload inside the watchdog turn being delivered. */
static bool g_wd_reload;
/* Il check pendente, deciso prima che g_wldev sia costruito. */
static bool g_cac_pending;
static struct b43_wl           g_wl;
static struct ieee80211_hw     g_hw;
static struct ieee80211_channel g_chan;
static struct ssb_sprom        g_sprom;
static struct bcma_bus         g_bcma_bus;
static struct bcma_device      g_bcma_dev;
static struct b43_bus_dev      g_bus_dev;
static struct b43_wldev        g_wldev;

/*
 * Le disposizioni della shared memory di b43_shm_layout_find() (main.c del
 * kernel), che qui non e' nel link: 928 per le board del 7.14, 784 per la
 * DSL, scelta dal campo ucode del profilo.
 */
static const struct b43_shm_layout g_shm_784 = {
	.mac_cfg = 0x0894, .cts_duration = 4400, .cts_suspended = true,
	.txerr = 0x16c8,
};
static const struct b43_shm_layout g_shm_928 = {
	.mac_cfg = 0x08ec, .mac_cfg_ext = true, .cts_duration = 29000,
	.txerr = 0x17f4,
};

/*
 * Il bit 0 di PHY 0x0270 e' il flag di start della misura RX-IQ e l'hardware lo
 * azzera al completamento: il driver rilegge il registro finche' resta alto,
 * quindi il valore letto governa quante op emette. Senza AC_READ_ORACLE quel
 * valore deve venire da qualche parte, e viene dalle letture che il tracer
 * vendor ha registrato sulla board in questione.
 *
 * La scelta e' su board e fase, gli stessi due assi da cui dipendono i
 * conteggi: le catture d6220 mostrano 44/14/42/43 poll nei blocchi v2
 * dell'attach contro 29/27/22/41 del down->up. Dove esiste una sola cattura
 * con i RETVAL la stessa serve entrambe le fasi, ed e' un'approssimazione
 * dichiarata: per agcombo c'e' solo un rescan (fase successiva) e per il DSL
 * solo dei down->up, l'attach da freddo non e' catturabile.
 */
static void plan_rxiq_poll(const char *board, bool first_init)
{
	const u16 *plan;
	int n;

	if (!strcmp(board, "agcombo")) {
		plan = readplan_0270_agcombo;
		n = (int)ARRAY_SIZE(readplan_0270_agcombo);
	} else if (!strcmp(board, "dsl")) {
		plan = readplan_0270_dsl;
		n = (int)ARRAY_SIZE(readplan_0270_dsl);
	} else if (first_init) {
		plan = readplan_0270_d6220_first;
		n = (int)ARRAY_SIZE(readplan_0270_d6220_first);
	} else {
		plan = readplan_0270_d6220_next;
		n = (int)ARRAY_SIZE(readplan_0270_d6220_next);
	}
	b43_test_plan_phy_reads(0x0270, plan, n);
}

/*
 * Chain @c's noise ring full of @idx, and the CRS block programmed from it:
 * the ladder index in force on the chain is @idx and the hardware carries it.
 */
static void seed_crs_chain(unsigned int c, u8 idx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(g_ac.crs_ring[0]); i++)
		g_ac.crs_ring[c][i] = idx;
	g_ac.crs_prog[c] = idx;
}

/* The same on every chain, so the 0x0910 bank carries no offset. */
static void seed_crs_rings(u8 idx)
{
	unsigned int c;

	for (c = 0; c < B43_PHY_AC_MAX_CORES; c++)
		seed_crs_chain(c, idx);
	g_ac.crs_ring_head = 0;
	g_ac.crs_ring_len = ARRAY_SIZE(g_ac.crs_ring[0]);
}

static int timeline_has(const char *kind);

/* Profilo montato, per i pochi punti che servono a modellare il core. */

static const struct board_profile *g_profile;


/*
 * Su quali delle quattro passate conf_tx lo stack di sopra ripubblica il
 * beacon. Non e' strutturale: su cold01 accade solo sulla prima, su cold05 su
 * tutte e quattro, e lo stesso albero emette le quattro passate in entrambi i
 * casi. E' hostapd che spinge un beacon mentre il tracer gira, quindi va come
 * AC_PROBE_TICKS e AC_BEACON_RELOADS -- fuori dalla cattura e dentro dal
 * chiamante.
 *
 * Le passate sono nell'ordine di edcf_queues[]: best effort, background,
 * video, voce, cioe' i blocchi che finiscono su 0x027e, 0x025e, 0x029e e
 * 0x02be. Il bit n dice che dopo la passata n c'e' una ricarica.
 */
static unsigned int g_edcf_reload_mask = 0x1;

/*
 * Lunghezza dell'SSID della cattura, in byte. Un ingresso solo: da lui
 * dipendono la cella 0x001e, le celle BTL e i PLCP degli otto rate. Lo sweep ricatturato usa `test-ap5`, otto caratteri; quello
 * vecchio ne usava sette, ed e' la ragione per cui i valori derivati erano
 * tutti uno sotto. Leva perche' la prossima cattura potra' usarne un terzo.
 */
static unsigned int g_ssid_len = 8;


static void mount_board(const struct board_profile *p)
{
	g_profile = p;
	memset(&g_ac, 0, sizeof(g_ac));
	g_ac.num_cores  = p->num_cores;
	g_ac.coremask   = p->coremask;
	/*
	 * dacbuf_cap: chip vero d6220 ha RCCAL_G=0x0009 → dacbuf_cap=0.
	 * Vedi commento in analog_on_reset dacbuf loop.
	 */
	/*
	 * dacbuf_cap: rccal computes it in op_init as (RCCAL_G & 0x03e0)>>5
	 * from the post-apply read. switch_channel does not re-run rccal, so
	 * replicate it here from the per-board rccal_g with the same formula.
	 */
	g_ac.dacbuf_cap = (u8)((p->rccal_g & 0x03e0) >> 5);
	/*
	 * lpf_cap0/1: in the real driver rccal computes it in op_init as
	 * cap = ((F-E)*193)>>8 from the R2069_RCCAL_E/F reads. switch_channel
	 * does not re-run rccal, so replicate that precondition here from the
	 * per-board E/F, using the same formula, instead of forcing a value.
	 */
	{
		u8 cap = (u8)(((p->rccal_f - p->rccal_e) * 193) >> 8);
		g_ac.lpf_cap0 = cap;
		g_ac.lpf_cap1 = cap;
	}

	board_profile_to_sprom(p, &g_sprom);
	/*
	 * phycal_tempdelta as the instance ran with, when it is not the NVRAM's:
	 * `wl phycal_tempdelta` after the insmod. It decides whether a full
	 * calibration opens with a temperature reading; see
	 * b43_phy_ac_cal_tempsense().
	 */
	{
		const char *e = getenv("AC_TEMPDELTA");

		if (e && *e)
			g_sprom.phycal_tempdelta = (u8)strtoul(e, NULL, 0);
	}

	g_bcma_dev.bus = &g_bcma_bus;
	g_bus_dev.bus_type = B43_BUS_BCMA;
	g_bus_dev.chip_id  = p->chip_id;
	g_bus_dev.bus_sprom = &g_sprom;
	g_bus_dev.bdev     = &g_bcma_dev;

	/*
	 * ch36 unless AC_CHANNEL says otherwise. Driving another channel only
	 * makes sense against a capture of that channel supplied through
	 * AC_READ_ORACLE. Channels 1 to 14 are 2.4 GHz.
	 */
	g_chan.band = NL80211_BAND_5GHZ;
	g_chan.hw_value = 36;
	{
		const char *e = getenv("AC_CHANNEL");

		if (e) {
			unsigned long v = strtoul(e, NULL, 10);

			if (v < 1 || v > 200) {
				fprintf(stderr, "AC_CHANNEL=%s out of range\n", e);
				exit(1);
			}
			g_chan.hw_value = (u16)v;
		}
	}
	g_chan.center_freq = 5000 + 5 * g_chan.hw_value;
	if (g_chan.hw_value <= 14) {
		g_chan.band = NL80211_BAND_2GHZ;
		g_chan.center_freq = g_chan.hw_value == 14 ?
			2484 : 2407 + 5 * g_chan.hw_value;
		b43_test_band = NL80211_BAND_2GHZ;
	}

	/*
	 * Radar-detection requirement, as cfg80211 would set it from the
	 * regulatory domain: U-NII-1 (5150-5250) and U-NII-3 (5725-5850) carry
	 * no radar duty, U-NII-2A (5250-5350) and U-NII-2C do.
	 *
	 * Where U-NII-2C ends is the one edge the spec and this device do not
	 * agree on, and here the captures arbitrate instead of the spec. The
	 * stock driver polls the radar detector on a channel that carries the
	 * duty and not on one that does not, so the POLL events of
	 * reverse-tools/timeline.py read the answer off each segment; run over
	 * all forty-three of the cold sweep they draw the boundary at ch140, not
	 * at 5725:
	 *
	 *   ch36-48       no poll        ch52-140      poll
	 *   ch144-165     no poll
	 *
	 * and the same on the 40 and 80 MHz blocks, which follow their low
	 * channel: 140/40 spans 5690-5730 and polls, 149/40 does not. So 5720
	 * -- ch144, inside U-NII-2C by the spec -- carries no duty on this
	 * board. Marking it would suppress the calibrations the vendor runs
	 * there, which is most of a 29k-operation attach.
	 *
	 * This is the regulatory domain the d6220 captures were taken under,
	 * not a universal truth: the tg789vac's domain does put ch144 under
	 * the duty, and its ch144 segment polls the detector 166 times. So
	 * where a timeline is given, its POLL events are the answer and the
	 * frequency rule is the fallback for the flows that run without one;
	 * on hardware it is cfg80211 that says so.
	 */
	{
		int polls = timeline_has("POLL");

		if (polls > 0 ||
		    (polls < 0 &&
		     ((g_chan.center_freq > 5250 && g_chan.center_freq <= 5350) ||
		      (g_chan.center_freq > 5470 && g_chan.center_freq <= 5700))))
			g_chan.flags |= IEEE80211_CHAN_RADAR;
	}

	/*
	 * Whether a channel availability check is outstanding. On hardware it
	 * is mac80211 asking for radar detection when it tunes the channel --
	 * hw->conf.radar_enabled -- and hostapd asks for it on every AP start
	 * on a channel with the duty. The vendor does the same on every `wl up`:
	 * the hot up on ch52 arms the detector, polls it 414 times and brings
	 * the BSS up 60 s later exactly like the cold attach does. So the
	 * default is the duty itself, cold or hot.
	 *
	 * AC_DFS_CAC_DONE overrides it, for a channel whose check some earlier
	 * owner had already passed and cfg80211 still holds as available.
	 */
	{
		const char *e = getenv("AC_DFS_CAC_DONE");

		g_cac_pending = e ? (strtoul(e, NULL, 0) == 0)
				  : !!(g_chan.flags & IEEE80211_CHAN_RADAR);
	}

	/*
	 * Regulatory ceiling for the channel, in dBm, as cfg80211 would supply
	 * it. The captures were taken on a system whose ceiling does not bind
	 * -- ch100 receives 86 quarter-dBm, above the 84 a 21 dBm limit would
	 * allow -- so the default here is permissive and AC_MAX_POWER lowers it
	 * to exercise the clamp.
	 */
	{
		const char *e = getenv("AC_MAX_POWER");
		int dflt = e ? (int)strtol(e, NULL, 10) : 30;

		g_chan.max_power = dflt;
		b43_test_reg_init(dflt, getenv("AC_MAX_POWER_MAP"));
	}

	/*
	 * CRS minimum-power state carried in from the previous cycle. The sweep
	 * is a continuous chain of channel changes, so running one segment on
	 * its own needs the state the chain would have left: AC_CRS_INDEX and
	 * AC_CRS_SUBBAND are the ladder entry, seeded into every chain's ring,
	 * and the sub-band in force. Without AC_CRS_INDEX the rings start
	 * empty, and the default sub-band of 0xff forces the reset to the
	 * floor: both are what a cold start does.
	 */
	{
		const char *e = getenv("AC_CRS_INDEX");

		if (e)
			seed_crs_rings((u8)strtoul(e, NULL, 0));
		e = getenv("AC_CRS_SUBBAND");
		g_ac.crs_subband = e ? (u8)strtoul(e, NULL, 0) : 0xff;
	}
	g_hw.conf.chandef.chan  = &g_chan;

	/*
	 * AC_BW selects the operating width: 20, 40 or 80. The centre of a
	 * bonded channel sits above the primary one by half the bonding span,
	 * so +10 MHz at 40 and +30 at 80, which is what the sweep's segment
	 * names imply -- ch36-bw40 is the 36/40 pair centred on 5190, and
	 * ch36-bw80 is 36 to 48 centred on 5210.
	 */
	{
		const char *e = getenv("AC_BW");
		unsigned long bw = e ? strtoul(e, NULL, 10) : 20;

		switch (bw) {
		case 20:
			g_hw.conf.chandef.width = NL80211_CHAN_WIDTH_20;
			g_hw.conf.chandef.center_freq1 = g_chan.center_freq;
			break;
		case 40:
			g_hw.conf.chandef.width = NL80211_CHAN_WIDTH_40;
			g_hw.conf.chandef.center_freq1 = g_chan.center_freq + 10;
			break;
		case 80:
			g_hw.conf.chandef.width = NL80211_CHAN_WIDTH_80;
			g_hw.conf.chandef.center_freq1 = g_chan.center_freq + 30;
			break;
		default:
			fprintf(stderr, "AC_BW=%s: expected 20, 40 or 80\n", e);
			exit(1);
		}
	}
	g_hw.wiphy = NULL;
	g_wl.hw = &g_hw;

	memset(&g_wldev, 0, sizeof(g_wldev));
	g_wldev.dev = &g_bus_dev;
	g_wldev.wl  = &g_wl;
	g_wldev.phy.type       = B43_PHYTYPE_AC;
	g_wldev.phy.rev        = p->phy_rev;
	g_wldev.fw.shm         = p->ucode == 784 ? &g_shm_784 : &g_shm_928;
	g_wldev.phy.radio_ver  = p->radio_ver;
	g_wldev.phy.radio_rev  = p->radio_rev;
	g_wldev.phy.dacbuf_cap = g_ac.dacbuf_cap;
	g_wldev.phy.lpf_cap    = g_ac.lpf_cap0;
	g_wldev.phy.ac         = &g_ac;
	g_wldev.cac_pending    = g_cac_pending;
	/*
	 * Come b43_phy_init(): il PHY guarda phy.chandef, non dentro hw->conf.
	 * DOPO il memset di g_wldev qui sopra, o si perde.
	 */
	g_wldev.phy.chandef    = &g_hw.conf.chandef;
	/*
	 * The width the MAC was last told about, which the chanspec write
	 * compares against: AC_MAC_WIDTH seeds it (gates.sh passes 0 on a
	 * segment that carries MAC.BW, the segment's own width otherwise), so
	 * a segment run on its own does not repeat a write the chain made.
	 */
	{
		const char *mw = getenv("AC_MAC_WIDTH");

		g_ac.mac_width = mw ? (enum nl80211_chan_width)
				      strtoul(mw, NULL, 0)
				    : g_hw.conf.chandef.width;
	}
	/*
	 * Primo bring-up per default: e' la fase che le catture attach
	 * testimoniano. AC_FIRST_INIT=0 seleziona il bring-up successivo, da
	 * confrontare con le catture down->up.
	 */
	{
		const char *e = getenv("AC_FIRST_INIT");
		g_wldev.phy.do_full_init = !(e && !strcmp(e, "0"));
	}

	/*
	 * Channel of the previous calibration, which in a live driver survives
	 * from one cycle to the next but which a single-flow run has to be
	 * told. It selects the irregular first probe group; see
	 * b43_phy_ac_rxiqcal_finalize.
	 *
	 * The default matches the flow: a first bring-up has no previous
	 * calibration, while a later cycle is by default a down-and-up on the
	 * channel it was already on, which is what the down->up captures are.
	 * AC_LAST_CAL_CHANNEL overrides it, and is what a warm cycle following
	 * a channel change needs -- the odd-numbered sweep segments.
	 */
	{
		const char *e = getenv("AC_LAST_CAL_CHANNEL");

		if (e)
			g_ac.last_cal_channel = (u16)strtoul(e, NULL, 10);
		else if (!g_wldev.phy.do_full_init)
			g_ac.last_cal_channel = g_chan.hw_value;
	}

	/*
	 * Le ricariche del template beacon che il vendor mette dentro la coda
	 * del bring-up, prima del primo giro del watchdog: sono dello stack e
	 * il conteggio viene dalla cattura (reverse-tools/beacon_reloads.py,
	 * il campo prima dei due punti). Le emette il core allo start_ap, vedi
	 * run_switch_channel(); quelle dopo sono eventi della timeline.
	 */
	{
		const char *e = getenv("AC_BEACON_RELOADS");

		if (e)
			g_beacon_reload_pre = (unsigned int)strtoul(e, NULL, 10);
	}

	/*
	 * Stato del watchdog all'ingresso: il contatore del periodo da dieci
	 * giri, che nel driver gira dal bring-up e che un segmento a caldo
	 * prende a meta'. reverse-tools/timeline.py lo legge dalla prima
	 * tempsense della cattura. Senza, zero: la tempsense sul nono giro,
	 * che e' l'attach a freddo.
	 */
	{
		const char *e = getenv("AC_WD_PHASE");

		if (e && *e)
			g_ac.wd_turns = (u16)strtoul(e, NULL, 10);
	}

	/*
	 * La forma del primo giro dopo il bring-up: piena invece della sola
	 * spazzata. La cattura lo dice dall'ordine delle quattro celle di
	 * testa, e reverse-tools/timeline.py lo passa. Vedi @wd_entry_turn.
	 */
	{
		const char *e = getenv("AC_WD_ENTRY_TURN");

		if (e && *e)
			g_ac.wd_entry_turn = strtoul(e, NULL, 10) != 0;
	}
	{
		const char *e = getenv("AC_SSID_LEN");

		if (e && *e)
			g_ssid_len = (unsigned int)strtoul(e, NULL, 0);
	}
	{
		const char *e = getenv("AC_EDCF_RELOADS");

		if (e)
			g_edcf_reload_mask = (unsigned int)strtoul(e, NULL, 0);
	}

	/* Preconditions the rxiqcal REQUIRE gates want to see. */
	g_ac.status_mask = B43_PHY_AC_STATE_RX_WAITED |
			   B43_PHY_AC_STATE_CLIP_ALL_DIS;
}

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
	} else if (!strcmp(p->name, "tg789")) {
		/* Prime letture di tg789vac-v2 cold01-ch36-bw20: CLASSCTL 0x0df7,
		 * radio 0x041a 0x0004, radio 0x0521 0x2000. La 0x0433 dell'agcombo
		 * non vale qui: legge 0x6060 e poi 0x4161. */
		b43_test_mirror_phy_set(0x0140, 0x0800);
		b43_test_mirror_radio_set(0x041a, 0x0004);
		b43_test_mirror_radio_set(0x0521, 0x2000);
	} else if (!strcmp(p->name, "dsl")) {
		/*
		 * DSL-3580L initial register state, read back from the wl6.30
		 * capture's RETVALs (router-data/dsl3580l). These replace the
		 * d6220-derived read-plan values so the port is exercised
		 * against what the DSL hardware really returns, not values
		 * tuned to make the d6220 trace match.
		 *
		 * 0x0140 CLASSCTL: the 4352 here comes up with bit 0x0800 SET
		 * (real read 0x0df4/0x0df7), unlike the D6220 -- so the "0x4352
		 * leaves it clear" assumption is a d6220 artifact, not a chip
		 * rule.
		 */
		b43_test_mirror_phy_set(0x0140, 0x0800);
		/* Radio 0x0033/0233/0433 background: real 0x6061/0x6061/0x6060
		 * (D6220 path yields the 0x40xx family). */
		b43_test_mirror_radio_set(0x0033, 0x6061);
		b43_test_mirror_radio_set(0x0233, 0x6061);
		b43_test_mirror_radio_set(0x0433, 0x6060);
		/* Radio 0x0045/0245/0445 background: real 0x703f/0x703f/0x7000. */
		b43_test_mirror_radio_set(0x0045, 0x703f);
		b43_test_mirror_radio_set(0x0245, 0x703f);
		b43_test_mirror_radio_set(0x0445, 0x7000);
	}
}

/*
 * Segmenti eseguibili. Ognuno registra i propri read plan e poi chiama la sua
 * op: plan_add azzera il cursore quando un indirizzo viene ri-registrato,
 * quindi registrare per segmento subito prima di eseguirlo e' anche l'ordine
 * corretto quando i segmenti sono concatenati.
 */
static void run_op_init(void)
{
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
}

static void run_rfkill(void)
{
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
}

static void emit_core_bss_config(void);
static void emit_core_bss_config1(void);
static void emit_core_conf_tx_passes(void);
static void emit_core_cac_gate(bool open);
static void emit_core_beacon_reload(unsigned int which);

/*
 * Replay degli eventi dell'ambiente dopo la coda del bring-up, nell'ordine dei
 * loro istanti sulla cattura: reverse-tools/timeline.py li estrae e dice cosa
 * sono. Ogni riga e' un callback che su hardware arriva da fuori del PHY:
 *
 *   WD      il tick periodico da un secondo -> pwork_1sec del PHY
 *   POLL    il work da 150 ms del radar   -> b43_phy_ac_radar_poll()
 *   TPL     bss_info_changed del core     -> la ricarica del template, che e'
 *                                            del core e la emette l'harness;
 *                                            quella che segue un WD cade fra
 *                                            le due passate dei contatori
 *   BSS_UP  il beacon parte dopo il CAC   -> il core chiude il check, riapre
 *                                            la riga AMT e chiama cac_done
 *   NOISE   il campione di rumore pronto   -> b43_phy_ac_noise_sample_done(),
 *                                            che su hardware arriva dal
 *                                            tasklet del core
 *
 * Quanti siano e quando cadano non e' del driver: e' quanto e' durata la
 * cattura e cosa ha fatto lo stack nel frattempo. Il driver decide solo cosa
 * fare a ogni callback.
 */
/*
 * Whether the timeline carries an event of @kind: 1 or 0, and -1 when there
 * is no timeline to ask.
 */
static int timeline_has(const char *kind)
{
	const char *path = getenv("AC_TIMELINE");
	FILE *f;
	char line[128];
	int found = 0;

	if (!path || !*path)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (!found && fgets(line, sizeof(line), f)) {
		char k[16];

		if (sscanf(line, "%*f %*d %15s", k) == 1 && !strcmp(k, kind))
			found = 1;
	}
	fclose(f);
	return found;
}

static void run_timeline(void)
{
	const char *path = getenv("AC_TIMELINE");
	FILE *f;
	char line[128];

	if (!path || !*path)
		return;
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "test: cannot open timeline %s\n", path);
		exit(1);
	}

	while (fgets(line, sizeof(line), f)) {
		char kind[16];

		if (sscanf(line, "%*f %*d %15s", kind) != 1)
			continue;
		if (!strcmp(kind, "WD")) {
			long at = ftell(f);
			char next[128], nkind[16];

			/*
			 * A reload that follows the turn in the timeline falls
			 * between its two counter passes: the watchdog's site
			 * delivers it, see b43_phy_ac_core_site().
			 */
			g_wd_reload = fgets(next, sizeof(next), f) &&
				sscanf(next, "%*f %*d %15s", nkind) == 1 &&
				!strcmp(nkind, "TPL");
			if (!g_wd_reload)
				fseek(f, at, SEEK_SET);
			b43_phyops_ac.pwork_1sec(&g_wldev);
			if (g_wd_reload) {
				g_wd_reload = false;
				emit_core_beacon_reload(g_beacon_reloads++);
			}
		} else if (!strcmp(kind, "POLL"))
			b43_phy_ac_radar_poll(&g_wldev);
		else if (!strcmp(kind, "TPL"))
			emit_core_beacon_reload(g_beacon_reloads++);
		else if (!strcmp(kind, "BSS_UP")) {
			if (g_wldev.cac_pending) {
				g_wldev.cac_pending = false;
				emit_core_cac_gate(true);
				b43_phyops_ac.cac_done(&g_wldev);
			}
		} else if (!strcmp(kind, "NOISE"))
			b43_phy_ac_noise_sample_done(&g_wldev);
		else {
			fprintf(stderr, "test: unknown timeline event %s\n", kind);
			exit(1);
		}
	}
	fclose(f);
}

/*
 * Doppioni di b43_upload_card_macaddress(), b43_macfilter_set() e
 * b43_security_init() di b43/main.c: le righe in cima della address match
 * table -- 0x3e il BSSID, 0x3f la stazione -- e le righe delle chiavi.
 *
 * b43 le scrive nella coda di b43_wireless_core_init(), dopo il primo
 * switch_channel; il vendor dentro il channel setup, in tre punti, e la
 * prima coppia prima che il BSSID ci sia. Si emettono ai siti del PHY, vedi
 * b43_phy_ac_core_site().
 *
 * Le righe delle chiavi sono 56, 0x00-0x37, quelle che azzera il vendor su
 * tutti i segmenti; b43_clear_keys() ne azzera B43_NR_PAIRWISE_KEYS, 50, e la
 * differenza e' il conto delle righe in docs/retrace-todo.md, "Key-table
 * clearing".
 */
static void emit_core_top_row(bool self, u16 flags)
{
	b43_test_emit_addrm(self ? 0xffffffffu : 0xfffffffeu);
	b43_test_emit_amt(self ? 0x3f : 0x3e, flags);
}

#define B43_TEST_KEY_ROWS	0x38

/*
 * La tabella chiavi del corerev 42 come il vendor la azzera prima delle
 * righe: il materiale da KTP (0x087a, 0x10f4), 480 parole, e il blocco
 * degli indici, 0x05e0-0x0666, 68 parole. b43_security_init() lo fa nel
 * core init, per le sue voci.
 */
#define B43_TEST_KEY_MATERIAL	0x10f4
#define B43_TEST_KEY_MATERIAL_WORDS	480
#define B43_TEST_KEYIDX_BLOCK	0x05e0
#define B43_TEST_KEYIDX_WORDS	68

/*
 * Doppioni del core sul bring-up a freddo: la MACCONTROL di
 * b43_wireless_core_reset() (IHR | AWAKE, quattro reset nella cattura), il
 * salto a 0 del PSM e il suo avvio in b43_upload_microcode(), e in coda la
 * pulizia di GPOUT con la GPIO.CTL vuota di b43_gpio_init(). Il vendor li
 * intercala alle host flag del preambolo del PHY; b43 li fa da main.c
 * prima di b43_phy_init(), quindi il PHY non li emette e stanno qui.
 */
static void emit_core_reset_mctrl(void)
{
	b43_test_emit_mctrl(0, B43_MACCTL_IHR_ENABLED |
			   B43_MACCTL_AWAKE);
}

static void emit_core_ucode_load(void)
{
	b43_test_emit_mctrl(0, B43_MACCTL_IHR_ENABLED |
			   B43_MACCTL_AWAKE | B43_MACCTL_PSM_JMP0);
}

static void emit_core_ucode_start(void)
{
	b43_test_emit_mctrl(0, B43_MACCTL_IHR_ENABLED |
			   B43_MACCTL_AWAKE | B43_MACCTL_INFRA |
			   B43_MACCTL_PSM_RUN);
	b43_test_emit_mctrl((u32)~B43_MACCTL_GPOUTSMSK, 0);
	bcma_chipco_gpio_control(&g_wldev.dev->bdev->bus->drv_cc, 0, 0);
}

/*
 * La coda del down del vendor: enable e suspend del MAC (wlc_bmac_down) e la
 * MACCONTROL del core reset che precede lo spegnimento dell'analogico. Fra
 * l'enable e la suspend il vendor spegne i due LED (wlc_bmac_led, del core),
 * che restano nel perimetro del confronto.
 */
/*
 * I toggle del MAC che il vendor fa dentro al proprio down. In b43 il down
 * del PHY gira col MAC tenuto sospeso da b43_wireless_core_stop() e dal
 * bracket di b43_software_rfkill(), a contatore 2: le coppie enable/suspend
 * di li' non toccano il registro, e per questo si emettono qui in forma
 * grezza, fuori dal contatore.
 */
static void emit_core_mac_toggle(bool on)
{
	b43_test_emit_mctrl(~(u32)B43_MACCTL_ENABLED,
			   on ? B43_MACCTL_ENABLED : 0);
}

static void emit_core_down(void)
{
	emit_core_mac_toggle(true);
	emit_core_mac_toggle(false);
	emit_core_reset_mctrl();
}

/*
 * Il modo operativo del core, che il vendor riscrive dentro alle fasi del
 * PHY: la promiscuita' sui beacon (bit 20), i filtri KEEP_* e, al down,
 * INFRA/DISCPMQ e AP. In b43 e' b43_adjust_opmode(); qui si emette il bit
 * che il vendor tocca in quel punto.
 */
static void emit_core_opmode(u32 mask, u32 set)
{
	b43_test_emit_mctrl(mask, set);
}

/*
 * Il blocco di configurazione del BSS del core che precede l'adjust del TX
 * power: TBTT hold alzato e abbassato (b43_time_lock/unlock intorno al
 * beacon interval), AP e INFRA (b43_adjust_opmode), il PRETBTT a 2
 * (b43_set_pretbtt) e la promiscuita' sui beacon fra un enable e una
 * suspend. DISCPMQ si abbassa come fa il vendor; b43 lo tiene alzato, la
 * coda di power management e' fuori scopo (docs/retrace-todo.md).
 */
static void emit_core_bss_mode(void)
{
	b43_test_emit_mctrl(~0x10000000u, 0x10000000);
	b43_test_emit_mctrl(~0x10000000u, 0);
	b43_test_emit_mctrl(~(u32)B43_MACCTL_AP, B43_MACCTL_AP);
	b43_test_emit_mctrl(~0x48020000u, B43_MACCTL_INFRA);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, B43_SHM_SH_PRETBTT, 2);
	b43_mac_enable(&g_wldev);
	b43_test_emit_mctrl(~0x00100000u, 0x00100000);
	b43_mac_suspend(&g_wldev);
}

void b43_phy_ac_core_site(struct b43_wldev *dev, enum b43_phy_ac_core_site site)
{
	u16 i;

	switch (site) {
	case B43_AC_SITE_CORE_DOWN:
		emit_core_down();
		break;
	case B43_AC_SITE_OPMODE_FILTERS:
		emit_core_opmode(~0x00100000u, 0);
		emit_core_opmode(~0x01c00000u, 0);
		break;
	case B43_AC_SITE_DOWN_OPMODE:
		emit_core_mac_toggle(true);
		emit_core_opmode(~0x48020000u, 0x40000000);
		emit_core_mac_toggle(false);
		emit_core_opmode(~(u32)B43_MACCTL_AP, 0);
		emit_core_mac_toggle(true);
		break;
	case B43_AC_SITE_DOWN_OPMODE_END:
		emit_core_mac_toggle(false);
		break;
	case B43_AC_SITE_CORE_RESET:
		emit_core_reset_mctrl();
		break;
	case B43_AC_SITE_UCODE_LOAD:
		emit_core_ucode_load();
		break;
	case B43_AC_SITE_UCODE_START:
		emit_core_ucode_start();
		break;
	case B43_AC_SITE_KEYS_CLEAR:
		for (i = 0; i < B43_TEST_KEY_MATERIAL_WORDS; i++)
			b43_shm_write16(dev, B43_SHM_SHARED,
					B43_TEST_KEY_MATERIAL + 2 * i, 0);
		for (i = 0; i < B43_TEST_KEYIDX_WORDS; i++)
			b43_shm_write16(dev, B43_SHM_SHARED,
					B43_TEST_KEYIDX_BLOCK + 2 * i, 0);
		for (i = 0; i < B43_TEST_KEY_ROWS; i++) {
			b43_test_emit_addrm(i);
			b43_test_emit_amt(i, 0);
		}
		break;
	case B43_AC_SITE_MACFILTER_FIRST:
		emit_core_top_row(true, 0x8008);
		emit_core_top_row(false, 0);
		break;
	case B43_AC_SITE_MACFILTER:
		emit_core_top_row(false, 0x8002);
		emit_core_top_row(true, 0x8008);
		break;
	case B43_AC_SITE_CAC_CLOSE:
		if (dev->cac_pending)
			emit_core_cac_gate(false);
		break;
	case B43_AC_SITE_BEACON_START:
		for (i = 0; i < g_beacon_reload_pre; i++)
			emit_core_beacon_reload(g_beacon_reloads++);
		break;
	case B43_AC_SITE_BEACON_WD:
		if (g_wd_reload) {
			g_wd_reload = false;
			emit_core_beacon_reload(g_beacon_reloads++);
		}
		break;
	}
}

/*
 * La testa del down del vendor prima che tocchi il PHY: la lettura di un
 * contatore a 32 bit (hi/lo/hi su 0x077c), la promiscuita' sui beacon giu' e
 * la suspend del MAC. In b43 e' b43_wireless_core_stop(), che sospende il MAC
 * prima di b43_phy_exit(); il contatore e il bit di modo sono suoi e non del
 * PHY.
 */
static void emit_core_stop(void)
{
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x077e);
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x077c);
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x077e);
	emit_core_opmode(~0x00100000u, 0);
	b43_mac_suspend(&g_wldev);
}

static void run_switch_channel(void)
{

	/*
	 * Stato entrante a switch_channel (post-op_init): MAC sospeso,
	 * classifier/clip-detector state non toccato (RX_ANY = 0,
	 * CLIP_ALL_DIS = 0). switch_channel imposterà RX_WAITED e
	 * CLIP_ALL_DIS via classifier()/clip_det() prima delle
	 * sub-routine che REQUIRE quello stato.
	 *
	 * op_init non è re-run qui: il suo trace è validato dal
	 * flow separato; eseguirlo back-to-back double-conta le
	 * register writes.
	 */
	/*
	 * Azzera solo i bit che switch_channel si aspetta puliti all'ingresso: non
	 * l'intero status_mask, che in `full` porta gia' il latch di fase messo da
	 * op_init (B43_PHY_AC_STATE_FIRST_BRINGUP) e lo stato del bracket PMU.
	 */
	g_ac.status_mask &= B43_PHY_AC_STATE_FIRST_BRINGUP |
			    B43_PHY_AC_STATE_PMU_REQ;

	/*
	 * NB: nessun read plan per 0x019e (table-write gate) in questo
	 * flow. Il mirror riflette correttamente lo stato del gate:
	 * tbl_write_lock scrive 0x0002, i sub-lock rileggono 0x0002 e
	 * al tbl_write_unlock finale il saved contiene lo stato del
	 * lock precedente (0 al primo lock, 0x0002 ai nested). Un plan
	 * a valore fisso 0x0000 rompeva questa sequenza.
	 */

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
	 * lo è, ma il flow avanza. I valori reali di lo_read/hi_read/cur
	 * richiedono una lettura strumentata ad hoc sul chip vero.
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
	 * run_rfseq_cmd legge 0x0403 e si ferma quando il bit 0 e' *caduto*: le
	 * invocazioni sono `0x0101, 0x0000` oppure `0x0000` da sola. Nello slice
	 * ch36 sono 18, con pattern [2,1,2,2,2,2,2,2,2,2,1,2,2,2,2,2,2,1] -- il
	 * plan lo riproduce per intero.
	 *
	 * Un plan tarato sulla lettura opposta -- l'attesa del bit che si alza --
	 * si esaurisce e dalla terza invocazione cade sul mirror, che restituisce
	 * 0. Il gate NON lo intercetta: la cattura ch36 ha i valori di lettura a
	 * UNDEFINED e il confronto li ignora, quindi un plan sbagliato qui non si
	 * vede dal punteggio.
	 */
	{
		static const u16 rfseq_done_poll[] = {
			0x0101, 0x0000, 0x0000, 0x0101, 0x0000, 0x0101,
			0x0000, 0x0101, 0x0000, 0x0101, 0x0000, 0x0101,
			0x0000, 0x0101, 0x0000, 0x0101, 0x0000, 0x0101,
			0x0000, 0x0000, 0x0101, 0x0000, 0x0101, 0x0000,
			0x0101, 0x0000, 0x0101, 0x0000, 0x0101, 0x0000,
			0x0101, 0x0000, 0x0000,
		};
		b43_test_plan_phy_reads(0x0403, rfseq_done_poll,
			(int)(sizeof(rfseq_done_poll) / sizeof(u16)));
	}

	/*
	 * Misura idle-TSSI: idle_tssi_meas deriva il base index da 0x0012 >> 2, e
	 * nello slice ch36 i base scritti sono core0 0x206/0x207/0x206 e core1
	 * 0x200 -- quindi le letture sono base << 2, in ordine core0, core1 per
	 * ognuna delle tre iterazioni. La cattura ch36 non ha i valori (UNDEFINED),
	 * ma le write li implicano.
	 */
	{
		static const u16 idle_tssi_meas_vals[] = {
			0x0818, 0x0800,   /* iter 1: core 0, core 1 */
			0x081c, 0x0800,   /* iter 2 */
			0x0818, 0x0800,   /* iter 3 */
		};
		b43_test_plan_phy_reads(0x0012, idle_tssi_meas_vals,
			(int)(sizeof(idle_tssi_meas_vals) / sizeof(u16)));
	}

	/*
	 * Accumulatori RXIQ. Il solve somma i due round piu' recenti, quindi la
	 * coppia deve dare i coefficienti che la cattura ch36 scrive: core 0
	 * `0x03f2/0x004c`, core 1 `0x03db/0x0037`. I valori sono uguali su tutti i
	 * round, cosi' qualunque coppia produce lo stesso risultato.
	 *
	 * Non sono valori misurati -- la ch36 non registra le letture -- ma un
	 * ingresso *coerente con l'uscita osservata*. Il gate resta significativo:
	 * i coefficienti sono cio' che verifica, e se il solve regredisse l'uscita
	 * cambierebbe. Per validare la misura vera serve AC_READ_ORACLE, vedi
	 * test/unit/README.md, "Read plans and the oracle".
	 */
	{
		static const u16 acc_06c3[6] = {
			0x0262, 0x0262, 0x0262, 0x0262, 0x0262, 0x0262,
		};
		static const u16 acc_06c2[6] = {
			0x5a00, 0x5a00, 0x5a00, 0x5a00, 0x5a00, 0x5a00,
		};
		static const u16 acc_06c5[6] = {
			0x02c0, 0x02c0, 0x02c0, 0x02c0, 0x02c0, 0x02c0,
		};
		static const u16 acc_06c4[6] = {
			0x5564, 0x5564, 0x5564, 0x5564, 0x5564, 0x5564,
		};
		static const u16 acc_06c1[6] = {
			0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008,
		};
		static const u16 acc_06c0[6] = {
			0x489b, 0x489b, 0x489b, 0x489b, 0x489b, 0x489b,
		};
		static const u16 acc_08c3[6] = {
			0x0262, 0x0262, 0x0262, 0x0262, 0x0262, 0x0262,
		};
		static const u16 acc_08c2[6] = {
			0x5a00, 0x5a00, 0x5a00, 0x5a00, 0x5a00, 0x5a00,
		};
		static const u16 acc_08c5[6] = {
			0x02a6, 0x02a6, 0x02a6, 0x02a6, 0x02a6, 0x02a6,
		};
		static const u16 acc_08c4[6] = {
			0x60dc, 0x60dc, 0x60dc, 0x60dc, 0x60dc, 0x60dc,
		};
		static const u16 acc_08c1[6] = {
			0x0015, 0x0015, 0x0015, 0x0015, 0x0015, 0x0015,
		};
		static const u16 acc_08c0[6] = {
			0xfe20, 0xfe20, 0xfe20, 0xfe20, 0xfe20, 0xfe20,
		};

		b43_test_plan_phy_reads(0x06c3, acc_06c3, 6);
		b43_test_plan_phy_reads(0x06c2, acc_06c2, 6);
		b43_test_plan_phy_reads(0x06c5, acc_06c5, 6);
		b43_test_plan_phy_reads(0x06c4, acc_06c4, 6);
		b43_test_plan_phy_reads(0x06c1, acc_06c1, 6);
		b43_test_plan_phy_reads(0x06c0, acc_06c0, 6);
		b43_test_plan_phy_reads(0x08c3, acc_08c3, 6);
		b43_test_plan_phy_reads(0x08c2, acc_08c2, 6);
		b43_test_plan_phy_reads(0x08c5, acc_08c5, 6);
		b43_test_plan_phy_reads(0x08c4, acc_08c4, 6);
		b43_test_plan_phy_reads(0x08c1, acc_08c1, 6);
		b43_test_plan_phy_reads(0x08c0, acc_08c0, 6);
	}

	/*
	 * Risultati della cal AFE RX: rxcal_afe_iter riscrive il valore che ha
	 * letto, quindi il plan porta cio' che la cattura ch36 riscrive. Chiavato
	 * sull'offset di lettura, non sulla posizione nella coda di 0x000f.
	 */
	{
		static const u16 afe_80[] = {
			0x0060, 0x0000, 0x0064, 0x000e,
			0x0060, 0x0000, 0x0062, 0xfffd,
		};
		static const u16 afe_83[] = {
			0xff01, 0xfe02,
		};
		static const u16 afe_84[] = {
			0x0102,
		};
		static const u16 afe_85[] = {
			0x0301,
		};
		static const u16 afe_87[] = {
			0x0020, 0x0000, 0x0022, 0x0004,
			0x0020, 0x0000, 0x0023, 0x0003,
		};
		static const u16 afe_8a[] = {
			0x0100, 0x0100,
		};
		static const u16 afe_8b[] = {
			0x0000,
		};
		static const u16 afe_8c[] = {
			0x0001,
		};
		static const u16 afe_8e[] = {
			0xfee0, 0x0080, 0xfed6, 0x00b6,
			0x00c0, 0xff40, 0x00dd, 0xff57,
		};
		static const u16 afe_91[] = {
			0x0d6e, 0x0764,
		};
		static const u16 afe_92[] = {
			0x05fe,
		};
		static const u16 afe_93[] = {
			0xfb03,
		};

		b43_test_plan_table_cell(0x000c, 0x0080, afe_80,
					 ARRAY_SIZE(afe_80));
		b43_test_plan_table_cell(0x000c, 0x0083, afe_83,
					 ARRAY_SIZE(afe_83));
		b43_test_plan_table_cell(0x000c, 0x0084, afe_84,
					 ARRAY_SIZE(afe_84));
		b43_test_plan_table_cell(0x000c, 0x0085, afe_85,
					 ARRAY_SIZE(afe_85));
		b43_test_plan_table_cell(0x000c, 0x0087, afe_87,
					 ARRAY_SIZE(afe_87));
		b43_test_plan_table_cell(0x000c, 0x008a, afe_8a,
					 ARRAY_SIZE(afe_8a));
		b43_test_plan_table_cell(0x000c, 0x008b, afe_8b,
					 ARRAY_SIZE(afe_8b));
		b43_test_plan_table_cell(0x000c, 0x008c, afe_8c,
					 ARRAY_SIZE(afe_8c));
		b43_test_plan_table_cell(0x000c, 0x008e, afe_8e,
					 ARRAY_SIZE(afe_8e));
		b43_test_plan_table_cell(0x000c, 0x0091, afe_91,
					 ARRAY_SIZE(afe_91));
		b43_test_plan_table_cell(0x000c, 0x0092, afe_92,
					 ARRAY_SIZE(afe_92));
		b43_test_plan_table_cell(0x000c, 0x0093, afe_93,
					 ARRAY_SIZE(afe_93));
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
		 * 0x0017 / 0x0217: il bit 4 lo muove l'hardware, non il driver,
		 * quindi il piano di lettura deve seguirlo slot per slot.
		 *
		 *   [0] [1]  bit 4 basso, dopo una WR a 0
		 *   [2]      peek di tempsense_radio_setup. Finisce in
		 *            tempsense_radio_saved[] e il restore lo riscrive: la
		 *            cattura ch36 ripristina 0x0011, quindi il peek deve
		 *            leggere quello
		 *   [3] [4]  RD interne dei maskset che seguono
		 *   [5]      fra il cleanup e l'iter 2 l'hardware fa bounce a
		 *            0x0002 (bit 4 basso, bit 1 alto) senza nessuna WR
		 *            tracciata: probabile flag di cal completata
		 *   [6]      guard a 0 per l'iter 3, che legge il mirror stabile
		 *   [7]      baseline del loop di rxiqcal_finalize, solo peek
		 *   [8] [9]  RD interne dei maskset, col bit 4 rialzato dall'hw
		 */
		static const u16 rad_0017[] = {
			0, 0, 0x0011, 0x0010, 0x0011, 0x0002, 0,
			0, 0x0010, 0x0011,
		};
		static const u16 rad_0217[] = {
			0, 0, 0x0011, 0x0010, 0x0011, 0x0002, 0,
			0, 0x0010, 0x0011,
		};

		/* Peek di setup: il cleanup ripristina 0x0001. */
		static const u16 rad_000e[] = { 0x0001 };
		static const u16 rad_020e[] = { 0x0001 };

		b43_test_plan_radio_reads(0x000e, rad_000e,
					  ARRAY_SIZE(rad_000e));
		b43_test_plan_radio_reads(0x020e, rad_020e,
					  ARRAY_SIZE(rad_020e));
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

	/*
	 * L'ordine di b43_op_config(): switch_channel, la configurazione BSS
	 * del core, il TX power adjust che b43_phy_txpower_check() accoda, le
	 * passate conf_tx dello stack, e al posto del mac_enable finale il
	 * phyop channel_calibrate, che riabilita il MAC fra lo sweep del gain
	 * control e le calibrazioni: e' dove il vendor lo emette.
	 */
	emit_core_bss_config();
	if (b43_phyops_ac.recalc_txpower(&g_wldev, true) ==
	    B43_TXPWR_RES_NEED_ADJUST) {
		emit_core_bss_mode();
		b43_phyops_ac.adjust_txpower(&g_wldev);
	}
	emit_core_conf_tx_passes();

	if (r == 0)
		b43_phyops_ac.channel_calibrate(&g_wldev);
	else
		b43_mac_enable(&g_wldev);

	/*
	 * Da qui in poi il driver non decide piu' il flusso: reagisce a quello
	 * che gli capita, e chi glielo fa capitare e' l'ambiente -- il kernel
	 * coi timer, mac80211 con i template e il bss-up. Qui l'ambiente e' la
	 * timeline della cattura; poi la discesa della radio, che su hardware
	 * e' software_rfkill(true) da b43_phy_exit().
	 */
	/*
	 * Il down come lo fa b43: b43_wireless_core_stop() sospende il MAC, poi
	 * b43_phy_exit() passa da b43_software_rfkill(), che sospende e
	 * riabilita attorno all'op del PHY.
	 */
	if (r == 0) {
		run_timeline();
		emit_core_stop();
		b43_mac_suspend(&g_wldev);
		b43_phyops_ac.software_rfkill(&g_wldev, true);
		b43_mac_enable(&g_wldev);
	}

	fprintf(stderr, "test: switch_channel returned %d\n", r);
}

/*
 * Bring-up continuo, nell'ordine di b43_phy_init(): switch_analog(true) ->
 * software_rfkill(false) -> ops->init -> switch_channel.
 */
/*
 * Doppione di b43_shm_macaddr_set(), che vive in b43/main.c e che l'harness
 * non compila.
 *
 * Sta qui e non nel PHY perche' NON e' codice del PHY: se finisse la' sarebbe
 * un errore di attribuzione. La forma e i valori sono gli stessi del core --
 * tre word a 0x078c, byte basso di ogni coppia per primo -- cosi' se il core
 * cambia questo diventa sbagliato e il confronto lo dice.
 *
 * Il PUNTO in cui viene chiamato e' quello di wl (subito dopo i limiti di
 * ritrasmissione in core init), non quello di b43, che chiama
 * b43_upload_card_macaddress() piu' tardi. E' la stessa licenza che l'harness
 * si prende altrove nel modellare l'ordine del core, e la riconciliazione dei
 * due ordini e' in docs/retrace-todo.md: qui serve a far avanzare il confronto
 * oltre queste tre op, non a dimostrare che b43 le emette nel posto giusto.
 */
/*
 * Host flags. b43 le scrive con b43_hf_write(), che copre le parole 1..3;
 * il core del port aggiunge la 4 e la 5. Il vendor le mette qui, appena prima del
 * chanspec (cold01 #686-#690), mentre b43 le scrive molto prima, fra WLCOREREV
 * e MACHW: l'ordine e' un punto di riconciliazione aperto, e qui si segue il
 * vendor perche' e' quello che il confronto misura.
 *
 * b43_hf_write() e' preceduta da b43_hf_read(), tre letture che il vendor in
 * questo punto non fa: non si emettono, sarebbero tre op in piu' che nessuna
 * cattura ha.
 *
 * Le parole 4 e 5 escono col PARZIALE, 0x0040 e 0x0080, che e' quello che il
 * vendor scrive qui: i bit in piu' (-> 0x0060 e 0x8088) arrivano dopo, in un
 * blocco che l'harness non modella. Scrivere subito il valore finale
 * salterebbe uno stato che l'ucode puo' leggere nel frattempo, e comunque non
 * combacerebbe. TODO: trovare i due punti dove i bit si aggiungono; sono a
 * #12243, #13526 e #13531 su cold01.
 */
/*
 * Op del core init di cui NON sappiamo il significato, trascritte per chiudere
 * la finestra del confronto posizionale. Ognuna e' un TODO.
 *
 *   0x0092  letta una volta prima della scrittura del MAC (cold01 #659) e una
 *           seconda a #12194. Vale 0xacc su d6220 e agcombo, quindi non e'
 *           stato di sessione ma qualcosa che l'ucode pubblica. b43.h non la
 *           nomina.
 *   0x077c  letta a 32 bit nella forma alto/basso/alto (#664-#668), che e' il
 *           modo in cui il driver stock legge i contatori a 32 bit. Vale zero
 *           qui: e' il primo campionamento, prima che qualcosa conti.
 *
 * Sono letture, quindi non cambiano lo stato dell'hardware: trascriverle costa
 * solo il fatto che il port fa due accessi che non sa spiegare. Il valore lo
 * fornisce l'oracolo, non e' cablato qui.
 */
static void emit_core_shm_unexplained(void)
{
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x0092);
}

static void emit_core_counters_first(void)
{
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x077e);
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x077c);
	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x077e);
}

/*
 * La riga 0x3f dell'AMT riscritta a zero una seconda volta, e solo dove il
 * canale porta la guardia radar.
 *
 * Contata sulle riscritture della riga su tutti e 43 i segmenti a freddo: tre
 * dove la guardia non c'e', sei dove c'e'. I tre radar-meteo ne hanno cinque,
 * e la mancante e' una di quelle del bss-up, che quei segmenti non hanno. Il
 * confine e' ch140, lo stesso che danno i POLL di timeline.py: ch144 sta
 * a tre come ch36.
 *
 * Delle tre in piu' questa e' l'unica nel preambolo, subito dopo il primo
 * campionamento dei contatori. Le altre due cadono nell'attesa del check --
 * su cold05 a 6.7 s e a 69.6 s dall'inizio, cioe' ai due bordi dei sessanta
 * secondi -- e questo harness quella transizione non la modella: cac_pending
 * e' un booleano per l'intera corsa. Vedi docs/retrace-todo.md.
 *
 * Non c'e' un sito b43 che la emetta. La riga la scrive il core init di
 * b43/main.c, e il chiamante qui e' la sospensione del match durante il channel
 * availability check, che b43 non ha: e' il motivo per cui sta fra i doppioni
 * e non nel PHY.
 */
static void emit_core_amt_cac_suspend(void)
{
	if (!(g_chan.flags & IEEE80211_CHAN_RADAR))
		return;
	b43_test_emit_amt(0x3f, 0);
}

static void emit_core_hostflags(void)
{
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x005e, 0x0100);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0060, 0x0000);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0062, 0x0000);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0078, 0x0040);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00d4, 0x0080);
}

/*
 * The GPIO pins b43 registers a LED on, from the mounted SPROM: the rule of
 * b43_led_get_sprominfo() in b43/leds.c, and b43_map_led(),
 * which registers nothing for OFF, ON and INACTIVE. compare.py uses them to
 * tell the vendor's LED ops from the other GPIO ones; they match the
 * mask the stock driver writes to chipcommon 0x8c in the LED block of the
 * attach, on every board.
 */
static u16 led_pins(const struct ssb_sprom *s)
{
	static const u8 defaults[4] = {
		B43_LED_ACTIVITY, B43_LED_RADIO_B, B43_LED_RADIO_A, B43_LED_OFF,
	};
	u8 bh[4 + ARRAY_SIZE(s->gpio_ext)] = { s->gpio0, s->gpio1, s->gpio2, s->gpio3 };
	bool none = (bh[0] & bh[1] & bh[2] & bh[3]) == 0xff;
	u16 pins = 0;
	unsigned int i;

	memcpy(&bh[4], s->gpio_ext, sizeof(s->gpio_ext));
	for (i = 0; i < ARRAY_SIZE(bh); i++) {
		u8 b;

		if (i < 4 && none)
			b = defaults[i];
		else if (bh[i] == 0xff || (i >= 4 && !bh[i]))
			b = B43_LED_OFF;
		else
			b = bh[i] & B43_LED_BEHAVIOUR;
		if (b != B43_LED_OFF && b != B43_LED_ON && b != B43_LED_INACTIVE)
			pins |= 1u << i;
	}
	return pins;
}

static void emit_core_shm_macaddr(const struct board_profile *p)
{
	unsigned int i;

	for (i = 0; i < 6; i += 2)
		b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(0x078c + i),
				(u16)(p->macaddr[i] | (p->macaddr[i + 1] << 8)));
}

/*
 * Doppione di b43_amt_write(), che vive in b43/main.c.
 *
 * Emette il record logico e il traffico della riga: l'hook del tracer su
 * wlc_bmac_write_amt da' `AMT.WR idx=`, e sotto ci sono la lettura e la
 * riscrittura degli 8 byte della riga sul routing RCMTA. Quelle due il vendor
 * le fa sulla coppia objaddr/objdata, che nelle catture vecchie nessun
 * accessor agganciato copriva -- percio' qui non si emettevano. La ricattura
 * ha aggiunto `OBJ.BULKR`/`OBJ.BULKW`, che le coprono, e la riga si confronta
 * per intero: vedi b43_test_emit_amt() in wrap.c.
 *
 * Resta fuori dal confronto dove vanno le due righe in cima. Qui sono in coda
 * all'azzeramento, e nella cattura non ci sono: il ciclo di azzeramento sta a
 * #307-#622 e le righe con i flag a #13302 e #13943, dentro il bss-up, tirate
 * da `ADDRM.SET` -- che e' l'entrata che il core chiama e che questo harness
 * non modella. Nel frattempo il vendor rifa anche un azzeramento parziale di
 * 56 righe (idx 0x00-0x37). Vanno spostate la', non tenute qui per comodita';
 * finche' ci sono, il confronto le conta due volte sbagliate.
 *
 * Come per emit_core_shm_macaddr(): sta qui e non nel PHY perche' non e'
 * codice del PHY, e i valori sono quelli del core, cosi' se il core cambia
 * questo diventa sbagliato e il confronto lo dice.
 */
/*
 * Doppione delle celle che il port aggiunge al core init di b43/main.c.
 *
 * Divise in due perche' la traccia le mette in due punti distinti: ANTSWAP e
 * BTSFOFF nel blocco di chip init prima del chanspec (cold01 #650, #656), il
 * resto nel blocco di config MAC dopo l'init radio. L'ordine conta: mettere
 * una di queste dopo il chanspec quando il vendor la fa prima basta a far
 * perdere l'appaiamento.
 *
 * Le celle del secondo gruppo non sono rispecchiate: cadono fra #12197 e
 * #14172, dentro un blocco che l'harness non modella affatto, e emetterle
 * altrove le metterebbe nel posto sbagliato -- peggio che non emetterle. Il
 * core le scrive comunque, perche' quello che conta per l'hardware e' lo
 * stato finale; qui conta l'ordine, e non lo sappiamo riprodurre.
 */
static void emit_core_shm_chipinit(const struct board_profile *p)
{
	/* b43/main.c, core init */
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0080, 8);      /* MAXBFRAMES */
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x005c, 0x000a); /* ANTSWAP */

	/*
	 * Queste b43 le scrive gia', e mancavano solo perche' l'harness compila
	 * il PHY e non main.c. Nell'ordine di b43, che NON e' quello del vendor:
	 * b43 mette WLCOREREV, poi le host flag, poi MACHW; il vendor mette
	 * MACHW subito dopo WLCOREREV e le host flag molto piu' tardi, appena
	 * prima del chanspec. Vedi docs/retrace-todo.md.
	 */
	/*
	 * Modo operativo del core: INFRA e DISCPMQ, AP azzerato. Lo fa
	 * b43_adjust_opmode(); stava dentro il preambolo MAC del PHY (cold_mac_preamble) e
	 * l'abbiamo spostato qui, che e' dove il vendor lo emette -- fra
	 * ANTSWAP e WLCOREREV, non con la GPIO.CTL.
	 */
	b43_test_emit_mctrl((u32)~0x40060000u, 0x40020000);

	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0016, p->core_rev);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00c0,
			(u16)(p->mac_hw_cap & 0xffff));
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00c2,
			(u16)((p->mac_hw_cap >> 16) & 0xffff));
	/*
	 * I limiti di ritrasmissione, cold01 #655: parole 6 e 7 dello scratch,
	 * che wl-diag stampa a 0x0018 e 0x001c. Non sono BTL0 e BTSFOFF, le
	 * celle della shared memory agli stessi indirizzi, che il vendor scrive
	 * piu' tardi nella config BSS. b43 li scrive in b43_set_retry_limits();
	 * il lungo e' 6, dove mac80211 ne da' 4 a b43.
	 */
	b43_shm_write16(&g_wldev, B43_SHM_SCRATCH, B43_SHM_SC_SRLIMIT, 7);
	b43_shm_write16(&g_wldev, B43_SHM_SCRATCH, B43_SHM_SC_LRLIMIT, 6);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0044, 3);      /* SFFBLIM */
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0046, 2);      /* LFFBLIM */
}

/*
 * Il template probe response: PRTLEN, l'SSID `test-ap` con gli zeri fino a
 * 0x017e, e PRSSIDLEN. E' la parte che il vendor ripete in tutte e quattro le
 * passate conf_tx, cold01 #14189, #14268 e #14347 oltre alla prima.
 */
/*
 * Il template della probe response NON si carica: b43 non fa rispondere il
 * firmware ai probe -- PRMAXTIME=1 in b43_wireless_core_init(), main.c:4918 --
 * e coerentemente non scrive nessuna delle tre celle del template. Le op del
 * vendor su PRTLEN, PRSSID, PRSSIDLEN, sulle temporizzazioni 0x0180-0x0186 e
 * sulla word 0x0700 di template RAM stanno in SOLO_VENDOR di compare.py.
 *
 * E' una scelta del WIP, non un limite: vedi "Probe-response offload" in
 * docs/retrace-todo.md. Quando l'offload verra' implementato questo doppione
 * torna, la voce di SOLO_VENDOR va togliata, e il carico va scritto in
 * main.c del kernel -- non qui.
 */

/*
 * La coda comune alle due passate che caricano un template beacon: TIMBPOS, il
 * template in template RAM e la sua lunghezza (@btl, 0x0018 per il primo e
 * 0x001a per il secondo). I template beacon b43 li fa: b43_upload_beacon0() e
 * b43_upload_beacon1() passano da b43_write_template_common().
 */
/*
 * Lunghezza del template beacon. Cresce di un byte per passo di larghezza --
 * 0x012a a 20 MHz, 0x012b a 40, 0x012c a 80 su tutti i segmenti -- perche'
 * l'elemento VHT operation cambia. La word scritta in template RAM invece e'
 * 0x012c fissa, a ogni larghezza.
 */
/*
 * The template RAM write that precedes the length is the whole template, in
 * 32-bit words: TPL.RAMW carries its byte count, the length rounded up to 4.
 */
static u16 beacon_tpl_len(void)
{
	enum nl80211_chan_width w = g_wldev.phy.chandef->width;

	/*
	 * Parte fissa piu' l'SSID: 0x012a era questo con sette caratteri.
	 */
	return (u16)(0x0123 + g_ssid_len +
		     (w == NL80211_CHAN_WIDTH_80 ? 2 :
		      w == NL80211_CHAN_WIDTH_40 ? 1 : 0));
}

/*
 * b43_update_templates() del core, per la fase probe. Le celle della probe
 * response non ci sono: b43 non fa l'offload, e stanno in SOLO_VENDOR di
 * compare.py -- vedi il TODO post-WIP.
 */
/*
 * Doppione di b43_ac_cac_match_gate() del core: la riga AMT del BSS ai due
 * confini del check, chiusa dal config che sintonizza col radar e riaperta dal
 * beacon. La forma e' quella della cattura -- il record logico e il traffico
 * della riga -- ed e' la stessa di emit_core_amt_cac_suspend(), che copre la
 * terza occorrenza, nel preambolo.
 */
static void emit_core_cac_gate(bool open)
{
	/*
	 * I cinque FIFO TX nell'ordine del vendor: 1, 3, 0, 2, 4. In chiusura
	 * ciascuno e' preceduto da una lettura di MACCONTROL, in apertura no.
	 */
	static const u16 txctl[] = { 0x0240, 0x02c0, 0x0200, 0x0280, 0x0300 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(txctl); i++) {
		u32 v;

		if (!open)
			b43_read32(&g_wldev, B43_MMIO_MACCTL);
		v = b43_read32(&g_wldev, txctl[i]);
		b43_write32(&g_wldev, txctl[i],
			    open ? v & ~B43_DMA64_TXSUSPEND : v | B43_DMA64_TXSUSPEND);
	}
	b43_test_emit_amt(0x3f, open ? 0x8008 : 0);
}

/*
 * Doppione di b43_update_templates() del core, che mac80211 chiama quando il
 * beacon cambia. @which alterna beacon0 e beacon1 e la serie parte da beacon0.
 */
static void emit_core_beacon_reload(unsigned int which)
{
	struct b43_wldev *dev = &g_wldev;
	static bool late_head_done;
	u16 btl = (which & 1) ? 0x001a : 0x0018;

	/*
	 * Testa della prima ricarica tardiva e solo di quella: quattro celle a
	 * 0x2637, una volta in tutto il segmento. La condizione e' "la prima
	 * tardiva" e non "indice zero" perche' l'indice ora conta anche le
	 * ricariche del blocco BSS e delle passate conf_tx, che la testa non
	 * hanno: su cold05 la prima tardiva e' la sesta del segmento.
	 */
	if (!late_head_done) {
		u16 off;

		for (off = 0x0300; off <= 0x0306; off += 2)
			b43_shm_write16(dev, B43_SHM_SHARED, off, 0x2637);
	}

	b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc,
			b43_shm_read16(dev, B43_SHM_SHARED, 0x00cc));
	/* Parte fissa piu' l'SSID, come in emit_core_bss_ssid(). */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x001e, (u16)(0x003c + g_ssid_len));
	b43_test_tplram_write16(btl == 0x0018 ? 0x0200 : 0x0480,
				(beacon_tpl_len() + 3) & ~3);
	b43_shm_write16(dev, B43_SHM_SHARED, btl, beacon_tpl_len());

	b43_mac_suspend(dev);
	b43_phy_ac_prb_rsp_plcp_pass(dev, g_ssid_len);
	b43_mac_enable(dev);

	/*
	 * Coda della prima ricarica e solo di quella, come la testa: tre celle
	 * a zero, una volta in tutto il segmento.
	 */
	if (!late_head_done) {
		late_head_done = true;
		b43_shm_write16(dev, B43_SHM_SHARED, 0x00a4, 0x0000);
		b43_shm_write16(dev, B43_SHM_SHARED, 0x00b4, 0x0000);
		b43_shm_write16(dev, B43_SHM_SHARED, 0x00d6, 0x0000);
	}
}

static void emit_core_bss_ssid(u16 btl)
{
	/* Parte fissa piu' l'SSID: 0x0043 era questo con sette caratteri. */
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x001e,
			(u16)(0x003c + g_ssid_len));
	/*
	 * L'offset del template segue la cella della lunghezza: BTL0 carica
	 * beacon0 a 0x0200, BTL1 carica beacon1 a 0x0480. Non sono le costanti
	 * BT_BASE0/BT_BASE1 di b43.h, che valgono 0x0068 e 0x0468: il layout
	 * della template RAM dell'AC non e' quello del firmware v4.
	 */
	b43_test_tplram_write16(btl == 0x0018 ? 0x0200 : 0x0480,
				(beacon_tpl_len() + 3) & ~3);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, btl, beacon_tpl_len());
}

/*
 * Configurazione BSS del core, cold01 #13593-#13624.
 *
 * Sta fra le due meta' del setup di canale: in b43 la fa il core dentro
 * b43_op_config(), fra il ritorno di b43_switch_channel() e la chiamata a
 * b43_phy_txpower_check(). Il vincolo di posizione e' reale e non un
 * allineamento di traccia: l'ucode legge SSID e lunghezze dei template quando
 * trasmette, quindi devono esserci prima che il MAC riparta -- e il MAC qui e'
 * ancora sospeso, l'enable arriva dopo.
 *
 * L'SSID e' quello della cattura, `test-ap`, con zeri fino a 0x017e: b43 non lo
 * scrive in shared memory, in AP mode sta nel template beacon, quindi qui e' un
 * valore della cattura e non un derivato. Le due lunghezze sono 0x0018
 * (B43_SHM_SH_BTL0) e 0x004a (B43_SHM_SH_PRTLEN), nomi di b43.h, che il core
 * scrive dal percorso dei template. Le TPL.RAMW che il vendor emette fra loro
 * non si riproducono: sono gia' dichiarate fra le op che nessun codice PHY di
 * b43 emette, e il confronto le scarta.
 */
static void emit_core_bss_config(void)
{
	/*
	 * Solo le celle che il perimetro conta. Le altre del blocco --
	 * 0x0012 DTIMPER, 0x0018 BTL0, 0x001e TIMBPOS, 0x0022 ACKCTSPHYCTL,
	 * 0x0048 PRSSIDLEN, 0x004a PRTLEN e l'SSID 0x0160-0x017e -- sono
	 * dichiarate del core in CORE_SHM e restano la': il vendor le scrive
	 * anche al chip init, dove il port non arriva, quindi toglierle dal
	 * perimetro scoprirebbe op che non emettiamo e farebbe crollare il
	 * confronto posizionale all'inizio della traccia. Provato: il muro
	 * torna a @72.
	 */
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0020, 0x0000);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0022, 0x0000);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x0012, 0x0003);

	b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x00cc);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00cc,
			b43_phy_ac_bss_cc(&g_wldev));
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00cc,
			b43_phy_ac_bss_cc(&g_wldev) | 0x0001);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00ce,
			b43_phy_ac_beacon_pwr_offset(&g_wldev));
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00d0, 0x0000);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x001c, 0x003a);
	emit_core_bss_ssid(0x0018);
	g_beacon_reloads++;
	/*
	 * I PLCP chiudono ogni caricamento di template, questo compreso:
	 * cold01 li mette a #13625, subito dopo PRSSIDLEN, e poi di nuovo in
	 * fondo a ognuna delle quattro passate conf_tx.
	 */
	b43_phy_ac_prb_rsp_plcp_pass(&g_wldev, g_ssid_len);
}

/*
 * Seconda passata della config BSS, cold01 #14103-#14127: il secondo template
 * beacon. Differisce dalla prima per la lunghezza -- 0x001a (BTL1) invece di
 * 0x0018 (BTL0), come b43_upload_beacon1() contro b43_upload_beacon0() -- e
 * perche' 0x00cc riceve una scrittura sola invece di due.
 */
static void emit_core_bss_config1(void)
{
	u16 cc = b43_shm_read16(&g_wldev, B43_SHM_SHARED, 0x00cc);

	b43_shm_write16(&g_wldev, B43_SHM_SHARED, 0x00cc, cc);
	emit_core_bss_ssid((g_beacon_reloads & 1) ? 0x001a : 0x0018);
	g_beacon_reloads++;
}

/*
 * Parametri EDCF di una coda di accesso, cold01 #14085-#14102 e i tre blocchi
 * gemelli a #14170, #14249 e #14328.
 *
 * Otto parole per coda piu' otto zeri fino a +0x1e, precedute dalla lettura
 * della cella di stato a +0x0e. Il layout e' quello di
 * b43_qos_params_upload(): TXOP, CWMIN, CWMAX, CWCUR, AIFS, BSLOTS, REGGAP,
 * STATUS.
 *
 * I valori sono i default EDCA -- TXOP 3008 e 1504 us per video e voce e zero
 * per best effort e background, CWMIN 15/15/7/3, AIFS 3/7/1/1 -- e REGGAP e'
 * AIFS + BSLOTS su tutte e quattro. BSLOTS e' il backoff estratto a caso e
 * quindi non si deriva: sono quelli della cattura, e su un'altra cattura
 * saranno altri.
 *
 * L'ordine delle code e' quello che il vendor emette, non 0..3.
 */
struct edcf_queue {
	u16 base;
	u16 txop;
	u16 cwmin;
	u16 cwmax;
	u16 aifs;
	u16 bslots;
};

static const struct edcf_queue edcf_queues[] = {
	{ 0x0260, 0x0000, 15,   63, 3, 9 },	/* best effort */
	{ 0x0240, 0x0000, 15, 1023, 7, 0 },	/* background */
	{ 0x0280, 0x0bc0,  7,   15, 1, 2 },	/* video */
	{ 0x02a0, 0x05e0,  3,    7, 1, 0 },	/* voce */
};

static void emit_core_edcf_queue(const struct edcf_queue *q)
{
	u16 off;

	b43_shm_read16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x0e));
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x00), q->txop);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x02), q->cwmin);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x04), q->cwmax);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x06), q->cwmin);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x08), q->aifs);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x0a), q->bslots);
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x0c),
			(u16)(q->aifs + q->bslots));
	b43_shm_write16(&g_wldev, B43_SHM_SHARED, (u16)(q->base + 0x0e), 0x0100);
	for (off = 0x10; off <= 0x1e; off += 2)
		b43_shm_write16(&g_wldev, B43_SHM_SHARED,
				(u16)(q->base + off), 0x0000);
}

/*
 * Una passata conf_tx, cold01 #14083-#14167 e le tre gemelle.
 *
 * Sta fra le due meta' della coda del setup di canale, dentro la sua parentesi
 * enable/suspend: i parametri della coda, il template, e i PLCP degli otto
 * rate che il PHY calcola dalla lunghezza del probe response. La prima passata
 * carica anche il secondo template beacon; le altre tre ripetono solo il probe
 * response.
 */
static void emit_core_conf_tx_pass(unsigned int n)
{
	/*
	 * L'enable e il suspend sono un impulso e stanno prima del carico, non
	 * intorno: cold01 #14083-#14084 e poi #14085 che apre i parametri
	 * della coda. Provato ad avvolgere il carico e il posizionale si
	 * ferma la', a @11822, con il vendor sul suspend e il port sulla
	 * lettura dei parametri.
	 */
	b43_mac_enable(&g_wldev);
	b43_mac_suspend(&g_wldev);
	emit_core_edcf_queue(&edcf_queues[n]);
	if (g_edcf_reload_mask & (1u << n))
		emit_core_bss_config1();
	b43_phy_ac_prb_rsp_plcp_pass(&g_wldev, g_ssid_len);
}

static void emit_core_conf_tx_passes(void)
{
	unsigned int q;

	for (q = 0; q < ARRAY_SIZE(edcf_queues); q++)
		emit_core_conf_tx_pass(q);

	/*
	 * Un quinto impulso, questo senza carico, prima che la coda del setup
	 * riprenda: cold01 lo mette subito prima del peek su PHY 0x019e della
	 * seconda meta'. E' l'impulso che il ciclo di channel_setup_tail
	 * contava insieme agli altri quattro.
	 *
	 * Il suspend dell'impulso e' quello della lettura di temperatura con
	 * cui channel_calibrate apre. Senza lettura (phycal_tempdelta 0, i
	 * quattro segmenti del secondo boot del TG789vac) il vendor emette solo
	 * l'enable e calibra col MAC acceso: qui il MAC resta sospeso e
	 * quell'enable lo emette channel_calibrate.
	 */
	if (g_sprom.phycal_tempdelta) {
		b43_mac_enable(&g_wldev);
		b43_mac_suspend(&g_wldev);
	}
}

static void emit_core_amt(const struct board_profile *p)
{
	u16 i;

	/* Azzeramento di tutte le righe, come fa il core init. */
	for (i = 0; i < 64; i++)
		b43_test_emit_amt(i, 0);

	/*
	 * Le due righe in cima -- BSSID e indirizzo di stazione -- non si
	 * emettono qui. Stavano in coda a questo ciclo, e la cattura non le ha
	 * la': l'azzeramento sta a #307-#622 e le righe con i flag a #13302 e
	 * #13943, dentro il bss-up e passando da `ADDRM.SET`. Tenerle qui
	 * costava sei op di troppo nel punto sbagliato, che e' peggio che non
	 * emetterle: il bss-up questo harness non lo modella, e finche' non lo
	 * modella la riga giusta e' nessuna riga.
	 */
	(void)p;
}

static void run_full(void)
{
	/*
	 * Manca la scrittura del chanspec in shared memory (OBJ.WR 0x00a0), che
	 * il flow `down` fa e questo no. Non e' una riga da aggiungere qui: la
	 * cattura la mette a #691, dopo le due unita' AFE dell'attach e la
	 * config MAC del core, mentre questo flow apre con una sola
	 * switch_analog e il suo prologo save-gain. Metterla in testa la
	 * piazza nel posto sbagliato -- provato. Va risolta insieme
	 * all'ordinamento della testa dell'attach, cioe' alla doppia
	 * programmazione dell'analogico di docs/retrace-todo.md.
	 */
	/*
	 * Le tre entrate che b43 fa prima di b43_phy_init(): il reset
	 * dell'attach (main.c:1455 da 5650), la coda dell'attach (5664) e il
	 * reset del core-init (1455 da 4956). Il reset dell'attach porta la
	 * parte del preambolo che il vendor fa nel proprio attach -- il banco
	 * AFE_ON due volte (#557, #576), le sette clear, la richiesta del PMU --
	 * e il resto lo porta l'entrata di b43_phy_init(); le altre due non
	 * emettono niente. Il banco AFE_ON torna solo nel bss-up (#36534) e il
	 * banco AFE_DOWN compare una sola volta, a #1262, che e' mode_init.
	 *
	 * Stanno qui perche' senza di loro il difetto non era visibile:
	 * l'harness chiamava switch_analog una volta sola, la fredda, e la
	 * traccia tornava mentre b43 intero emetteva 62 op in piu' e svuotava
	 * le code dell'oracolo sui dieci registri di salvataggio prima che il
	 * preambolo vero le usasse. Se una di queste tre ricomincia a emettere,
	 * il confronto lo dice subito.
	 */
	{
		const struct cfg80211_chan_def *saved = g_wldev.phy.chandef;

		/*
		 * Senza canale, come le vede b43: phy->chandef lo assegna
		 * b43_phy_init() (phy_common.c:93) sulla riga prima della sua
		 * switch_analog, e nessuno prima. Il preambolo pesca proprio
		 * quello per riconoscere l'entrata giusta, quindi tenerlo
		 * assegnato qui faceva cadere il preambolo sulla prima entrata
		 * e non sulla quarta.
		 */
		g_wldev.phy.chandef = NULL;
		b43_phyops_ac.switch_analog(&g_wldev, true);
		b43_phyops_ac.switch_analog(&g_wldev, false);
		b43_phyops_ac.switch_analog(&g_wldev, true);
		g_wldev.phy.chandef = saved;
	}

	/* L'entrata di b43_phy_init(), quella che porta il preambolo. */
	b43_phyops_ac.switch_analog(&g_wldev, true);
	/*
	 * L'ordine e' quello della traccia, e conta piu' dei valori: una sola
	 * inversione fa scartare l'op dal confronto. Su cold01: AMT #443,
	 * blocco di chip init #649-#658, MAC in shared memory #661-#663, host
	 * flag #686-#690, chanspec #691 (quello lo scrive il driver, in
	 * op_software_rfkill).
	 */
	emit_core_amt(g_profile);
	emit_core_shm_chipinit(g_profile);
	emit_core_shm_unexplained();
	emit_core_shm_macaddr(g_profile);
	emit_core_counters_first();
	emit_core_amt_cac_suspend();
	emit_core_hostflags();
	run_rfkill();
	run_op_init();
	/*
	 * b43_phy_init azzera phy->do_full_init fra ops->init e switch_channel,
	 * quindi il codice di switch_channel non vede mai il flag alzato -- gatare
	 * la' su do_full_init non ha effetto nel driver vero.
	 */
	g_wldev.phy.do_full_init = false;
	run_switch_channel();
}

int main(int argc, char **argv)
{
	const char *flow  = (argc > 1) ? argv[1] : "full";
	const char *board = (argc > 2) ? argv[2] : "d6220";

	const struct board_profile *p = board_profile_lookup(board);

	fprintf(stderr, "test: board=%s flow=%s\n", p->name, flow);
	mount_board(p);
	if (!strcmp(flow, "led_pins")) {
		printf("0x%04x\n", led_pins(&g_sprom));
		return 0;
	}
	b43_test_plans_reset();
	plan_rxiq_poll(p->name, g_wldev.phy.do_full_init);
	g_wldev.mac_suspended = 1;
	b43_test_trace_to(stdout);

	/*
	 * PLL state the PMU brings up before the driver runs, per chip:
	 * PLLCTL2=0x0c31 on both, PLLCTL3=0x00133333 on 4352 / 0x100e on 4360.
	 * op_init verifies PLLCTL3 on the 4352, so seed it to the real value.
	 */
	b43_test_pll_set(2, 0x0c31);
	b43_test_pll_set(3, p->chip_id == 0x4360 ? 0x100e : 0x00133333);

	/*
	 * Pre-populate radio-mirror slots set by earlier init flows we don't
	 * re-execute here. The RCCAL comparators (R2069_RCCAL_E/F) are
	 * written by the RC-cal engine during b43_radio_2069_init and later
	 * read by rccal to derive dev->phy.ac->lpf_cap0/1. Per-board values
	 * (see the profile) so each unit's real measurement is served.
	 */
	b43_test_mirror_radio_set(0x0414, p->rccal_e);  /* R2069_RCCAL_E */
	b43_test_mirror_radio_set(0x0415, p->rccal_f);  /* R2069_RCCAL_F */

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
	 * Radio pre-state per tempsense_radio_setup (vendor #39641-#39708).
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
		b43_phy_ac_rxiqcal_est_debug(&g_wldev);
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

		int r = b43_phy_ac_rxiqcal_comp_update(&g_wldev, 0x07);
		fprintf(stderr, "test: rx_iq_comp_update returned %d\n", r);
	} else if (!strcmp(flow, "op_init")) {
		run_op_init();
	} else if (!strcmp(flow, "up")) {
		/*
		 * The up half of a sweep cycle, from the segment boundary on.
		 *
		 * Same reason the down phase has its own flow: switch_analog()
		 * calls rx_gain_regs_program(), whose eleven reads happen before
		 * a segment starts, so running them here takes the oracle queue
		 * entries for 0x073a, 0x0725 and their neighbours that the up
		 * phase itself needs. Everything after the radio init is the
		 * same as "full".
		 */
		/*
		 * A warm segment is not a first bring-up and the vendor's
		 * init_regs takes its two-pass branch there, so do_full_init has
		 * to be clear before op_init and not just before
		 * switch_channel. A cold segment is the opposite, and
		 * AC_FIRST_INIT picks between them: the cold sweep has one
		 * module load per channel, so both cases are now captured.
		 */
		{
			const char *e = getenv("AC_FIRST_INIT");

			g_wldev.phy.do_full_init = e && strtoul(e, NULL, 10);
		}
		run_rfkill();
		run_op_init();
		run_switch_channel();
	} else if (!strcmp(flow, "down")) {
		/*
		 * The down phase on its own: the radio init that heads a sweep
		 * segment, episodes 72 to 232 of an up cycle. 130 ops.
		 *
		 * It gets its own flow because the read oracle is a per-address
		 * queue and every flow that reaches this code emits a save-gain
		 * prologue first. Those reads touch 0x0728, 0x0720 and 0x0408
		 * and take the queue entries the down phase itself needs, so the
		 * phase then reads the *second* value for each and every
		 * read-modify-write after it lands one field off. Comparing the
		 * phase in isolation is the only way its reads line up with the
		 * capture.
		 */
		g_ac.status_mask = 0;
		/*
		 * set_channel caches these and this flow does not run it, so the
		 * chanspec and the MAC bandwidth write need them seeded from
		 * AC_CHANNEL and AC_BW.
		 */
		g_ac.cal_channel = g_chan.hw_value;
		g_ac.cal_width = g_hw.conf.chandef.width;
		g_ac.cal_freq = g_hw.conf.chandef.center_freq1;
		/*
		 * The caller writes the chanspec, and with it the MAC width,
		 * just before the radio init rather than inside it: in the
		 * attach capture that carries the OBJ class the chanspec is at
		 * episode 35395 and the first op of b43_radio_2069_init() at
		 * 35396, with the prefregs at 35420. The driver writes it from
		 * b43_phy_ac_op_software_rfkill(), which this flow does not
		 * run: keep the two in step.
		 */
		b43_phy_ac_write_chanspec(&g_wldev);
		{
			static const u16 rccal_stat[] = {
				0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0010,
				0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0010,
				0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0010,
			};
			b43_test_plan_radio_reads(0x0413, rccal_stat,
						  ARRAY_SIZE(rccal_stat));
		}
		b43_radio_2069_init(&g_wldev);
	} else if (!strcmp(flow, "rfkill")) {
		run_rfkill();
	} else if (!strcmp(flow, "switch_channel")) {
		/*
		 * Da solo questo flow modella un cambio di canale a runtime, non
		 * un bring-up: lo slice vendor di riferimento (32887:55154) ha il
		 * MAC attivo all'ingresso e lo vede sospendere e riabilitare. In
		 * `full` il valore non va toccato: ci arriva a 1 dal preambolo,
		 * che spegne il MAC con una write diretta.
		 */
		g_wldev.mac_suspended = 0;
		run_switch_channel();
	} else if (!strcmp(flow, "periodic")) {
		/*
		 * Un tick del watchdog a regime, con la tornata di measure
		 * block (noise cal). Riferimento: sweep d6220, tick
		 * #33124-#33879 di cold01 (ch36 BW20, dopo il primo up del
		 * segmento), estratto in router-data/d6220/
		 * wl-diag-wl1-steady-tick-ch36-bw20.txt — file che fa sia da
		 * AC_READ_ORACLE (i valori SHM/TSSI sono stato ucode, non
		 * derivabile) sia da riferimento per compare.py.
		 *
		 * Stato entrante: MAC attivo (come switch_channel), PHY in
		 * release — il REQUIRE della tempsense vuole
		 * RX_WAITED|RX_OFDM e niente CLIP_ALL_DIS/CCA_RESET/RX_CCK.
		 *
		 * Il toggle di 0x0520[3:2] fa parte dello stato entrante come
		 * gli altri due, e la sua fase e' una proprieta' dell'estratto,
		 * non una costante del driver: l'alternanza parte da 0x0000 al
		 * primo cambio di modo del segmento e non si azzera mai piu'
		 * (26 cambi su cold01, alternanza intera senza rotture), quindi
		 * il valore di un tick a meta' flusso e' deciso dalla parita'
		 * del suo indice. Il tick estratto e' il decimo, e scrive
		 * 0x0004. Un estratto preso a un altro canale cade su un altro
		 * indice, perche' quante calibrazioni lo precedono cambia con
		 * la famiglia, e li' la fase va ricontata sul segmento.
		 */
		g_wldev.mac_suspended = 0;
		g_ac.status_mask = B43_PHY_AC_STATE_RX_WAITED |
				   B43_PHY_AC_STATE_RX_OFDM;
		g_ac.probe_mode = 0x0004;
		g_ac.wd_turns = 9;
		g_ac.wd_switch_turns = 9;
		/*
		 * The CRS state the segment carries into this tick: the block
		 * last written at #31414 (0x34, bank 0x0500) from chain 0 at
		 * ladder index 1 and chain 1 at 3, and the five latches since
		 * (#32019-#33110) all on the same two indices. The tick's own
		 * sample lands there too, so the block is not rewritten;
		 * without this state the rings are empty and the sample alone
		 * reads as a move.
		 */
		seed_crs_rings(1);
		seed_crs_chain(1, 3);
		b43_phy_ac_watchdog(&g_wldev);
		/*
		 * Il latch della finestra non e' la coda del giro: arriva col
		 * campione di rumore, 2 ms dopo e su un'altra CPU, e a freddo
		 * lo porta l'evento NOISE della timeline. Qui la timeline non
		 * c'e' e lo consegna il flow, o le ultime otto op
		 * dell'estratto non hanno chi le emetta.
		 */
		b43_phy_ac_noise_sample_done(&g_wldev);
	} else if (!strcmp(flow, "crsmin")) {
		/*
		 * Self-test della catena crsmin (non usa oracolo). Esercita
		 * pwork_60sec tre volte: le prime due (cal_cycles 0->1->2)
		 * sono "a freddo" e scrivono la ladder bumped (+4); dalla
		 * terza il bump sparisce. Atteso, ch36 BW20 (idx 1 -> 48):
		 * cold 52 (=0x34, il valore d'attach osservato), poi 48 (=0x30).
		 *
		 * Il ciclo 1 calcola la stessa soglia del ciclo 0, e l'hook
		 * deve saltare la scrittura invece di riemettere gli otto
		 * registri: e' quello che fa il vendor, che riscrive solo
		 * quando l'indice della scala si muove. Leggere il mirror non
		 * lo distinguerebbe -- il valore e' lo stesso in entrambi i
		 * casi -- quindi prima di quel ciclo si semina un sentinella e
		 * si controlla che resti.
		 */
		static const u8 want[3] = { 0x34, 0xff, 0x30 };
		unsigned int k;
		int fails = 0;

		/*
		 * Lo stato che la catena avrebbe lasciato, e che questo flow non
		 * produce: sono le due cose che AC_CRS_INDEX e AC_CRS_SUBBAND
		 * seminano per gli altri flow. Il canale c'e' -- lo imposta il
		 * preambolo -- ma il sub-band in forza parte dalla sentinella
		 * 0xff, che e' come dire "mai visto" e forza il riazzeramento
		 * dell'indice al primo giro, e l'indice lo muove il campione di
		 * rumore, che latcha il watchdog: questo flow non lo fa girare.
		 * Senza i due, la catena girava sull'indice 0 e il self-test
		 * falliva tutti e tre i cicli, misurando il primo scalino della
		 * scala mentre le attese sono quelle del secondo.
		 *
		 * Zero e' il gruppo pa5g di ch36 con subband5gver=4, cioe' il
		 * sub-band che il channel setup avrebbe lasciato in forza.
		 */
		g_ac.crs_subband = 0;
		seed_crs_rings(1);
		g_ac.cal_cycles = 0;

		for (k = 0; k < 3; k++) {
			u8 got;

			if (k == 1)
				b43_test_mirror_phy_set(0x0324, 0x00ff);

			b43_phyops_ac.pwork_60sec(&g_wldev);
			got = b43_test_mirror_phy_get(0x0324) & 0xff;
			fprintf(stderr,
				"crsmin: ciclo %u  CRS byte-basso=0x%02x atteso=0x%02x  %s%s\n",
				k, got, want[k], got == want[k] ? "OK" : "FAIL",
				k == 1 ? "  (sentinella: nessuna scrittura)" : "");
			if (got != want[k])
				fails++;
		}
		fprintf(stderr, "crsmin: %s\n", fails ? "FAIL" : "PASS");
		return fails ? 1 : 0;
	} else if (!strcmp(flow, "full")) {
		run_full();
	} else {
		fprintf(stderr, "test: unknown flow '%s'\n", flow);
		return 2;
	}

	fprintf(stderr, "test: plan consumption ---\n");
	b43_test_plans_report(stderr);
	b43_test_oracle_coverage_report();
	b43_test_oracle_report();
	return 0;
}
