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
 *   b43_radio_read_log() and land in a discarded variable. They reproduce the
 *   bus order the stock driver emits, so they are not dead code.
 * - "self-contained" describes a table write that opens the gate, writes and
 *   closes it within its own scope (b43_actab_*_scoped), as opposed to the
 *   fast variants that work inside a gate the caller holds open.
 * - where a comment says the order is the observed one, that order comes from
 *   a capture and cannot be rearranged; the op-for-op comparison checks it.
 *
 * The #NNNNN indices in the comments are episode positions in a reference
 * capture. An index alone does not identify one: the index spaces of the
 * captures overlap, so reverse-tools/check_trace_refs.py resolves most bare
 * indices to several different ops. Check a reference with
 * reverse-tools/check_ref_spans.py before relying on it.
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

/*
 * Periodic TX power recalculation.
 *
 * 1. CRS min power is ported, as b43_phy_ac_op_recalc_txpower(), the core's
 *    hook. The high byte of 0x0324 and friends is the one-shot reset already
 *    done in op_switch_channel; the low byte is the recalculated threshold,
 *    derived from the crsmin chain verified against the D6220 7.14 blob
 *    (ladder, per-bandwidth anchoring, clamp and cold bump). The one input
 *    that cannot be reproduced without hardware is the interference sample
 *    per freq_range: it is pinned here to the steady-state low-5 GHz value
 *    and has to be replaced by the measurement on real hardware.
 *
 * 2. The periodic cycle on 0x0725/0x0925 is ported as the measure block
 *    inside b43_phy_ac_watchdog(), the pwork_15sec hook, on the vendor's
 *    period of roughly five seconds.
 *
 * adjust_txpower stays a stub: no capture shows an adjust phase distinct
 * from the recalc, and the b43 core only calls it when recalc returns
 * NEED_ADJUST, which does not happen here.
 */
static void b43_phy_ac_op_adjust_txpower(struct b43_wldev *dev)
{
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

	b43_phy_write(dev, 0x1728, 0x0080);
	b43_phy_write(dev, 0x1720, 0x0180);
	b43_phy_write(dev, 0x1729, 0x0000);
	b43_phy_write(dev, 0x1721, 0x5000);

	/*
	 * RMW pairs: read the base-page register, OR in the bit, write to the
	 * 0x17xx-page mirror. On the reference board both base values read 0,
	 * so the OR-in bit alone lands in the mirror. Each read is issued
	 * immediately before its dependent write; the vendor trace shows the
	 * same interleaving (d6220 attach #32847-32850).
	 */
	b43_phy_write(dev, 0x173a, b43_phy_read_log(dev, 0x073a) | 0x0100);
	b43_phy_write(dev, 0x1725, b43_phy_read_log(dev, B43_PHY_AC_AFE_C1) | 0x0400);
}

/*
 * Initial ADC gain words. Both the value pair and the number of passes
 * follow the bring-up phase, not the chip. adc_reset() later rewrites
 * 0x03ac and 0x032c.
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
 * Idle-TSSI: calibrate and commit the per-core base index. The index is
 * measured, not constant, and the loop is gated on the coremask. The capture
 * shows four calls per bring-up.
 */
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
static void b43_phy_ac_iq_acc_peek(struct b43_wldev *dev, unsigned int core);
static void b43_phy_ac_loopback_gain_search(struct b43_wldev *dev);
static void b43_phy_ac_pmu_req(struct b43_wldev *dev, bool on);
static void b43_phy_ac_probe_cycle(struct b43_wldev *dev, unsigned int n_iter,
				   bool extended_first, bool closes_sequence);
static void b43_phy_ac_wd_stats_clear(struct b43_wldev *dev);
static void b43_phy_ac_wd_stats_poll_opt(struct b43_wldev *dev,
					 bool head_sweep,
					 unsigned int ctr32_passes);
static unsigned int b43_phy_ac_po_band(u16 chan);
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
 * Lettura e riscrittura di OFDM_PCTL1 sugli otto blocchi dei rate OFDM.
 *
 * E' `brcms_upd_ofdm_pctl1_table()` di brcmsmac, che itera la stessa lista
 * esplicita di otto rate, legge `entry_ptr + M_RT_OFDM_PCTL1_POS`, ci rimette i
 * bit di modo STF e riscrive. A una catena `hw_stf_ss_opmode` e' zero, quindi
 * la modifica e' un no-op e la traccia mostra il valore letto tornare indietro.
 *
 * Il vendor la esegue due volte per attach: cold01 #12205, dentro il blocco di
 * config MAC, e #13414, subito dopo la scrittura di MHF3 in channel_setup().
 * Da qui le due chiamate.
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
 */
static void b43_phy_ac_shm_readback_block(struct b43_wldev *dev)
{
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

	b43_phy_ac_ofdm_pctl1_readback(dev);
}

/*
 * Parte invariante del blocco di config MAC, subito dopo i due azzeramenti.
 *
 * Ogni valore qui e' lo stesso su tutti e 26 i segmenti dello sweep a freddo
 * del d6220, a ogni canale e a ogni larghezza -- classificati `invariante` da
 * reverse-tools/decorrelate_channels.py, che sulle stesse catture non trova
 * nemmeno una chiave dinamica. Sono trascritti: a cosa servano non e' noto.
 *
 * Non sono qui, e restano da fare, le due famiglie non invarianti dello stesso
 * blocco: le quindici celle a passo 0x14 che dipendono dalla larghezza e le
 * dodici a passo 0x1c che dipendono dalla frequenza centrale. Fuori restano
 * anche la lettura di 0x00b0, che b43.h dichiara EXTNPHYCTL del core, e
 * 0x05dc, che cade nel blocco KEYIDXBLOCK: di quelle due non e' stabilito chi
 * le debba emettere.
 */
/*
 * Campo a +0x0e del blocco per-rate: offset di potenza del rate, in ottavi.
 *
 * Per l'indirizzo vedi b43_phy_ac_rate_shm_offset(). brcmsmac scrive gli
 * offset 10, 12 e 16 dello stesso blocco -- PLCP della probe response e durata
 * -- e non il 14.
 *
 * Il valore e' una distanza dal massimo, come nel PPR di phy_n: con
 *
 *     ppr[i] = maxp5ga[sb] - 2 * nib[i]        (phy_n.c srom_convert)
 *     max    = maxp5ga[sb] - 2 * min(nib)      (get_max sul PPR appena caricato)
 *
 * il campo vale `(max - ppr[i]) / 2`, in cui maxp5ga si cancella e resta
 *
 *     nib[i] - min(nib)
 *
 * dove nib[i] e' il nibble di mcsbw*po del MCS su cui il rate legacy ricade --
 * 6, 9, 12 e 18 su mcs0, poi 24, 36, 48 e 54 su mcs1..4 -- e min e' preso su
 * tutti e otto i nibble del campo della larghezza operativa. I nibble sono
 * senza segno: con la conversione a intero con segno mcsbw205ghpo = 0xcca88440
 * dell'agcombo darebbe backoff negativi. Il risultato va moltiplicato per 8,
 * cioe' il campo e' in mezzi quarti di dB.
 *
 * Ricavato invertendo la catena sui due sweep a freddo, d6220 e agcombo, 52
 * configurazioni: 41 tornano esatte con questa forma.
 *
 * TODO: le altre 11 hanno due termini che questa forma non porta, e che
 * rompono entrambi la cancellazione di maxp5ga.
 *
 * Sette sono il bonus di densita' spettrale sulle larghezze legate, dove il
 * massimo sta *sopra* maxp5ga di 1 o 2 dB: allargando il canale il totale
 * ammesso cresce. L'incremento osservato non e' uniforme fra le due board -- 8
 * quarti sul d6220 a ch36 bw80, 4 sull'agcombo -- quindi non si trascrive.
 *
 * Ipotesi non verificata: il bonus scala col numero di catene. Il d6220 ha due
 * core e prende 8 quarti a 80 MHz, l'agcombo ne ha tre e ne prende 4; a 40 MHz
 * sono entrambi 4. Spiegherebbe la non uniformita' invece di constatarla, ma a
 * 80 MHz c'e' un solo punto per board.
 *
 * Quattro hanno il massimo *sotto* maxp5ga, cioe' il tetto di gruppo morde
 * prima che il massimo venga preso: agcombo ch36-48 bw20 e ch100 bw40, e d6220
 * ch100 bw40. Probabilmente e' solo un massimo piu' ristretto, ma quattro punti
 * non bastano a distinguerlo da un tetto applicato per gruppo; il full-sweep
 * del DSL e' il dato che serve, e ora ha senso girarlo perche' il modello non
 * ha piu' parametri liberi.
 *
 * Fuori restano i quattro rate CCK, le cui celle il vendor scrive prima di
 * queste: sono il gruppo cck[4] del PPR, senza campo SROM per 5 GHz nella rev
 * 11, quindi il loro valore viene da tetto e pavimento e da nient'altro -- che
 * e' la board-independence misurata su tre schede.
 */
/*
 * PLCP della probe response e sua durata, per ognuno degli otto rate OFDM.
 *
 * Il campo SIGNAL sta a +8 e +10 come due word, la durata a +12. Nessuno dei
 * due e' trascritto: si calcolano, ed e' lo stesso conto di
 * brcms_c_compute_ofdm_plcp() e brcms_c_calc_frame_time().
 *
 *   SIGNAL:  tmp = len << 5, poi plcp[0] = nibble_rate | (tmp & 0xff),
 *            plcp[1] = tmp >> 8, plcp[2] = tmp >> 16
 *   durata:  20 + ceil((len * 8 + 22) / NDBPS) * 4 + SIFS
 *
 * Verificato su cold01 al microsecondo per tutti e otto i rate: 420, 292, 228,
 * 164, 132, 100, 84 e 80 us con len = 284 e SIFS = 16.
 *
 * @len e' la lunghezza della probe response piu' l'FCS. Sul ferro va letta da
 * B43_SHM_SH_PRTLEN (0x004a), dove il core scrive la lunghezza del template --
 * nella cattura vale 0x0118, cioe' 280, e 280 + 4 di FCS fa i 284 usati qui.
 * Niente ancora la fornisce, quindi per ora:
 * qui e' quella delle catture, 284 byte a 20 MHz e un byte in piu' per ogni
 * raddoppio della larghezza. Quel +1 per larghezza non e' spiegato -- il
 * template a 20 MHz ne misura 280 nella TPL.RAMW, e 280 + 4 di FCS torna, ma a
 * 40 MHz la TPL.RAMW ne misura 284 e il SIGNAL dice 285.
 */
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
 * uguale": cold01 #13458-#13465 scrive 0x04c6 per 6 e 9, 0x04da per 12 e 18,
 * 0x04ee per 24, 36, 48 e 54. Non c'e' niente di trascritto, i puntatori
 * vengono dalla tabella.
 *
 * Il vendor la esegue due volte per attach: cold01 #13458, dentro il blocco di
 * config MAC, e #13585, dopo la terza spazzata. Solo la prima e' preceduta dalle
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

static void b43_phy_ac_prb_rsp_rate_po(struct b43_wldev *dev)
{
	struct b43_phy_ac *ac = dev->phy.ac;
	struct ssb_sprom *sprom = dev->dev->bus_sprom;
	unsigned int band = b43_phy_ac_po_band(ac->cal_channel);
	unsigned int i;
	u8 min_nib;
	u32 po;

	po = (ac->cal_width == NL80211_CHAN_WIDTH_40)
		? sprom->mcsbw5g_po[band].bw40
		: sprom->mcsbw5g_po[band].bw20;

	min_nib = 0xf;
	for (i = 0; i < 8; i++)
		min_nib = min_t(u8, min_nib, (po >> (4 * i)) & 0xf);

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
			val = (u16)((((po >> (4 * r->mcs)) & 0xf) - min_nib) * 8);

		b43_shm_read16(dev, B43_SHM_SHARED, cell);
		b43_shm_write16(dev, B43_SHM_SHARED, cell, val);
	}
}

static void b43_phy_ac_shm_mac_config_block(struct b43_wldev *dev)
{
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
 */
static void b43_phy_ac_classctl_write(struct b43_wldev *dev, bool arm)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;

	/*
	 * Bit 0x0800 is set at 20 MHz and clear above it: the captures write
	 * 0x0df4/0x0df6 at 20 MHz and 0x05f4/0x05f6 at 40 and 80, and the rest
	 * of the word does not move. Seventeen of the eighteen writes to this
	 * register follow that; the one that does not comes from elsewhere.
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
 * Core 0's idle-TSSI base index, transcribed and phase-dependent: on a first
 * bring-up 0x205 / 0x203 / 0x205 for iterations 1 to 3, on a later channel
 * setup 0x206 / 0x207 / 0x206. Core 1 is 0x200 in both phases.
 *
 * This is still a transcribed seed; deriving it from the readback is on the
 * to-do list.
 */
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

static void b43_phy_ac_idle_tssi_meas(struct b43_wldev *dev)
{
	B43_AC_FN();
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

	/* Abilitazione del path TSSI per catena, #38268-#38273. */
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
	/* The per-core fields of RF_SEQ_MODE are coremask and coremask << 12,
	 * not constants: 0x0003/0x3000 on a 2x2, 0x0007/0x7000 on a 3x3. */
	b43_phy_maskset(dev, 0x0401, (u16)~0x0007, dev->phy.ac->coremask);
	b43_phy_maskset(dev, 0x0401, (u16)~0x7000,
			(u16)(dev->phy.ac->coremask << 12));

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		u16 p = (u16)(core * 0x0200);
		u16 base_index;

		if (!((dev->phy.ac->coremask >> core) & 1))
			continue;

		/*
		 * The 0x0140 gate is already armed on entry, by the
		 * rx_gate_with_adc_hold(true) at the end of adc_reset(), and
		 * is released and re-armed at the *end* of the per-core body:
		 * the capture releases after core 0 and arms again before
		 * core 1. So no opening arm is needed here.
		 */
		/* Prologo per catena, #38329-#38339. */
		{
			u16 dummy_baseidx;

			b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_read(dev, 0x040f);
			b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
			b43_phy_read(dev, 0x0394);
			b43_phy_read(dev, 0x0393);

			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			b43_actab_read_bulk(dev, 0x0c, 0x63,
					    16, 1, &dummy_baseidx);
		}
		b43_phy_read(dev, 0x0747);
		b43_phy_read(dev, 0x0732);
		b43_phy_read(dev, 0x0733);
		b43_phy_read(dev, 0x0734);
		b43_phy_read(dev, 0x0722);
		b43_phy_read(dev, 0x0727);
		b43_phy_read(dev, 0x073c);
		/*
		 * Table read of id 0x0c offset 0x67: core 1's base index
		 * readback. Goes through actab_read_bulk() for the trace label.
		 */
		{
			u16 dummy_baseidx2;

			b43_actab_read_bulk(dev, 0x0c, 0x67, 16, 1, &dummy_baseidx2);
		}
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
			u16 tblr_dummy_3;
			b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1, &tblr_dummy_3);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		{
			u16 tblr_dummy_4;
			b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1, &tblr_dummy_4);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_5 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &tblw_val_5);
		}
		{
			static const u16 tblw_val_6 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &tblw_val_6);
		}
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_7 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &tblw_val_7);
		}
		{
			static const u16 tblw_val_8 = 0x0000;
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &tblw_val_8);
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
		b43_phy_mask(dev, 0x0460, (u16)~0x0004);	/* AND clr bit 2 */
		b43_phy_mask(dev, 0x0460, (u16)~0x0001);	/* AND clr bit 0 */
		b43_phy_mask(dev, 0x0382, (u16)~0xc000);	/* trace: AND clr bit14-15 (era OR) */
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
		b43_phy_read(dev, 0x0739);
		b43_phy_write(dev, 0x0739, 0x0080);
		b43_phy_read(dev, 0x073a);
		b43_phy_write(dev, 0x073a, 0x0180);
		b43_phy_read(dev, 0x0725);
		b43_phy_write(dev, 0x0725, 0x0604);
		b43_phy_read(dev, 0x0939);
		b43_phy_write(dev, 0x0939, 0x0080);
		b43_phy_read(dev, 0x093a);
		b43_phy_write(dev, 0x093a, 0x0180);
		b43_phy_read(dev, 0x0925);
		b43_phy_write(dev, 0x0925, 0x0604);
		b43_phy_write(dev, 0x0925, 0x0600);
		b43_phy_write(dev, 0x093a, 0x0180);
		b43_phy_write(dev, 0x0939, 0x0000);
		b43_phy_write(dev, 0x0725, 0x0600);
		b43_phy_write(dev, 0x073a, 0x0180);
		b43_phy_write(dev, 0x0739, 0x0000);
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
		b43_phy_mask(dev, 0x0460, (u16)~0x0004);	/* trace: AND clr bit2 (era OR) */
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
		b43_phy_write(dev, 0x0732, 0x0000);
		b43_phy_write(dev, 0x0733, 0x0000);
		b43_phy_write(dev, 0x0747, 0x0000);
		b43_phy_write(dev, 0x0722, 0x0000);
		/* 0x0000 on the 4352; the 4360, also 5 GHz only, writes 0x0029
		 * here, and likewise for 0x0934. A chip or board difference, not
		 * a band one. */
		b43_phy_write(dev, 0x0734, 0x0000);
		b43_phy_write(dev, 0x0727, 0x0004);
		b43_phy_write(dev, 0x073c, 0x0000);
		b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~(0x0002), (0x0002));
		{
			static const u16 tblw_val_13 = 0x0035;
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &tblw_val_13);
		}
		{
			static const u16 tblw_val_14 = 0x0035;
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &tblw_val_14);
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
			static const u16 tblw_val_15 = 0x0035;
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &tblw_val_15);
		}
		{
			static const u16 tblw_val_16 = 0x0035;
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &tblw_val_16);
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
 * by its primary alone, so the minimum over the block is what binds. That is
 * the part that matters here: it is the only stage with per-channel
 * granularity, and the two 40 MHz configurations this driver still gets wrong
 * are both cases where the primary's own limit is not the binding one.
 *
 * The vendor's own regulatory data confirms per-channel granularity is real
 * and used -- its channel-range table carries single-channel ranges such as
 * 36..36 and 44..44, and the 5 GHz locale tables reference them thousands of
 * times. None of that data is needed here: cfg80211 already supplies
 * max_power per channel, which is the same granularity.
 *
 * On the captures this driver is verified against, this stage does not bind:
 * ch100 receives 86 where a 21 dBm ceiling would give 84. So the op-for-op
 * match holds only for a regulatory domain at least as permissive as the one
 * the captures were taken under, which is a property of the system and not of
 * the driver.
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
/*
 * Sub-band index for the TX power ceiling.
 *
 * Keyed on the primary channel, not the centre: at 80 MHz the captures follow
 * the primary, and using the centre puts ch36 one group too high.
 *
 * The first boundary depends on the width, 5210 at 20 and 80 MHz and 5250 at
 * 40. That is not a fit dressed up as a rule -- the sign of the residuals
 * settles it. A regulatory limit can only lower a ceiling, so any residual
 * where this driver comes out *below* the vendor cannot be explained by the
 * regulatory stage that is still missing. At 40 MHz the 5210 boundary leaves
 * ch44 two units low, which nothing downstream could raise; 5250 leaves ch36
 * and ch52 two units high, which a missing clamp explains, and every other
 * 40 MHz configuration exact.
 *
 * Deliberately not b43_phy_ac_pa5g_group(), which implements the
 * subband5gver=4 split at 5250 for every width and feeds the pa5ga
 * coefficients. The two partitions coincide at 40 MHz and differ at 20, so
 * they are kept apart rather than one being bent to fit the other.
 *
 * The 20 MHz boundary rests on one board: it is pinned by ch40 giving 66 and
 * ch44 giving 64 with maxp5ga = {72, 70, ...}, and only the d6220 has those
 * two entries distinct.
 */
static unsigned int b43_phy_ac_txpwr_subband(u16 chan,
					     enum nl80211_chan_width width)
{
	u16 freq = 5000 + 5 * chan;
	u16 first = (width == NL80211_CHAN_WIDTH_40) ? 5250 : 5210;

	if (freq < first)
		return 0;
	if (freq < 5500)
		return 1;
	return 2;
}

/*
 * Is this the lowest 40 MHz block of its sub-band?
 *
 * The previous block's primary sits eight channels down; if that falls in a
 * different sub-band, or off the bottom of the band, this is the first.
 */
static bool b43_phy_ac_txpwr_first_block(u16 chan)
{
	if (chan < 44)
		return true;

	return b43_phy_ac_txpwr_subband(chan - 8, NL80211_CHAN_WIDTH_40) !=
	       b43_phy_ac_txpwr_subband(chan, NL80211_CHAN_WIDTH_40);
}


static unsigned int b43_phy_ac_po_band(u16 chan)
{
	if (chan < 52)
		return 0;
	if (chan < 100)
		return 1;
	return 2;
}

/*
 * Per-core TX power target, in quarter-dBm, for register 0x0646 + core stride.
 *
 * This is the reduction brcmsmac performs in wlc_phy_txpower_recalc_target():
 * a per-rate limit is built from the SROM, the regulatory ceiling and a fixed
 * 6-unit margin, and the register takes the maximum over rates while the
 * per-rate distances from that maximum go to table 0x21, the ppr array.
 *
 *   srom_max[rate] = maxp5ga[sb] - 2 * nibble(rate)   (phy_n.c srom_convert)
 *   target         = max over rates, minus 6          (phy_cmn.c)
 *
 * The maximum lands on the rate with the smallest offset nibble, so the whole
 * per-rate array collapses to its minimum nibble here. The nibbles are in
 * half-dB and the register in quarter-dB, which is where the factor 2 comes
 * from; brcmsmac's QDB() factor of 4 converts the whole-dB SROM and
 * regulatory values.
 *
 * The regulatory stage is not applied. All four 5 GHz rules in brcmsmac's
 * world regdomain cap at 21 dBm, so QDB(21) = 84, and the captures show 86
 * reaching the register on ch100 -- the ceiling does not bind on these
 * boards, whose ccode is empty and regrev 0.
 *
 * Verified against the d6220 sweep and the agcombo captures: exact on all 17
 * observations at 20 MHz and all 4 at 80 MHz. 40 MHz is 5 of 8, and is why
 * the validated list carries no 40 MHz entry.
 *
 * The direction of that error matters. Coming out below the vendor costs
 * range and nothing else; coming out above it drives the PA harder than the
 * board was characterised for. Both 40 MHz residuals are on the wrong side --
 * this driver computes 66 where the vendor writes 64, and 64 where it writes
 * 62 -- so 40 MHz is not merely unverified here, it is unverified in the
 * hazardous direction. Whoever enables it should bias the result low until
 * the last stage is understood.
 *
 * The residual is regular: the first 40 MHz block of each sub-band comes out
 * two units high and the second is exact, which is a function of the block's
 * position rather than of any channel. That rules out the per-channel stages,
 * including the regulatory ceiling above.
 *
 * The two 40 MHz configurations the derivation misses are corrected by hand.
 * The predicate that selects them -- lowest 40 MHz block of a sub-band whose
 * maxp5ga entry differs from the next one's -- is fitted, and fitted on two
 * points. It was written after seeing that an unconditional correction broke
 * agcombo, so agcombo does not confirm it: any predicate false there and true
 * on the d6220's ch36 and ch52 would score the same. There is one agcombo
 * observation at 40 MHz.
 *
 * It is also not physically motivated. The grp0/grp1 boundary is at 5250 MHz,
 * and it is ch44's block that touches it, 5210 to 5250, while ch36's sits well
 * inside at 5170 to 5210. A "block spills into the neighbouring sub-band"
 * mechanism would fire on ch44, which is the configuration the derivation
 * already gets right.
 *
 * What the correction does have going for it is direction: it only ever
 * lowers, so a board where the predicate misfires loses range rather than
 * overdriving the PA.
 *
 * table 0x21, the ppr array, cannot settle which rate wins the maximum: its
 * offsets are identical across all 52 sweep segments and all three widths,
 * while the mcsbw*po nibbles differ by width. So the offsets are not
 * 2 * nibble(rate) and carry no information about the per-rate SROM limits.
 */
static u16 b43_phy_ac_txpwr_target(struct b43_wldev *dev, unsigned int core)
{
	const struct ssb_sprom *sprom = dev->dev->bus_sprom;
	struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int band = b43_phy_ac_po_band(ac->cal_channel);
	unsigned int grp = b43_phy_ac_txpwr_subband(ac->cal_channel,
						   ac->cal_width);
	u32 po;
	u8 maxp, nib;
	int lim, ceil;
	unsigned int j;

	/*
	 * 80 MHz takes the 20 MHz offsets. The maximum is over every rate,
	 * and the 20 MHz rates stay populated whatever the operating width,
	 * so they carry the smallest nibble and win; the sweep's 80 MHz
	 * configurations agree with the 20 MHz ones channel for channel.
	 */
	po = (ac->cal_width == NL80211_CHAN_WIDTH_40)
		? sprom->mcsbw5g_po[band].bw40
		: sprom->mcsbw5g_po[band].bw20;

	nib = 0xf;
	for (j = 0; j < 8; j++)
		nib = min_t(u8, nib, (po >> (4 * j)) & 0xf);

	maxp = sprom->core_pwr_info[core].maxp5ga[grp];
	lim = maxp - 2 * nib;

	/* TODO: find the last stage; this fitted correction stands in for it. */
	if (ac->cal_width == NL80211_CHAN_WIDTH_40 &&
	    b43_phy_ac_txpwr_first_block(ac->cal_channel) && grp + 1 < 4 &&
	    sprom->core_pwr_info[core].maxp5ga[grp + 1] &&
	    sprom->core_pwr_info[core].maxp5ga[grp + 1] != maxp)
		lim -= 2;

	/* The regulatory ceiling bounds the SROM limit, before the margin. */
	ceil = b43_phy_ac_reg_ceiling(dev);
	if (ceil && lim > ceil)
		lim = ceil;

	if (lim <= 6)
		return 0;

	return (u16)(lim - 6);
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
	 * ppr[24], the per-rate power reduction in table 0x21 offset 0, 24
	 * u32s: ppr[1] = ppr[5] = ppr[6] = 0x00000202 and the rest zero.
	 *
	 * Channel-invariant, and the d6220 sweep is unambiguous about it: one
	 * payload across all 16 of its 20 MHz channels. So these constants are
	 * right on every channel of this board, not just ch36.
	 *
	 * They are also not the mcsbw*po mapping an earlier note here claimed.
	 * If they were, they would change at 5250: this board's
	 * mcsbw205glpo and mcsbw205gmpo differ, and ppr does not. Nor does
	 * regulatory limiting enter -- all three boards have an empty ccode
	 * and regrev 0, so there is no country table in play.
	 *
	 * L'asse board invece e' verificato, e non e' invariante: l'agcombo ha
	 * uno sweep, e sui suoi segmenti della sottobanda alta -- ch100-140, a
	 * ogni larghezza -- il payload porta anche ppr[10] = 0x00000101,
	 * mentre il d6220 ha zero la' su tutti e 26 i segmenti. Si vede dove i
	 * nibble della SROM sono grandi: l'agcombo ha mcsbw205ghpo = 0xcca88440
	 * contro valori piccoli su questa board. Quindi queste tre costanti
	 * sono giuste per il d6220 e incomplete altrove, e la tabella e'
	 * derivata dalla SROM anche se su questa board non si muove.
	 */
	ppr[1] = 0x00000202;
	ppr[5] = 0x00000202;
	ppr[6] = 0x00000202;

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
	 * b43_phy_ac_idle_tssi_meas(), which set_channel() calls before this
	 * function, matching the position seen in the capture some 240 ops
	 * before this block.
	 */

	/* Target power and control bits, transcribed as 0xc8, not 0x96. */
	b43_phy_maskset(dev, 0x0071, (u16)~0x00ff, 0x00c8);
	b43_phy_maskset(dev, 0x0071, (u16)~0x0700, 0x0400);
	b43_phy_maskset(dev, 0x0070, (u16)~0x0800, 0);
	b43_phy_maskset(dev, 0x0070, (u16)~(0x0400), (0x0400));

	/*
	 * Per-core max index, emitted high core -> low core to match the
	 * vendor order (0x0846 before 0x0646; the current index above goes
	 * 0->1).
	 *
	 * maxp5ga - 6 is not the rule, and this is not a matter of fixing the
	 * margin or the sub-band boundaries. The d6220 sweep refutes it on its
	 * own, without appealing to another board:
	 *
	 *  - it writes four distinct values below 5.5 GHz -- 66 and 64 at
	 *    20 MHz, 62 at 20 MHz from ch52 up, and 66 again at 40 MHz on the
	 *    ch44 pair -- where maxp5ga holds three usable entries. No constant
	 *    margin over a three-entry lookup can produce four values.
	 *  - it is not a function of the centre frequency either: 5180 and
	 *    5200 give 66, 5190 gives 64, 5220 gives 64 and 5230 gives 66.
	 *
	 * Both points are same-board, same-driver. The DSL-3580L looks like a
	 * third witness -- same 4352, maxp5ga uniformly 76, three different
	 * values written -- but it runs wl 6.30 against this port's 7.14 and
	 * writes the cores in the opposite order, so it testifies about a
	 * different algorithm. It is the right capture for telling a version
	 * fork from a hardware fact, and the wrong one for refuting a 7.14
	 * model.
	 *
	 * This register is the TX power ceiling index, written during power
	 * control setup, but it is not an outcome of that loop. The sweep runs
	 * every configuration twice, eleven seconds apart, and across those 26
	 * pairs the RX-IQ coefficients differ 26 times out of 26 and the
	 * idle-TSSI base index 25 times, while this value differs zero times.
	 * It is a deterministic function of channel and bandwidth, so there is
	 * a computation to find; it just is not this one.
	 *
	 * The most useful clue is that 20 and 40 MHz swap values on the first
	 * two channels -- ch36 gives 66 then 64, ch44 gives 64 then 66 -- so
	 * bandwidth does not enter as a scale factor. The NVRAM has a separate
	 * mcsbw*po field per bandwidth and per band, nine in all, which is
	 * where a term of that shape would come from.
	 *
	 * maxp5ga is the ceiling the board declares; the clamp below is what
	 * makes it binding. ch36 and ch100 both land on maxp5ga - 6, so
	 * neither distinguishes the margin from the derivation.
	 */
	{
		unsigned int cr;

		for (cr = num_cores; cr-- > 0; ) {
			if (!((dev->phy.ac->coremask >> cr) & 1))
				continue;
			/*
			 * On a first bring-up the max index is the constant
			 * 0x38, independent of the SROM: the d6220, agcombo and
			 * DSL all write 0x38 on attach. On a later channel setup
			 * it is maxp5ga[grp] - 6, which gives 0x42 on the d6220
			 * (maxp5ga0 = 72) and 0x44 on agcombo (74).
			 *
			 * The DSL emits 0x38 on the down-to-up path too, a
			 * version difference tracked in retrace-todo.md.
			 */
			b43_phy_maskset(dev, 0x0646 + cr * 0x0200, (u16)~0x00ff,
					(dev->phy.ac->status_mask &
					 B43_PHY_AC_STATE_FIRST_BRINGUP)
					? 0x0038
					: (b43_phy_ac_txpwr_target(dev, cr) &
					   0x00ff));
		}
	}

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
 * TBL 0x20 bulk write in b43_phy_ac_channel_setup). Match with D6220
 * wl-diag #34564+: byte-low of column [0] reproduces the TBL 0x20 bulk
 * 128/128 exactly.
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
 * @rf_seq is one of B43_PHY_AC_RF_SEQ_TRIG_{RX2TX,RST2RX,...}.
 *
 * Poll budget: up to 200 x udelay(1), about 200us, the same wait the N-PHY
 * and HT-PHY force_rf_sequence helpers use. Validated on hardware, where the
 * timeout has never been logged; the roughly 1ms DELAY in the blob is just
 * its retry granularity.
 *
 * Returns true if the sequence completed within the spin window, false on
 * timeout. Non-static: shared by other PHY units.
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
 * forcing the PHY clock.
 *
 * For the hard variant, which does force the clock and is used by
 * channel_switch_prep(), see b43_phy_ac_reset_cca().
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
 * Reset the CCA (Clear Channel Assessment) state machine.
 *
 * Stage 1 pulses BBCFG bit 0x4000 with the PHY clock forced.
 */
void
b43_phy_ac_reset_cca(struct b43_wldev *dev)
{
	B43_AC_FN();
	struct b43_phy_ac *phy_ac = dev->phy.ac;

	b43_phy_force_clock(dev, true);
	/* Pulse RSTCCA. The vendor always renders the pulse as a maskset
	 * pair, 98 out of 98 ops across the d6220 and agcombo captures, so
	 * phy_maskset() is used here to make the harness emit the same
	 * format. */
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
 * TODO: the 0x05f4 written to 0x0140 is pinned to ch36. If it has to vary per
 * channel it must come from the channel table or from a computation.
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
	 * while bit 11 carries PHY state: clear on a fresh attach, then set
	 * by coeff_bank_init() and persistent on the chip. Preserving it from
	 * the peek is what makes both the attach captures (bit 11 clear) and
	 * a later set_channel (bit 11 set) match. */
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
/*
 * Quiesce the silicon RX cores the board does not wire.
 *
 * PHY reg 0x0b reports the silicon PHY core count (3 on the 4352/4360 die); the
 * board may wire fewer, given by the SROM rxchain mask. Save reg 0x401/0x400,
 * drive the sequencer mode bits for the desired mask, fire force_rfseq cmd 0
 * (trigger 0x01) then cmd 1 (0x02), restore. Reg 0x401 is read once and not
 * written in between, so a single save is equivalent.
 */
/* Forward declaration; run_rfseq_cmd() is defined later in this file. */
static void b43_phy_ac_run_rfseq_cmd(struct b43_wldev *dev, u16 cmd_bit);

static void b43_phy_ac_rxcore_setstate(struct b43_wldev *dev, u8 coremask)
{
	B43_AC_FN();
	u16 saved_401, saved_400;

	saved_401 = b43_phy_read_log(dev, B43_PHY_AC_RF_SEQ_MODE);
	saved_400 = b43_phy_read_log(dev, B43_PHY_AC_RFCTL1);
	/* The stock driver reads this register here; the value's use is not
	 * known, so the read is logged and discarded. */
	b43_phy_read_log(dev, 0x06d8);

	/* RX-gain override companion of 0x06d8, bracketing the core-state
	 * change: forced wide-open (0xffff) before, set to the operational
	 * mask (0x1431) after. Values transcribed literally -- identical in
	 * both d6220 captures (attach /, down-to-bss-up
	 * #53398/#53426); exact field semantics unconfirmed. */
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

	b43_phy_write(dev, 0x16d8, 0x1431);
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
 * bit 0. The chip signals completion with bit 0 of 0x0403, which is polled
 * until it appears or the wait expires. The blob polls twice on the first
 * execution and once on the second, consistent with a wait-for-done loop.
 *
 * cmd_bit is OR-ed into 0x0402; 0x0001 then 0x0002 are the observed values.
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

	/* Poll until the done bit is set, at most ten reads. The
	 * down-to-bss-up capture delays 1027us between the first two reads of
	 * 0x0403, so the poll does wait between iterations; udelay(200) ten
	 * times gives a 2ms ceiling, which is enough. */
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
 * set_channel() fills: the vendor writes the chanspec at the head of the RF
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
 */
static void b43_phy_ac_post_rfseq_misc_setup(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Peek diagnostica pre-setup (vendor #34526). */
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
 * Coefficient bank init, 48 ops. What the cross-capture comparison shows:
 *   - d6220 ch44 BW20 equals d6220 ch36 BW20, so it is channel-independent
 *     within 5 GHz at 20 MHz
 *   - agcombo ch36 BW20 equals d6220 ch36 BW20, so it is chip-independent for
 *     a given bandwidth and band
 *   - d6220 ch36 BW40 differs, so the LUT does depend on bandwidth
 *
 * TODO: parametrise for 40 and 80 MHz and for 2 GHz with separate LUTs. Only
 * 5 GHz at 20 MHz is wired here, the one configuration a capture supports.
 */
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

static void b43_phy_ac_coeff_bank_init_bw20_5g(struct b43_wldev *dev)
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
		/* 0x018f */ B43_PHY_AC_REG_TBL_WRITE_GATE,
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
 * Analog reset-time sub-setups, run under the caller's table-write lock. FEM
 * control and TX-LPF stay as helpers; RX-LPF and dacbuf-cap have a single
 * call site each and are inline here.
 */
/*
 * Power-detector setup: 14 one-shot writes. @full selects the long variant,
 * which includes 0x0358-0x035a, over the short one that stops at 0x0559.
 *
 * It is not per-core: the +0x200 stride, which would give 0x0750/0x0950/
 * 0x0b50, appears in no capture. The values are transcribed rather than
 * derived, and the two identical pairs (1000/1000 and 500/500) look more like
 * settle windows than gain codes. 0x0554 and 0x0555 are adjusted later by the
 * periodic watchdog; see b43_phy_ac_op_recalc_txpower().
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

static void b43_phy_ac_channel_setup(struct b43_wldev *dev,
				     const struct b43_phy_ac_channeltab_e_radio2069 *e,
				     struct ieee80211_channel *new_channel)
{
	B43_AC_FN();
	unsigned int i;

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_AFE_ON);

	if (!e) {
		b43err(dev->wl, "AC-PHY: no channel table entry, skipping setup\n");
		return;
	}

	/* Reset-time register block (includes the CCA reset). */
	b43_phy_ac_set_reg_on_reset(dev);

	/*
	 * RXIQ coefficient seed (per-core, 2 bytes each). ch36 #33025-33028.
	 * Runs before the 2nd AFE/LPF stage call; the comment below mentioned
	 * these as "not yet ported".
	 */
	b43_phy_maskset(dev, 0x02d1, (u16)~0x00f0, 0x0040);
	b43_phy_maskset(dev, 0x02d1, (u16)~0x0f00, 0x0400);
	b43_phy_maskset(dev, 0x02d2, (u16)~0x00f0, 0x0040);
	b43_phy_maskset(dev, 0x02d2, (u16)~0x0f00, 0x0400);

	/* Stadio AFE/LPF per catena, 2a chiamata. #51897-51965. */
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

	/* #34526-#34559. Ri-locka il gate outer in uscita. */
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

	/* #34698-#34749. */
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
	 * Azzeramento per catena. #34771-#34804, agcombo #60610+. Filtrato per
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

	/* #34805-#34852. */
	b43_phy_ac_coeff_bank_init_bw20_5g(dev);

	/*
	 * Second invocation of set_analog_tx_lpf with stages=0x100 (only
	 * stage 8). The 0x019e gate is already locked by
	 * channel_analog_setup(), which ends on a relock maskset, so the
	 * _locked variant is used and skips the pre-lock. The closing
	 * idempotent unlock is emitted explicitly. 41 ops in total: 20 per
	 * core over two cores, plus the unlock.
	 */
	/*
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

	/* #34894-#34907. */
	b43_phy_ac_rx_evm_shaping_override(dev);

	/*
	 * Per-core maskset "post rfseq_2 prep" (d6220 ch36 #34908-#34911, 4
	 * op = 2 op × 2 core attivi):
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

	/* #34912-#34983. */
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
	 */
	/*
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
	 * Clear bits massivo per 0x0410 + per-core 0x0X3a/0x0X25 (d6220 ch36
	 * #34987-#35006, 20 op = 2 op globali + 6 op × 3 core). NON filtrato
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

	/* #35007-#35020. */
	b43_phy_ac_farrow_setup(dev, new_channel);

	/*
	 * Peek + relock outer gate (d6220 ch36 #35021-#35022, 2 op).
	 * Prepara la fase finale di TBL.RD/TBL.WR su celle 0x03cd/0x03dd.
	 */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

	/*
	 * RMW-shaped access to two cells of table 7: read, then write a fixed
	 * value. Not a real RMW, since the value read is discarded; the read is
	 * most likely there for hardware synchronisation. 20 ops, 10 per cell:
	 *   - 0x03cd: read, then write 0x04c2
	 *   - 0x03dd: read, then write 0x04e2
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

	/* #35050-#35086. */
	b43_phy_ac_chanspec_tail(dev);
}

/*
 * Table id 0x11, 464 words, loaded through the alternate data register 0x0011
 * by b43_actab_write_r11().
 *
 * Only the twelve words at the head and the four at the tail are real data,
 * and both are channel-invariant. The 448 words between them are a single
 * repeated value that depends on the sub-band, which is why the array used to
 * be wrong above channel 48.
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
 * Per-channel table loads (radio_rev 4, 5 GHz): twin coeff 0x00ec-0x00f5 + tbl 0x11 (464w) + tbl 0x0b/0x15 + coppia per-core 0x44/0x45 (broadcast a num_cores). */
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
 */
static void
b43_phy_ac_post_noise_shaping_rx_regprog_core(struct b43_wldev *dev,
					      unsigned int core)
{
	B43_AC_FN();
	u16 stride = (u16)(core * 0x200);
	u16 tbl_off = (u16)(0x00f9 + core);
	static const u16 tbl_val = 0xc0b5;

	/* peek + program 0x06dc/0x06dd */
	b43_phy_read_log(dev, 0x06dc + stride);
	b43_phy_write(dev, 0x06dc + stride, 0x016a);
	b43_phy_write(dev, 0x06dd + stride, 0x0604);

	/* TBL.WR id=0x07 off=0xf9+core val=0xc0b5 (gate era unlockato: usa _reopen) */
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
 * Program one chain's RX gain-control block, 0x0720-0x073e: open the override
 * bracket on 0x?73e, re-read the 13 saved registers in the vendor's order,
 * then emit the 26 programming ops. All of them are masksets, to match the
 * capture's RMW shape.
 *
 * 41 ops, emitted identically at two points of the flow and in the same
 * order: the tail of switch_channel, in b43_phy_ac_rxgainctrl_regs(), and the
 * RX AFE reconfiguration at the head of the measure block. @gw_hi, the
 * channel's high gain word, is the only difference between the two sites.
 */
static void b43_phy_ac_rx_gain_regs_program(struct b43_wldev *dev,
					    unsigned int core, u16 gw_hi)
{
	u16 s = (u16)(core * 0x200);

	b43_phy_read_log(dev, 0x073e + s);
	b43_phy_write(dev,    0x073e + s, 0x0440);

	b43_phy_read_log(dev, 0x0727 + s);
	b43_phy_read_log(dev, 0x073c + s);
	b43_phy_read_log(dev, 0x0721 + s);
	b43_phy_read_log(dev, 0x0729 + s);
	b43_phy_read_log(dev, 0x0720 + s);
	b43_phy_read_log(dev, 0x0728 + s);
	b43_phy_read_log(dev, 0x0724 + s);
	b43_phy_read_log(dev, 0x0736 + s);
	b43_phy_read_log(dev, 0x0725 + s);
	b43_phy_read_log(dev, 0x0739 + s);
	b43_phy_read_log(dev, 0x073a + s);
	b43_phy_read_log(dev, 0x0722 + s);
	b43_phy_read_log(dev, 0x0734 + s);

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
		static const u16 z = 0x0000, w0 = 0x2f13, w1 = 0x00f3, g = 0x0035;
		u8 dummy;

		if (!((mask >> c) & 1))
			continue;

		/* Apertura core: peek + relock */
		saved = b43_phy_ac_tbl_write_lock(dev);

		/*
		 * Readback gain-curve, offset chip-dependent: 0x40 su 4352
		 * (d6220 #38127-#38131), 0x0000 su 4360 (agcombo #64021).
		 */
		b43_actab_read_bulk(dev, 0x20,
				    dev->dev->chip_id == 0x4360 ? 0x0000 : 0x40,
				    8, 1, &dummy);

		b43_actab_write_bulk(dev, 7, 0x100 + c, 16, 1, &z);
		b43_actab_write_bulk(dev, 7, 0x103 + c, 16, 1, &w0);
		b43_actab_write_bulk(dev, 7, 0x106 + c, 16, 1, &w1);

		/* Mid-sync tra gruppo id=0x07 e gruppo id=0x0c (vendor #38147-#38148). */
		saved = b43_phy_ac_tbl_write_lock(dev);

		b43_actab_write_bulk(dev, 0xc, 0x63 + c * 4, 16, 1, &g);
		b43_actab_write_bulk(dev, 0xc, 0x73 + c * 4, 16, 1, &g);

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
 * Abilitazione del controllo di potenza TX, seconda fase PHY del setup.
 *
 * Era la coda di b43_phy_ac_adc_reset(), che con 192 righe portava due blocchi
 * distinti sotto il nome del primo. Lo split non cambia una sola op: le due
 * meta' restano chiamate in sequenza da set_channel, ed e' il posto giusto --
 * il marcatore di questo blocco, PHY.WR 0x1641, compare una volta sola nella
 * cattura a freddo e cade dentro la prima fase PHY.
 *
 * Non confonderlo con il ricalcolo del target di potenza, 0x0644/0x0646 per
 * core: quello e' la fase PHY che nella cattura cade *dopo* la config BSS del
 * core, ed e' quello che in b43 spetta a b43_phy_txpower_check() chiamata da
 * b43_op_config().
 */
static void b43_phy_ac_txpwrctrl_enable(struct b43_wldev *dev)
{
	B43_AC_FN();
	u8 c, num_cores = dev->phy.ac->num_cores;
	u8 mask = dev->phy.ac->coremask;
	unsigned int i;

	/*
	 * TX power-control enable (0x70[15:13]), framed by 0x1641 (broadcast
	 * gain reg). Vendor #38197-#38204:
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
	 * PLL lock verify, the counterpart of channel_switch_prep(). Now that
	 * the RX freeze has covered the whole channel_setup() flow and the PLL
	 * has had tens of milliseconds to settle -- through the udelays in
	 * radio_2069_channel_setup() and the rest of the setup -- the vendor
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

	/* PHY update strobe (vendor #38206-#38207). */
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

	/* Arm the RX gate and drop the ADC bracket (#38257). */
	b43_phy_ac_rx_gate_with_adc_hold(dev, true);
	b43_phy_write(dev, 0x0339, 0x0000);

	/*
	 * PHY update strobe. Vendor #38266-#38267: MOD (mask esplicita), NON OR.
	 * Il TSSI-path enable per-core (#38268-#38273: MOD 0x0072/0x?727/0x?73c
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

static void b43_phy_ac_crs_regs_write(struct b43_wldev *dev, u16 val)
{
	B43_AC_FN();
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_crs_regs); i++)
		b43_phy_maskset(dev, b43_phy_ac_crs_regs[i],
				(u16)~0x00ff, val);
}

/*
 * CRS min-power recalculation, the low byte of 0x0324 through 0x0333:
 * crs = ladder[bw][clamp(threshold + anchor[bw], 0, 14)], plus 4 when cold.
 *
 * The entry is selected by b43_phy_ac_crs_index() from the noise statistic
 * the watchdog latches; the observed values are the entry plus the cold bump
 * of 4, so 45, 48, 53 and 60 appear as 49, 52, 57 and 64.
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
 * b43_phy_ac_txpwr_subband() uses for the power ceiling, whose first boundary
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

static u8 b43_phy_ac_crs_min_pwr(unsigned int bw_idx, unsigned int idx, bool cold)
{
	u8 crs = b43_phy_ac_crs_ladder[bw_idx][idx];

	if (cold)
		crs = (u8)(crs + 4);	/* bump a freddo */

	return crs;
}

static enum b43_txpwr_result
b43_phy_ac_op_recalc_txpower(struct b43_wldev *dev, bool ignore_tssi)
{
	unsigned int bw_idx = 0;	/* solo BW20: switch_channel rifiuta 40/80 */
	bool cold = (dev->phy.ac->cal_cycles < 2);
	unsigned int idx;
	u8 crs;

	B43_AC_FN();

	idx = b43_phy_ac_crs_index(dev);
	crs = b43_phy_ac_crs_min_pwr(bw_idx, idx, cold);
	b43_phy_ac_crs_regs_write(dev, crs);

	if (dev->phy.ac->cal_cycles < 2)
		dev->phy.ac->cal_cycles++;

	return B43_TXPWR_RES_DONE;
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
 * ch36 they are wrong CCA thresholds, and the filter in set_channel() is
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
	 * The vendor writes the negative case as 0xfb, a signed byte. The old
	 * form clamped at zero and so wrote nothing where an offset of -5 was
	 * wanted; it looked right only because ch36, the one validated
	 * channel, is on the side where the difference is positive.
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
 *   1. clear bit 4 of 0x0164, undoing the set from coeff_bank_init_bw20_5g()
 *   2. clear bits 14 and 15 of 0x030f
 *   3. write the gain to 0x031c-0x031f: 0x00bf at 20 MHz in 5 GHz, 0x0100 at
 *      80 MHz in 5 GHz, 0x00ff in 2.4 GHz
 *   4. the per-core CRS registers, eight ops -- four registers over two
 *      cores, interleaved, +0xc stride -- with 0x31 under mask 0x00ff at
 *      20 MHz in 5 GHz and 0x36 at 40 and 80
 *   5. clear the high and low bytes of 0x0910-0x0913, eight alternating ops
 *   6. peek 0x03a9, then two clears on it, bits 0-6 and bit 11
 *   7. ten raw writes to 0x00ec-0x00f5, values pinned to 5 GHz at 20 MHz
 *   8. peek and relock the outer gate, closing the tail for chan_tables()
 *
 * TODO: parametrise the 0x00ec-0x00f5 values by bandwidth and band.
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
		crs = b43_phy_ac_crs_min_pwr(0, b43_phy_ac_crs_index(dev),
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
 * Orchestrator for the post-channel-setup calibrations: everything the vendor
 * emits after the rxcal_afe finalize, gathered in one place. In order:
 * post_cal_finalize iterations 2 and 3, rxiqcal iterations 1 to 24, the
 * rxcal AFE calibrate and finalize, a first txpwr round with its rxiqcal
 * iteration, a second txpwr round with the gainctrl_final loop, then the RXIQ
 * teardown and finalize.
 */
/*
 * Whether the calibrations that transmit may run on this channel.
 *
 * Above 5250 MHz the captures do not run them at all. Two phases are absent
 * outright, and the evidence is the same on every segment: not one access to
 * the command register 0x0380 and not one to 0x0b22 on any of the nineteen
 * segments from channel 52 up, against 313 to 978 and nine respectively on
 * every segment from 48 down. It is not a shorter run, it is nothing, and it
 * is most of the difference between a 36k-operation attach and a 20k one.
 *
 * 5250 MHz is where the regulatory domains put the DFS boundary, and these
 * calibrations transmit -- they drive the tone generator and poll for the
 * result. A radio that may not transmit until the channel availability check
 * has finished cannot run them, which is a reason for the split rather than
 * just a line that happens to fit. Not proof, though: the same boundary is
 * also "the second 5 GHz sub-band", and the captures do not separate the two.
 *
 * Only the phases proven absent are behind this. The rest of the block below
 * runs on both sides of the boundary as far as has been checked, and where the
 * boundary really falls in it is in docs/retrace-todo.md.
 */
static bool b43_phy_ac_may_calibrate_tx(struct b43_wldev *dev)
{
	return dev->phy.chandef->chan->center_freq <= 5250;
}

void b43_phy_ac_set_channel_calibrations(struct b43_wldev *dev)
{
	/*
	 * Called from set_channel() after mac_enable and after the body of
	 * set_channel() has put the classifier in WAITED mode, that is RX_OFDM
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
	 * Post-cal finalize iter 2 (vendor #39947-#40538) e iter 3
	 * (#40539+).
	 */
	b43_phy_ac_post_cal_finalize(dev);
	b43_phy_ac_post_cal_finalize_iter3(dev);
	b43_phy_ac_rxiqcal_apply(dev);
	/*
	 * Salta dal canale 52 in su: la tabella 0x000e, che questa fase e' la
	 * sola a toccare, ha 8 accessi sui canali fino al 48 e zero dal 52 in
	 * su su tutti e ventidue i segmenti.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_post_rxiqcal_stage2(dev);

	/*
	 * RX AFE calibration (vendor #41909+, ~1500 op), on the lower 5 GHz
	 * channels only.
	 *
	 * Skipped above 5250 MHz; see b43_phy_ac_may_calibrate_tx().
	 */
	if (b43_phy_ac_may_calibrate_tx(dev)) {
		b43_phy_ac_rxcal_afe_calibrate(dev);
		b43_phy_ac_rxcal_afe_finalize_gain_luts(dev);
	}

	/*
	 * Primo round post-cal RXIQ (vendor #45962-#46774).
	 */
	b43_phy_ac_txpwr_by_index(dev, B43_PHY_AC_TXPWR_INDEX_DEFAULT);
	b43_phy_ac_rxgain_defaults_pulse(dev);
	b43_phy_ac_radio_chain_range_setup(dev, true);
	b43_phy_ac_rxgain_perchan_config(dev);
	b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(dev);
	b43_phy_ac_rxiqcal_dds_seed(dev);
	b43_phy_ac_rxiqcal_prep_second_iter(dev);
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_rxiqcal_run_meas_iters(dev);
	b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(dev);
	/*
	 * Azzeramento delle tabelle dei coefficienti IQ, 0x42/0x62/0x82: 256
	 * voci ciascuna sui canali fino al 48, zero dal 52 in su su tutti e
	 * ventidue i segmenti. Sono le tabelle che le fasi di calibrazione
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
	b43_phy_ac_rxgain_config_apply(dev);
	b43_phy_ac_radio_iqcal_config(dev);

	/*
	 * Ricerca del guadagno di loopback (round 6°-9° di txpwr apply):
	 * vedi b43_phy_ac_loopback_gain_search e il commento alla ricerca.
	 */
	if (b43_phy_ac_may_calibrate_tx(dev))
		b43_phy_ac_loopback_gain_search(dev);

	/* 5° dds_seed + meas_apply variante v2 */
	b43_phy_ac_rxiqcal_dds_seed_second_tone(dev);
	b43_phy_ac_iqcal_meas_post_dds_apply_v2(dev);

	/* 6° dds_seed (third_tone: 2° reversed) + meas_apply v2 (37, 51) */
	b43_phy_ac_rxiqcal_dds_seed_third_tone(dev);
	b43_phy_ac_iqcal_meas_post_dds_apply_v2(dev);

	/* Teardown finale RXIQ. */
	b43_phy_ac_rxiq_apply_coefficients(dev);
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

static int b43_phy_ac_set_channel(struct b43_wldev *dev,
				  struct ieee80211_channel *channel,
				  enum nl80211_channel_type channel_type)
{
	B43_AC_FN();
	struct b43_phy *phy = &dev->phy;
	const struct cfg80211_chan_def *chandef = &dev->wl->hw->conf.chandef;
	enum nl80211_chan_width width = chandef->width;
	const struct b43_phy_ac_channeltab_e_radio2069 *e2069;
	u16 off;

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
	 * moving. Gains, TX power indices, gain-LUT defaults, tone generator
	 * amplitudes and thresholds are among them, and on an RF chain other
	 * than the one they were read from they are not slightly wrong, they
	 * can overdrive the PA.
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

	dev->phy.ac->cal_channel = channel->hw_value;
	dev->phy.ac->cal_width = width;
	dev->phy.ac->cal_freq = width == NL80211_CHAN_WIDTH_20
				? channel->center_freq : chandef->center_freq1;

	/*
	 * The MAC is already suspended from the end of op_init(), whose tail
	 * b43_mac_suspend leaves bit 0 clear, so there is no mac_suspend op
	 * here: the vendor emits the MAC preparation sequence in op_init, and
	 * set_channel() is entered with the MAC suspended. status_mask already
	 * has MAC_EN clear at this point.
	 *
	 * No precondition on the classifier or clip detect: on the first entry
	 * after attach, RX_ANY and CLIP_ALL_DIS are both clear, while on a
	 * channel change the bits reflect the previous channel's state.
	 * set_channel() sets both itself further down.
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
	 * clip detectors frozen, CCA reset. Trace d6220 ch36 #32891-32901,
	 * BEFORE radio_channel_setup (#34530). Everything from here through
	 * rx_enable runs under RX=w CLIP=111; first release is the rx_gate
	 * pulse in idle_tssi_meas (#38209).
	 */
	b43_phy_ac_channel_switch_prep(dev);

	b43_radio_2069_channel_setup(dev, e2069);
	b43_phy_ac_channel_setup(dev, e2069, channel);

	b43_phy_ac_chan_tables(dev);

	/*
	 * Noise shaping table init (ch36 #37407-37900). Populates the per-core
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
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0000);   /* #37407 unlock */
		saved = b43_phy_ac_tbl_write_lock(dev);               /* #37408-#37409 peek+relock */

		b43_phy_read_log(dev, 0x016c);                        /* #37410 peek */
		b43_phy_maskset(dev, 0x016c, (u16)~0x0040, 0x0040);   /* #37411 set bit 6 */

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

			/* Diagnostic readbacks (#37662-37728, etc.) */
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

	/* BW select per-core (ch36 #38075-38078), before reset_cca/afecal. */
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
	 * Short settle before txpwrctrl reads the idle-TSSI result. The trace
	 * shows a ~35 ms gap here, a brief busy-wait suffices.
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
	 * Scansione delle due direct-map table, sedici voci ciascuna, in sola
	 * lettura: cold01 #12245-#12307, e la lettura di 0x0056 che la chiude.
	 * Nessun valore da riprodurre, e cosa consumi il risultato non e' noto
	 * -- una scansione per sapere quali blocchi per-rate esistono avrebbe
	 * questa forma.
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
	 * 0x10f4-0x14b2 azzerate, 480 word. Nel blob e' il caricamento di una
	 * tabella da rodata, e la tabella e' tutta zeri: il ciclo la sostituisce
	 * senza perdere niente. Una volta per attach e subito dopo RFATT --
	 * cold01 #12311-#12790 -- e le stesse 480 word a zero su ogni segmento
	 * controllato, a ogni canale e larghezza.
	 */
	for (off = 0x10f4; off <= 0x14b2; off += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, off, 0x0000);
	/*
	 * Secondo azzeramento, attaccato al primo: 68 word da 0x05e0 a 0x0666,
	 * cold01 #12791-#12858, identiche su ogni segmento controllato. Le
	 * prime dieci cadono nel blocco KEYIDXBLOCK che b43.h dichiara del
	 * core, ma il vendor le scrive in questa corsa e non altrove -- ogni
	 * cella compare una volta sola nella cattura -- quindi la corsa e'
	 * riprodotta per intero e il perimetro di compare.py e' stato
	 * ristretto di conseguenza.
	 */
	for (off = 0x05e0; off <= 0x0666; off += 2)
		b43_shm_write16(dev, B43_SHM_SHARED, off, 0x0000);
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);
	b43_phy_ac_shm_mac_config_block(dev);
	/*
	 * cold01 #12894-#12956: seconda meta' del poll con una sola passata,
	 * senza testa, senza spazzata e senza coda.
	 */
	b43_phy_ac_wd_stats_poll_opt(dev, false, 1);
	/*
	 * cold01 #12958-#12959, invarianti su tutti e 26 i segmenti. Fra queste
	 * e il poll sotto la cattura ha una TPL.RAMW, che e' template RAM del
	 * core.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x018a, 0xffce);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x018c, 0xffba);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0);
	/*
	 * cold01 #13024-#13055, dopo la spazzata e dopo i quattro blocchi CCK
	 * che restano da capire.
	 */
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
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);           /* #39182 unlock */
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);      /* #39183 TX power ctrl enable */
	/*
	 * The gain-word pair only appears from the second bring-up on: on the
	 * first, the stock driver emits 0x0070 and then goes straight to
	 * 0x0678/0x0878, while on a later channel setup the pair is there. This
	 * holds for this site and for adc_reset(), but not for the tail of
	 * rxiqcal_finalize(), which always emits it.
	 */
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		b43_phy_maskset(dev, 0x0644, (u16)~0x007f, 0x0014); /* #39184 c0 */
		b43_phy_maskset(dev, 0x0844, (u16)~0x007f, 0x0014); /* #39185 c1 */
	}
	b43_phy_maskset(dev, 0x0678, (u16)~0x0004, 0);           /* #39186 clr bit 2 c0 */
	b43_phy_maskset(dev, 0x0878, (u16)~0x0004, 0);           /* #39187 clr bit 2 c1 */
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0040, 0x0040);    /* #39188 MHF3 set bit 6 */
	b43_phy_ac_ofdm_pctl1_readback(dev);
	/*
	 * cold01 #13451-#13453, invarianti su tutti e 26 i segmenti; a cosa
	 * servano non e' noto. Fra queste e la mappa il vendor scrive
	 * KEYIDXBLOCK, che e' del core. Solo la prima delle due passate della
	 * mappa le porta.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x0082, 0x2710);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ba, 0xffff);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x003c, 0x000a);
	b43_phy_ac_basic_rate_map(dev);
	b43_maccontrol_set(dev, ~0x00100000u, 0);                /* #39189 clr bit 20 */
	b43_maccontrol_set(dev, ~0x01c00000u, 0);                /* #39190 clr bits 22-24 */
	b43_mac_enable(dev);                                     /* #39191 */
	/*
	 * Le quattro celle di PWRIND_BLKS che precedono la 0x0308 da cui
	 * crs_note_noise() prende il campione: cold01 #13473-#13479, subito
	 * dopo la riattivazione del MAC. Sole letture, e niente qui consuma il
	 * valore -- a cosa servano al vendor non e' noto.
	 */
	for (off = 0x0300; off <= 0x0306; off += 2)
		b43_shm_read16(dev, B43_SHM_SHARED, off);
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0);
	/*
	 * MHF4 bit 15: alzato al primo bring-up (attach-to-bss-up #12023),
	 * abbassato su un channel setup successivo (ch36 #39192).
	 */
	b43_phy_ac_mhf_maskset(dev, 4, (u16)~0x8000,
			       (dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP) ? 0x8000 : 0);
	/*
	 * MHF1 bit 0: abbassato al primo bring-up (#12024), alzato su un channel
	 * setup successivo (ch36 #39193) -- polarita' opposta a MHF4 bit 15 sopra.
	 */
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0001,
			       (dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP) ? 0 : 0x0001);
	b43_mac_suspend(dev);                                    /* #39194 */
	/*
	 * cold01 #13529. Invariante su tutti e 26 i segmenti dello sweep a
	 * freddo; a cosa serva non e' noto.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x007c, 0x0320);
	b43_phy_ac_mhf_maskset(dev, 3, (u16)~0x0020, 0x0020);    /* #39195 MHF3 set bit 5 */
	b43_mac_enable(dev);                                     /* #39196 */
	/*
	 * cold01 #13533. Dipende dalla sola larghezza -- 0x0f0f a 20 e 80 MHz,
	 * 0x0303 a 40 -- e i due byte sono sempre uguali fra loro, quindi e' un
	 * valore replicato su due meta'. Che 0x0f diventi 0x03 a 40 MHz e' un
	 * quarto e non un mezzo, e non e' spiegato.
	 */
	b43_shm_write16(dev, B43_SHM_SHARED, 0x005a,
			(dev->phy.ac->cal_width == NL80211_CHAN_WIDTH_40)
				? 0x0303 : 0x0f0f);
	b43_phy_maskset(dev, 0x0042, (u16)~0x8000, 0x8000);      /* #39197 set bit 15 */
	b43_phy_ac_mhf_maskset(dev, 1, (u16)~0x0020, 0x0020);    /* #39198 MHF1 set bit 5 */
	b43_mac_suspend(dev);                                    /* #39199 */
	b43_phy_ac_wd_stats_poll_opt(dev, true, 0);
	b43_phy_ac_basic_rate_map(dev);				/* cold01 #13585 */

	/*
	 * Fine della prima meta'. Il resto lo invoca il chiamante, dopo che il
	 * core ha scritto la configurazione BSS -- SSID e lunghezze dei
	 * template, cold01 #13593-#13624 -- con il MAC ancora sospeso.
	 */
	return 0;
}

/*
 * Seconda meta' del setup di canale.
 *
 * E' cio' che in b43 invoca il core dopo il ritorno di b43_switch_channel():
 * b43_op_config() chiama b43_phy_txpower_check(), e fra le due meta' il core
 * scrive la configurazione BSS. Il confine e' letto dai dati, non scelto: sul
 * segmento di riferimento l'ultima op della prima meta' e' #13585 e la prima
 * di questa e' il PLCP a #13627, col blocco BSS #13593-#13624 in mezzo.
 *
 * Prende `channel` per parametro e ricava il resto da `dev`: sono i soli due
 * valori del prologo di set_channel che questa meta' usava, per cui il taglio
 * non porta stato implicito. Un taglio che ne lasciasse compilerebbe senza un
 * warning e sbaglierebbe i valori a runtime.
 */
void b43_phy_ac_channel_setup_tail(struct b43_wldev *dev,
				   struct ieee80211_channel *channel)
{
	B43_AC_FN();
	struct b43_phy *phy = &dev->phy;

	(void)phy;
	b43_phy_ac_prb_rsp_plcp(dev,
			b43_phy_ac_prb_rsp_len(dev->phy.ac->cal_width));
	b43_maccontrol_set(dev, ~0x10000000u, 0x10000000);       /* #39200 set bit 28 */
	b43_maccontrol_set(dev, ~0x10000000u, 0);                /* #39201 clr bit 28 */
	b43_maccontrol_set(dev, ~0x00040000u, 0x00040000);       /* #39202 set bit 18 */
	b43_maccontrol_set(dev, ~0x48020000u, 0x00020000);       /* #39203 clr30 set17 clr22-24 */
	b43_mac_enable(dev);                                     /* #39204 */
	b43_maccontrol_set(dev, ~0x00100000u, 0x00100000);       /* #39205 set bit 20 */
	b43_mac_suspend(dev);                                    /* #39206 */
	/*
	 * Lettura-modifica-scrittura di 0x00cc, cold01 #13673-#13676: legge il
	 * valore e lo riscrive due volte identico. La cella e' toccata cosi'
	 * anche nella config BSS e a ogni tick del watchdog, sempre con lo
	 * stesso schema; a cosa serva la doppia riscrittura non e' noto.
	 */
	{
		u16 cc = b43_shm_read16(dev, B43_SHM_SHARED, 0x00cc);

		b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
		b43_shm_write16(dev, B43_SHM_SHARED, 0x00cc, cc);
	}
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00ce, 0x0000);
	b43_shm_write16(dev, B43_SHM_SHARED, 0x00d0, 0x0000);

	/*
	 * Seconda passata del ciclo dei dodici rate, cold01 #13683-#13740. Fra
	 * le celle sopra e questa il vendor scrive KEYIDXBLOCK, che e' del
	 * core.
	 */
	b43_phy_ac_prb_rsp_rate_po(dev);
	b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);                               /* #39207 peek */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);      /* #39208 relock */

	/* Second txpwrctrl_setup() call. The vendor emits the same sequence
	 * op-for-op: the LUT is computed from the same SPROM coefficients and
	 * the ppr values are unchanged. */
	b43_phy_ac_txpwrctrl_setup(dev, channel->center_freq);

	/*
	 * Transition after the second txpwrctrl_setup(): TX power control setup,
	 * a restore, five MAC wake/suspend pulses -- probably a firmware flush
	 * to make it reload the rate table -- then three MODs on 0x019e clearing
	 * bits 6, 7 and 8.
	 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);          /* #39539 unlock */
	b43_phy_maskset(dev, 0x0070, (u16)~0xe000, 0xe000);     /* #39540 */
	/* Come sopra: coppia gain word assente al primo bring-up (#12375). */
	if (!(dev->phy.ac->status_mask & B43_PHY_AC_STATE_FIRST_BRINGUP)) {
		b43_phy_maskset(dev, 0x0644, (u16)~0x007f, 0x0014); /* #39541 */
		b43_phy_maskset(dev, 0x0844, (u16)~0x007f, 0x0014); /* #39542 */
	}
	b43_phy_maskset(dev, 0x0678, (u16)~0x0004, 0);          /* #39543 */
	b43_phy_maskset(dev, 0x0878, (u16)~0x0004, 0);          /* #39544 */
	/* 5 pulses mac wake/suspend (#39545-#39554) */
	for (unsigned int pulse = 0; pulse < 5; pulse++) {
		b43_mac_enable(dev);
		b43_mac_suspend(dev);
	}
	b43_phy_read(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);                              /* #39555 peek */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0040, 0);          /* #39556 clr bit 6 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0080, 0);          /* #39557 clr bit 7 */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0100, 0);          /* #39558 clr bit 8 */

	/* Per-core RX gain-control programming, immediately after the third
	 * transition that follows the two txpwrctrl_setup() calls. */
	b43_phy_ac_rxgainctrl_regs(dev);

	/* Vendor #39641-#39708: radio loopback setup per-core (34 op/core). */
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

	/* Vendor #39709-#39734: PHY tone-setup (26 op, non per-core). */
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
		b43_phy_write(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, 0x03d0);    /* #39898 unlock gate plain */

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

		/* #39941-#39945: finalize (unarm tone gen + gate cleanup). */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);  /* relock */
		b43_phy_write(dev,   0x0394, 0x000b);
		b43_phy_write(dev,   0x0393, 0x0000);                /* unarm */
		b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);       /* clr bit 9 */
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);       /* unlock */
	}

	/*
	 * The post-channel calibration sequence -- post_cal_finalize, rxiqcal,
	 * rxcal_afe, the gainctrl_final loop and the teardown -- is invoked by the caller of
	 * op_switch_channel(), after its mac_enable; see
	 * b43_phy_ac_op_switch_channel() and
	 * b43_phy_ac_set_channel_calibrations(). The vendor emits the MAC.MCTRL
	 * enable between the end of the rxcal cleanup and post_cal_finalize.
	 */
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
 * PHY attach/init entry (.init op). Whole-attach dispatcher: each step below
 * carries its own capture range on its own function. First op on the wire is
 * the num_cores read (4360 #5); the trailing PMU regctl/GPIO
 * pair is 4360 #335-336.
 */
/*
 * Pre-op_init analog frontend (vendor down-to-bss-up #51679-#51705): runs
 * after the radio bring-up (op_software_rfkill) and before op_init proper.
 * The vendor programs pdet + per-core RF-frontend gates here so that
 * init_regs and the first channel_setup see a settled analog state; the
 * driver previously only ran the equivalent inside channel_setup (i.e. after
 * op_init), which is the second occurrence the vendor also does (#33099).
 *
 * pdet is the 11-write short form (no 0x0358 trio, which is set_channel-only).
 * Per core: PHY 0x0X29/0x0X21 bit 12, RAD 0x0X33 nibble -> 0x4000. Closes
 * with PHY 0x01b0 bit 15.
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

	/* Vendor reads PHY 0x0000 here (#51706) before the PMU regctl. */
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

static int b43_phy_ac_op_init(struct b43_wldev *dev)
{
	B43_AC_FN();
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
	 * op_init proper (#51679-#51705), before the PMU regctl strobe.
	 */
	b43_phy_ac_pre_init_frontend(dev);

	/*
	 * PMU regctl chip-dependent field [24:20] (0x1 on 4352, 0x2 on 4360) +
	 * GPIO.CTL clear. d6220 attach #32831-32832, at the head of the op_init
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
	 * only raise the refcount, and set_channel(), which b43_phy_init() calls
	 * straight afterwards, would find it at 1 instead of 0. Its 90
	 * suspend/enable pairs would then all become no-ops and the MAC would
	 * never be toggled during the calibration.
	 */
	return 0;
}

enum b43_phy_ac_afe_mode {
	B43_PHY_AC_AFE_DOWN,	/* front-end parked (RF blocked / pre-init) */
	B43_PHY_AC_AFE_ON,	/* front-end armed for RX/TX (bss-up) */
};

/*
 * Program the PHY analog front-end bank (the AFE_C1 registers at +0x1000
 * stride, 0x1720-0x173e) to the requested mode. B43_PHY_AC_AFE_ON is the
 * final RX/TX arm the OEM emits at bss-up (down-to-bss-up #86603-86610);
 * B43_PHY_AC_AFE_DOWN parks the front-end. Kept as one named operation so
 * the enable point is explicit and callers pick a mode rather than
 * open-coding register writes.
 */
static void b43_phy_ac_enable_afe(struct b43_wldev *dev,
				  enum b43_phy_ac_afe_mode mode)
{
	B43_AC_FN();
	switch (mode) {
	case B43_PHY_AC_AFE_ON:
		b43_phy_write(dev, 0x173e, 0x0000);
		b43_phy_write(dev, 0x1739, 0x0000);
		b43_phy_write(dev, 0x173a, 0x0000);
		b43_phy_write(dev, 0x1725, 0x1fff);
		b43_phy_write(dev, 0x1729, 0x0000);
		b43_phy_write(dev, 0x1721, 0xffff);
		b43_phy_write(dev, 0x1728, 0x0000);
		b43_phy_write(dev, 0x1720, 0x03ff);
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_AFE_ON;
		break;
	case B43_PHY_AC_AFE_DOWN:
		b43_phy_write(dev, 0x1728, 0x0080);
		b43_phy_write(dev, 0x1720, 0x0180);
		b43_phy_write(dev, 0x1729, 0x0000);
		b43_phy_write(dev, 0x1721, 0x5000);
		dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_AFE_ON;
		break;
	}
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
 */
/*
 * regctl 0 bit 1 is a PMU resource request. The stock driver uses it as a
 * bracket around the channel work: raised in the attach preamble and lowered
 * at its end, then on a later bring-up lowered on entry to set_channel() and
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
	 * The preamble used to end with a fourth MACCONTROL write here, setting
	 * INFRA and DISCPMQ and clearing AP. Those are the core's operating
	 * mode, not the front end's, and b43_adjust_opmode() sets them: the PHY
	 * had no business writing them. The captures agree -- the stock driver
	 * emits it after the core has written its chip-init cells to shared
	 * memory, not with the GPIO setup (cold01 #645 for the GPIO control
	 * write above, #651 for this one, with two shared-memory writes in
	 * between).
	 */
}

static void b43_phy_ac_switch_analog_once(struct b43_wldev *dev, bool on)
{
	B43_AC_FN();
	u16 saved_417, saved_416;

	/*
	 * The generic b43_phyop_switch_analog_generic writes B43_MMIO_PHY0
	 * (0x3e6); the OEM never touches that offset (no SI.COREREG off=0x03e6
	 * in any capture). Like every modern PHY in-tree (N, HT, LCN) the AC
	 * drives its own analog front-end instead.
	 *
	 * The unit is save -> AFE bank -> restore, as emitted at the top of a
	 * cold attach (d6220 attach-to-bss-up #50-#99, twice back to back): ten
	 * reads, the AFE_ON block on the override page, then chan-select bit 1
	 * cleared and 0x0417/0x0416 put back to what they held. The first five
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

	b43_phy_ac_enable_afe(dev, on ? B43_PHY_AC_AFE_ON
				     : B43_PHY_AC_AFE_DOWN);

	b43_phy_maskset(dev, 0x0408, (u16)~0x0002, 0);
	b43_phy_write(dev, 0x0417, saved_417);
	b43_phy_write(dev, 0x0416, saved_416);

	/*
	 * Cold attach only: a 6-bit field in 0x02e4 is set right after the unit
	 * above, before the MHF block and a second copy of the unit (d6220
	 * attach-to-bss-up #81, agcombo attach #27 -- same value on both chips,
	 * so it is the phase that selects it, not the chip).
	 *
	 * On a later bring-up the d6220 does not touch 0x02e4 at all, hence the
	 * do_full_init gate. The DSL (wl 6.30) writes 0x0800 there on its
	 * down->up instead: a version divergence, tracked in retrace-todo.md,
	 * not reproduced here.
	 *
	 * Unrelated to the 0x0800 that set_channel writes on the 4360 -- that
	 * one lands after init_regs and no 4352 witness emits it.
	 */
	if (!on || !dev->phy.do_full_init)
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

	b43_phy_ac_enable_afe(dev, B43_PHY_AC_AFE_ON);
	b43_phy_maskset(dev, 0x0408, (u16)~0x0002, 0);
	b43_phy_write(dev, 0x0417, saved_417);
	b43_phy_write(dev, 0x0416, saved_416);

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
 */
static void b43_phy_ac_op_switch_analog(struct b43_wldev *dev, bool on)
{
	b43_phy_ac_switch_analog_once(dev, on);

	if (on && dev->phy.do_full_init && dev->dev->chip_id == 0x4360) {
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
		b43_phy_ac_switch_analog_once(dev, on);
	}

	/*
	 * The front-end GPIO setup closes the sequence once, not on every entry:
	 * in the agcombo capture GPIO.OUT = 0x400 appears exactly once, after
	 * the second entry. On the 4352, which enters once, the distinction is
	 * invisible.
	 */
	if (on)
		b43_phy_ac_frontend_gpio_setup(dev);
}

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

	if (blocked) {
		/* RF blocked: park the front-end. */
		b43_phy_ac_enable_afe(dev, B43_PHY_AC_AFE_DOWN);
		return;
	}

	/*
	 * The chanspec heads the radio bring-up, and one position covers both
	 * phases: on the cold attach it is the op immediately before the
	 * radio prologue (cold03 #691, prologue at #692), and on a warm cycle
	 * it lands immediately before the first op of b43_radio_2069_init().
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
	 * bring-up (#51609-51677, after the 0x08ea RCCAL_EN cleanup, before the
	 * op_init analog reset). afe_728 = 0x0800 in the ch36/5GHz capture.
	 */
	b43_radio_2069_afe_lpf_stage(dev, 0x0800);

	/*
	 * software_rfkill's scope ends at the radio front-end being up. The
	 * two-phase GPIO frontend, per-core PA bias and the final PMU regctl
	 * enable that the vendor emits only at steady state (#52545,
	 * #86566-#86616) are driven from the rxiqcal / TX-enable path, not
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
 * TODO: establish where iteration 3 ends and what follows -- further
 * iterations, or the RX-IQ compensation write-back.
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
	 * set_channel(), but emitted on its own here -- without the MHF and gate
	 * relock that follow it there, because that was a transition into
	 * txpwrctrl rather than into another idle_tssi iteration.
	 */
	b43_phy_write(dev, 0x0339, 0x0fff);

	/*
	 * The RX suspend closes with the same clear of the statistics window
	 * that a watchdog tick does: cold03 #15729-#15734, between the 0x0339
	 * write above and the mac_suspend below. The counters have been
	 * accumulating since the channel setup, and the probe phase that
	 * follows reads the window on every tick.
	 */
	b43_phy_ac_wd_stats_clear(dev);

	/*
	 * Ensure the MAC is suspended without nesting. The write the stock
	 * driver emits here is already produced by the preceding site in
	 * set_channel(); an unconditional suspend would write nothing but would
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
	/* #41247-#41256 */
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
	/* #41257-#41264 */
	{ 0x0721, 0x0800, 0x0800 },
	{ 0x0729, 0x0800, 0x0000 },
	{ 0x0721, 0x0400, 0x0400 },
	{ 0x0729, 0x0400, 0x0000 },
	{ 0x0721, 0x4000, 0x4000 },
	{ 0x0728, 0x3800, 0x0000 },
	{ 0x0721, 0x1000, 0x1000 },
	{ 0x0729, 0x1000, 0x0000 },
	/* #41265-#41274 */
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
	/* #41275-#41276: WR raw coefficienti I/Q (TODO formula) */
	{ 0x0724, 0x0000, 0x03ff },
	{ 0x0736, 0x0000, 0x0152 },
	/* #41277-#41286 */
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
	/* #41287-#41302 */
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
		u8 dummy_rd;
		b43_actab_read_bulk(dev, 0x0020, 0x0014, 8, 1, &dummy_rd);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
		b43_actab_read_bulk(dev, 0x0020, 0x001e, 8, 1, &dummy_rd);
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
			u16 dummy_63;
			static const u16 tblw_063 = 0x0040;
			static const u16 tblw_073 = 0x0040;

			b43_actab_read_bulk(dev, 0x000c, 0x0063, 16, 1, &dummy_63);
			b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &tblw_063);
			b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &tblw_073);
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
			u16 dummy_67;
			static const u16 tblw_067 = 0x003c;
			static const u16 tblw_077 = 0x003c;

			b43_actab_read_bulk(dev, 0x000c, 0x0067, 16, 1, &dummy_67);
			b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
			b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &tblw_067);
			b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &tblw_077);
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
 * TODO: the three groups are fixed because the 0x000c overrides are global
 * even when the coremask excludes a chain. To be checked against a capture
 * from a different chip.
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
	 * B4b, 86 ops: table 0x000e, offset 0, 40 entries of 32 bits. The table
	 * is symmetric -- elements 0 to 19 are identical to 20 to 39 -- which
	 * suggests a DDS or NCO lookup, or a filter with a 20-sample period.
	 *
	 * The values are transcribed from the d6220 ch36 capture. They may be
	 * channel-dependent, or derived from a formula; neither is established.
	 */
	{
		static const u32 b4b_tbl_data[40] = {
			/* metà 1 (elementi 0-19) */
			0x0003e800, 0x0003b84d, 0x00032893, 0x00024cca,
			0x000134ee, 0x000000fa, 0x000eccee, 0x000db4ca,
			0x000cd893, 0x000c484d, 0x000c1800, 0x000c4bb3,
			0x000cdb6d, 0x000db736, 0x000ecf12, 0x00000306,
			0x00013712, 0x00024f36, 0x00032b6d, 0x0003bbb3,
			/* Second half, elements 20-39: an exact copy of the first. */
			0x0003e800, 0x0003b84d, 0x00032893, 0x00024cca,
			0x000134ee, 0x000000fa, 0x000eccee, 0x000db4ca,
			0x000cd893, 0x000c484d, 0x000c1800, 0x000c4bb3,
			0x000cdb6d, 0x000db736, 0x000ecf12, 0x00000306,
			0x00013712, 0x00024f36, 0x00032b6d, 0x0003bbb3,
		};

		b43_actab_write_bulk_scoped(dev, 0x000e, 0x0000, 32, 40,
					    b4b_tbl_data);
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
	 * between cores 0 and 1, while slots 0x07 to 0x10 differ. Two cores are
	 * hardcoded here, from the d6220 ch36 capture where two are active. A
	 * board with three will probably need a 0x40 + i slot for core 2.
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
 */
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
	/* Iter 18: cmd=0xa234, result 0x0764. Tail extra pending (8 WR di
	 * inizializzazione fase successiva). */
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
		static const u16 val_40 = 0x0040;
		static const u16 val_3c = 0x003c;

		/* Trailer 1: 0x63 + 0x73 val=0x0040 */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, 0x0063, 16, 1, &val_40);
		b43_actab_write_bulk(dev, 0x000c, 0x0073, 16, 1, &val_40);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);

		/* Trailer 2: 0x67 + 0x77 val=0x003c */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, 0x0067, 16, 1, &val_3c);
		b43_actab_write_bulk(dev, 0x000c, 0x0077, 16, 1, &val_3c);
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
 * RX AFE cal finalize gain LUTs, phase C.
 *
 * Preamble, 3 ops:
 *   set then clear bit 14 of 0x0001, pulsing the hardware trigger
 *   write 0x0382 = 0, resetting the B4b config registers
 *
 * Body, 2688 ops: 128 offsets over three cores, each offset emitting three
 * self-contained table writes to 0x42 for core 0, 0x62 for core 1 and 0x82
 * for core 2, with values uniform per core.
 *
 * Core 0's and core 1's values, 0x0002 and 0x0200, do not depend on the
 * phase: they are the same in both d6220 captures that record read values.
 * Only core 2's default changes between a first bring-up and a later switch.
 *
 * SALAME: the transition to offset 0x21 for core 2 is an observation with no
 * explanation yet. Worth revisiting once the structure of the 0x42/0x62/0x82
 * LUTs is clearer.
 *
 * The sweep bears on this: table 0x82's payload is distinct on every one of
 * the 16 BW20 channels, while 0x40 and 0x60 fall into three sub-band groups.
 * So these are not one kind of thing.
 */
void b43_phy_ac_rxcal_afe_finalize_gain_luts(struct b43_wldev *dev)
{
	B43_AC_FN();

	b43_phy_ac_todo(dev,
		"TX IQ/LO compensation coefficients are written from a table, "
		"not computed. They are accumulated calibration state and no "
		"formula for them has been found; what is written here is what "
		"one board produced on one run. Transmit spectrum may be worse "
		"than the hardware can do.\n");

	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Scaffolding: the constants below are measured, not derived.
	 *
	 * TODO: they are not gain defaults. Tables 0x0042/0x0062/0x0082 hold the
	 * TX IQ/LO compensation coefficients, and the values change from run to
	 * run because they are accumulated state -- there is no formula to find,
	 * the calibration has to run. Hardcoding them is wrong on a three-chain
	 * board. Blob symbols, the checks that ruled other readings out, and the
	 * current state: docs/rxiq-cal-analysis.md section 12.
	 */
	bool first = dev->phy.ac->status_mask &
		     B43_PHY_AC_STATE_FIRST_BRINGUP;
	u16 val_c0 = 0x0002;
	u16 val_c1 = 0x0200;
	u16 val_c2_lo = first ? 0x0962 : 0xabd0;
	u16 val_c2_hi = first ? 0x0758 : 0xa9c6;
	unsigned int i;

	/* Preamble */
	b43_phy_ac_cca_pulse(dev);
	b43_phy_write(dev, 0x0382, 0x0000);

	/* Body: 128 × 3 TBL.WR interleaved core 0/1/2 */
	for (i = 0; i < 0x80; i++) {
		const u16 *vc2 = (i < 0x21) ? &val_c2_lo : &val_c2_hi;

		b43_actab_write_bulk_scoped(dev, 0x0042, i, 16, 1, &val_c0);
		b43_actab_write_bulk_scoped(dev, 0x0062, i, 16, 1, &val_c1);
		b43_actab_write_bulk_scoped(dev, 0x0082, i, 16, 1, vc2);
	}
}

/*
 * Return the gain registers 0x0720-0x073e to their defaults and close with
 * the commit pulse. Called immediately after txpwr_by_index(). The values are
 * identical across chains.
 *
 * TODO: whether the vendor also emits core 2 on a three-chain board is
 * unknown; no capture shows it.
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

	/* Block 2: 16 WR per core × 2 core */
	for (core = 0; core < 2; core++) {
		u16 stride = (u16)(core * 0x200);

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
	u8 dummy_rd[2];

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
		b43_actab_read_bulk(dev, 0x0020, 0x0014, 16, 1, dummy_rd);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);   /* unlock manuale */
		b43_actab_read_bulk(dev, 0x0020, 0x001e, 16, 1, dummy_rd);
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
 */
static void b43_phy_ac_rxgain_perchan_tail(struct b43_wldev *dev)
{
	B43_AC_FN();
	unsigned int num_cores = dev->phy.ac->num_cores;
	u8 coremask = dev->phy.ac->coremask;
	unsigned int c;

	/* Pass 1 (forward): 3 peek+WR pair per core attivo. */
	for (c = 0; c < num_cores; c++) {
		u16 s = (u16)(c * 0x200);

		if (!((coremask >> c) & 1))
			continue;

		b43_phy_read_log(dev, 0x0739 + s);
		b43_phy_write(dev,    0x0739 + s, 0x00fa);
		b43_phy_read_log(dev, 0x073a + s);
		b43_phy_write(dev,    0x073a + s, 0x01d3);
		b43_phy_read_log(dev, 0x0725 + s);
		b43_phy_write(dev,    0x0725 + s, 0x07e6);
	}

	/* Pass 2 (reverse): 3 WR standalone per core attivo, high-to-low. */
	for (c = num_cores; c-- > 0; ) {
		u16 s = (u16)(c * 0x200);

		if (!((coremask >> c) & 1))
			continue;

		b43_phy_write(dev, 0x0725 + s, 0x07e2);
		b43_phy_write(dev, 0x073a + s, 0x01d3);
		b43_phy_write(dev, 0x0739 + s, 0x007a);
	}
}

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
	static const struct { u16 g1; u16 bbmult; } comp[2] = {
		{ 0x4f7f, 0x0040 },  /* core 0 */
		{ 0x2f7f, 0x003c },  /* core 1 */
	};
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
		u16 bbmult;

		if (!((ac->coremask >> core) & 1))
			continue;

		if (!first_core) {
			/* Bridge between cores: an idempotent lock MOD only. */
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		}
		first_core = false;

		bbmult = comp[core].bbmult;

		/* Batch A: 3 fast TBL.RD TX gain code (readback pre-compensation).
		 * Area TBL 0x0007 off 0x100+ = TX gain LUT (stessa area di
		 * txpwr_by_index). */
		b43_actab_read_bulk(dev, 7, (u16)(core + 0x0100), 16, 1, &discard);
		b43_actab_read_bulk(dev, 7, (u16)(core + 0x0103), 16, 1, &discard);
		b43_actab_read_bulk(dev, 7, (u16)(core + 0x0106), 16, 1, &discard);

		/* Batch B: 3 fast TBL.WR TX gain code (writeback compensato) */
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0100), 16, 1, &g0);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0103), 16, 1, &comp[core].g1);
		b43_actab_write_bulk(dev, 7, (u16)(core + 0x0106), 16, 1, &g2);

		/* Batch C: TBL.RD bbmult (TX baseband multiplier) readback +
		 * sync explicit (peek+MOD lock idempotente) */
		b43_actab_read_bulk(dev, 0xc, bbmult_lo[core], 16, 1, &discard);
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Batch D: 2 fast TBL.WR TX bbmult compensato */
		b43_actab_write_bulk(dev, 0xc, bbmult_lo[core], 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0xc, bbmult_hi[core], 16, 1, &bbmult);
	}

	/* Postamble */
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
}

/*
 * RX-IQ DDS/NCO seed (vendor #46451-#46561, fase E block 2, 111 op).
 * Vedi commento in phy_ac.h per struttura dettagliata.
 */
void b43_phy_ac_rxiqcal_dds_seed(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * DDS/NCO table: 40 u32s with a 20+20 periodic symmetry, the two halves
	 * being identical. Transcribed from the d6220 ch36 capture.
	 */
	static const u32 dds_seed[40] = {
		0x0003e800, 0x0003b84d, 0x00032893, 0x00024cca, 0x000134ee,
		0x000000fa, 0x000eccee, 0x000db4ca, 0x000cd893, 0x000c484d,
		0x000c1800, 0x000c4bb3, 0x000cdb6d, 0x000db736, 0x000ecf12,
		0x00000306, 0x00013712, 0x00024f36, 0x00032b6d, 0x0003bbb3,
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

	/* 86 op: carica 40 u32 DDS/NCO in TBL 0x000e */
	b43_actab_write_bulk_scoped(dev, 0x000e, 0x0000, 32, 40, dds_seed);
}

/*
 * RX-IQ prep second iteration (vendor #46562-#46777, 216 op).
 * Vedi commento in phy_ac.h per struttura completa.
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
	b43_phy_mask(dev,      0x0471, (u16)~0x0001);              /* AND clr bit 0 */
	b43_phy_write(dev,     0x0463, 0x0027);
	b43_phy_write(dev,     0x0461, 0xffff);
	b43_phy_write(dev,     0x0462, 0x003c);
	b43_phy_read_log(dev,  0x0400);
	b43_phy_set(dev,       0x0400, 0x0001);                    /* OR set bit 0 */
	b43_phy_mask(dev,      0x0460, (u16)~0x0004);              /* AND clr bit 2 */
	b43_phy_mask(dev,      0x0460, (u16)~0x0001);              /* AND clr bit 0 */
	b43_phy_mask(dev,      0x0382, (u16)~0xc000);              /* AND clr bit 14/15 */
	b43_phy_set(dev,       0x0382, 0x8000);                    /* OR set bit 15 */
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
 * RX-IQ measurement iters (vendor #46778-#47296, gruppo 4, 519 op).
 * Vedi commento in phy_ac.h per struttura e op count per iter.
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
 * RX-IQ post-measurement apply (vendor #47297-#47328, fase F seg A, 32 op).
 * Vedi commento in phy_ac.h per struttura.
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
	static const u16 bbmult_per_core[2] = { 0x0040, 0x003c };
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
		u16 bbmult = bbmult_per_core[core];

		if (!((ac->coremask >> core) & 1))
			continue;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, &bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* 3 op standalone finali: pulse + reset */
	b43_phy_ac_cca_pulse(dev);
	b43_phy_write(dev, 0x0382, 0x0000);
}

/*
 * Reset tabelle di coefficienti (vendor #47329-#50016, 2688 op).
 * Vedi commento in phy_ac.h.
 */
void b43_phy_ac_iqcal_coeff_tables_reset(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	static const u16 zero = 0x0000;
	unsigned int off;

	for (off = 0; off < 128; off++) {
		u16 tbl82_val = (off <= 0x20) ? 0xf8fc : 0xf6f2;

		b43_actab_write_bulk_scoped(dev, 0x0042, (u16)off, 16, 1, &zero);
		b43_actab_write_bulk_scoped(dev, 0x0062, (u16)off, 16, 1, &zero);
		b43_actab_write_bulk_scoped(dev, 0x0082, (u16)off, 16, 1, &tbl82_val);
	}
}

/*
 * IQ-cal secondary stage apply (vendor #50152-#50198, 47 op).
 * Vedi commento in phy_ac.h per struttura.
 */
void b43_phy_ac_iqcal_apply_second_stage(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Values measured by iteration 20 on core 0 and iteration 22 on core 1;
	 * see b43_phy_ac_rxiqcal_run_meas_iters(). They are reapplied here at
	 * different offsets in table 0x000c, 0x60 and 0x64 instead of 0x40 and
	 * 0x48.
	 *
	 * TODO: derive these at runtime; they are transcribed from the d6220
	 * ch36 capture.
	 */
	/* Reapply the AFE results that were kept, not constants from a capture. */
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
 * RX-gain config readback (vendor #50199-#50292, 94 op).
 * Vedi commento in phy_ac.h per struttura.
 */
void b43_phy_ac_rxgain_config_readback(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/* The order the capture shows, which is not sorted by address. */
	static const u16 gain_regs[25] = {
		0x0720, 0x0721, 0x0722, 0x0723, 0x0724, 0x0725, 0x0726, 0x0727,
		0x0728, 0x0729, 0x0732, 0x0733, 0x0730, 0x0731, 0x0734, 0x0735,
		0x0737, 0x0738, 0x0736, 0x0739, 0x073a, 0x073b, 0x073c, 0x073d,
		0x0747,
	};
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
		for (i = 0; i < ARRAY_SIZE(gain_regs); i++)
			b43_phy_read_log(dev, gain_regs[i] + stride);

		/* 1× fast TBL.RD id=0x000c bbmult */
		b43_actab_read_bulk(dev, 0x000c, bbmult_off[core], 16, 1, &discard);

		/* 3× fast TBL.RD id=0x0007 gain code */
		for (i = 0; i < 3; i++)
			b43_actab_read_bulk(dev, 0x0007, gain_lut_off[core][i],
					    16, 1, &discard);

		/* 1 peek 0x?73e */
		b43_phy_read_log(dev, 0x073e + stride);
	}
}

/*
 * RX-gain config apply (vendor #50293-#50438, 146 op).
 * Vedi commento in phy_ac.h per struttura.
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
 * Radio 2069 IQ-cal config per-core (vendor #50439-#50522, 84 op).
 * Vedi commento in phy_ac.h per struttura.
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
 * Gain control final apply (vendor #50523-#50651, 129 op).
 * Vedi commento in phy_ac.h per struttura.
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
		b43_actab_read_bulk(dev, 0x0020, 0x0000, 16, 1, &discard);

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
 * DDS/NCO second tone seed (vendor #50652-#50737, 86 op).
 * Vedi commento in phy_ac.h.
 */
void b43_phy_ac_rxiqcal_dds_seed_second_tone(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Second DDS/NCO LUT: 40 u32s with the same 20+20 symmetry. The values
	 * differ from the first dds_seed(), probably a different tone or
	 * modulation.
	 */
	static const u32 dds_seed[40] = {
		0x0002d400, 0x0002b038, 0x0002486a, 0x0001a892, 0x0000e0ac,
		0x000000b5, 0x000f20ac, 0x000e5892, 0x000db86a, 0x000d5038,
		0x000d2c00, 0x000d53c8, 0x000dbb96, 0x000e5b6e, 0x000f2354,
		0x0000034b, 0x0000e354, 0x0001ab6e, 0x00024b96, 0x0002b3c8,
		0x0002d400, 0x0002b038, 0x0002486a, 0x0001a892, 0x0000e0ac,
		0x000000b5, 0x000f20ac, 0x000e5892, 0x000db86a, 0x000d5038,
		0x000d2c00, 0x000d53c8, 0x000dbb96, 0x000e5b6e, 0x000f2354,
		0x0000034b, 0x0000e354, 0x0001ab6e, 0x00024b96, 0x0002b3c8,
	};

	b43_actab_write_bulk_scoped(dev, 0x000e, 0x0000, 32, 40, dds_seed);
}

/*
 * DDS/NCO third tone seed (vendor #51957-#52042, 86 op).
 * Vedi commento in phy_ac.h.
 */
void b43_phy_ac_rxiqcal_dds_seed_third_tone(struct b43_wldev *dev)
{
	B43_AC_FN();
	B43_PHY_AC_REQUIRE(dev,
			   B43_PHY_AC_STATE_RX_WAITED | B43_PHY_AC_STATE_CLIP_ALL_DIS,
			   B43_PHY_AC_STATE_RX_CCK | B43_PHY_AC_STATE_RX_OFDM |
			   B43_PHY_AC_STATE_CCA_RESET | B43_PHY_AC_STATE_MAC_EN);

	/*
	 * Third DDS/NCO LUT: 40 u32s with the same 20+20 symmetry. The values
	 * are the second LUT with positions 1 to 19 in reverse order.
	 */
	static const u32 dds_seed[40] = {
		0x0002d400, 0x0002b3c8, 0x00024b96, 0x0001ab6e, 0x0000e354,
		0x0000034b, 0x000f2354, 0x000e5b6e, 0x000dbb96, 0x000d53c8,
		0x000d2c00, 0x000d5038, 0x000db86a, 0x000e5892, 0x000f20ac,
		0x000000b5, 0x0000e0ac, 0x0001a892, 0x0002486a, 0x0002b038,
		0x0002d400, 0x0002b3c8, 0x00024b96, 0x0001ab6e, 0x0000e354,
		0x0000034b, 0x000f2354, 0x000e5b6e, 0x000dbb96, 0x000d53c8,
		0x000d2c00, 0x000d5038, 0x000db86a, 0x000e5892, 0x000f20ac,
		0x000000b5, 0x0000e0ac, 0x0001a892, 0x0002486a, 0x0002b038,
	};

	b43_actab_write_bulk_scoped(dev, 0x000e, 0x0000, 32, 40, dds_seed);
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
 * IQ-cal measurement + apply post second DDS (vendor #50738-#50836, 99 op).
 * Vedi commento in phy_ac.h per struttura completa.
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
		b43_phy_ac_iq_acc_peek(dev, c);

	/* Blocco G (29 op): apply bbmult per-core (variante) */
	b43_phy_read_log(dev, 0x0464);
	b43_phy_set(dev,      0x0460, 0x0002);
	b43_phy_mask(dev,     0x0460, (u16)~0x0004);
	for (c = 0; c < 2; c++) {
		u16 lo = (u16)(0x0063 + 4 * c);
		u16 hi = (u16)(0x0073 + 4 * c);
		static const u16 bbmult = 0x0044;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, &bbmult);
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
 * docs/rxiq-cal-analysis.md section 11.
 *
 * The later cal stages -- estimate, leakage, tone fit -- are not here; see the
 * TODO in b43_phy_ac_rxcal_afe_finalize_gain_luts().
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
		b43_phy_ac_rxiqcal_dds_seed_second_tone(dev);
		b43_phy_ac_iqcal_meas_post_dds_apply(dev);

		for (c = 0; c < 2; c++)
			if (searching[c])
				searching[c] =
					!b43_phy_ac_loopback_step(dev, c,
								  &idx[c]);
	}
}

/*
 * Six gain-register peeks, 0x?c0 to 0x?c5, for one core, in the observed
 * order 0x?c3, 0x?c2, 0x?c5, 0x?c4, 0x?c1, 0x?c0.
 */
/*
 * Capture a core's six RX-IQ accumulators and add them into the state. These
 * are the reads the stock driver emits anyway, in the same order; the only
 * difference is that the value is kept instead of discarded.
 */
static void b43_phy_ac_iq_acc_peek(struct b43_wldev *dev, unsigned int core)
{
	struct b43_phy_ac_iq_acc *acc;
	u16 stride = (u16)(core * 0x200);
	u16 ii_hi, ii_lo, qq_hi, qq_lo, iq_hi, iq_lo;
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
	acc->ii[1] = acc->ii[0];
	acc->qq[1] = acc->qq[0];
	acc->iq[1] = acc->iq[0];
	acc->ii[0] = ((u32)ii_hi << 16) | ii_lo;
	acc->qq[0] = ((u32)qq_hi << 16) | qq_lo;
	acc->iq[0] = (s32)iq;
	acc->rounds++;
}

static void meas_v2_peek_c0_c5(struct b43_wldev *dev, unsigned int core)
{
	b43_phy_ac_iq_acc_peek(dev, core);
}

/*
 * IQ-cal measurement variante v2 (vendor #51814-#51956, 143 op).
 * Vedi commento in phy_ac.h.
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
		static const u16 bbmult = 0x0044;

		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
		b43_actab_write_bulk(dev, 0x000c, lo, 16, 1, &bbmult);
		b43_actab_write_bulk(dev, 0x000c, hi, 16, 1, &bbmult);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
	}

	/* Blocco H (2 op): MOD 0x0001 pulse finale */
	b43_phy_ac_cca_pulse(dev);
}

/*
 * Solve the RX-IQ coefficients (a, b) from one core's accumulators, summing
 * the two most recent estimate rounds. Same arithmetic as
 * b43_phy_ac_rx_iq_comp_update(), which the generic path does not reach yet.
 *
 * a = round(-(iq << 10) / ii), b = isqrt_near((qq << 20) / ii - a*a) - 1024,
 * where isqrt_near is the floor plus one when the remainder exceeds the root.
 *
 * Bit-exact on every coefficient on the attach path. On the warm path core
 * 0's b comes out one LSB low, 0x4b against 0x4c. That residue cannot be
 * chased with the oracle, which exposes only the sums and not the blob's
 * per-tone rounding; characterised in docs/retrace-todo.md. The warm gate
 * stands at 99.99%.
 */
static void b43_phy_ac_iq_solve(struct b43_phy_ac_iq_acc *acc,
				s16 *a_out, s16 *b_out)
{
	s64 num, v;
	s32 a;
	s32 b;

	u64 ii, qq;
	s64 iq;

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

	ii = (u64)acc->ii[0] + acc->ii[1];
	qq = (u64)acc->qq[0] + acc->qq[1];
	iq = (s64)acc->iq[0] + acc->iq[1];
	if (!ii) {
		*a_out = 0;
		*b_out = 0;
		return;
	}

	num = -(iq << 10);
	a = (s32)div64_s64(num + (num < 0 ? -(s64)(ii >> 1)
					  : (s64)(ii >> 1)), ii);

	v = (s64)div64_u64((qq << 20) + (ii >> 1), ii) - (s64)a * a;
	b = v > 0 ? (s32)int_sqrt64((u64)v) : 0;
	if (v > 0 && (u64)v - (u64)b * b > (u64)b)
		b++;
	b -= 1 << 10;

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
		"the RX IQ coefficient b lands one LSB away from what the stock "
		"driver writes, on both of the two boards it was checked "
		"against. The accumulators feeding it are reproduced exactly, "
		"so the difference is in the last step, and no rounding of the "
		"sum reaches the stock value. Receive image rejection may be "
		"marginally worse.\n");

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
 * Radio 2069 IQ-cal teardown (vendor #52256-#52267, 12 op).
 * Vedi commento in phy_ac.h.
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
 * RXIQ cal teardown + apply defaults (vendor #52268-#52452, 185 op).
 * Vedi commento in phy_ac.h per struttura.
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
	static const u16 gain_vals[3] = { 0x0000, 0x2f13, 0x00f3 };
	static const u16 bbmult_val = 0x0035;

	/*
	 * Gain-register reset values, 27 writes per core in the vendor's order,
	 * which is not contiguous by address -- note 0x0727 before 0x0726 and
	 * 0x0737 before 0x0736. The addresses given are core 0's; core 1 adds a
	 * +0x200 stride.
	 */
	static const struct { u16 reg; u16 val; } reset_regs[27] = {
		{ 0x073e, 0x0000 },
		{ 0x0678, 0x0008 },
		{ 0x0720, 0x0180 },
		{ 0x0721, 0x5000 },
		{ 0x0722, 0x0000 },
		{ 0x0723, 0x0000 },
		{ 0x0724, 0x0000 },
		{ 0x0725, 0x0600 },
		{ 0x0727, 0x0004 },  /* NB: 0x0727 PRIMA di 0x0726 */
		{ 0x0726, 0x000c },
		{ 0x0728, 0x0880 },
		{ 0x0729, 0x1000 },
		{ 0x0732, 0x0000 },
		{ 0x0733, 0x0000 },
		{ 0x0730, 0x0000 },
		{ 0x0731, 0x0000 },
		{ 0x0734, 0x0000 },
		{ 0x0735, 0x0000 },
		{ 0x0737, 0x0000 },  /* NB: 0x0737 PRIMA di 0x0736 */
		{ 0x0738, 0x0000 },
		{ 0x0736, 0x0000 },
		{ 0x0739, 0x0000 },
		{ 0x073a, 0x0180 },
		{ 0x073b, 0x002c },
		{ 0x073c, 0x0000 },
		{ 0x073d, 0x0000 },
		{ 0x0747, 0x0000 },
	};
	unsigned int c, i;

	/* Preamble globale (3 op) */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);
	b43_phy_write(dev,   0x0401, 0x7733);

	for (c = 0; c < 2; c++) {
		u16 stride = (u16)(c * 0x200);
		u16 discard;

		/* 3 fast TBL.WR gain (15 op) */
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 0x0007,
					     (u16)(0x0100 + c + i * 3),
					     16, 1, &gain_vals[i]);

		/* Sync (2 op) */
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0x0002);

		/* Fast TBL.RD 0x0020 off=0x0040 (5 op) */
		b43_actab_read_bulk(dev, 0x0020, 0x0040, 16, 1, &discard);

		/* The three fast gain table writes, repeated -- 15 ops, same
		 * values. */
		for (i = 0; i < 3; i++)
			b43_actab_write_bulk(dev, 0x0007,
					     (u16)(0x0100 + c + i * 3),
					     16, 1, &gain_vals[i]);

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

		/*
		 * Gain-register reset, 27 ops, rewriting them to their startup
		 * values in the observed order, which is not contiguous by
		 * address.
		 */
		for (i = 0; i < ARRAY_SIZE(reset_regs); i++)
			b43_phy_write(dev,
				      (u16)(reset_regs[i].reg + stride),
				      reset_regs[i].val);
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
 * docs/rxiq-cal-analysis.md section 12.
 *
 * The entry state is the PHY in release -- RX_WAITED and RX_OFDM, clip
 * enabled -- with the MAC suspended. The block toggles the MAC internally
 * during the polls and ends with it suspended; the outer framing belongs to
 * the callers.
 */
static void b43_phy_ac_measure_block(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* Per-core 0x?024/0x?025 baseline, restored by the radio reset. */
	u16 rad_restore[2][2];
	/*
	 * Two callers: rxiqcal_finalize(), after a probe cycle, and the
	 * periodic tick of b43_phy_ac_watchdog(). Both enter with the PHY in
	 * release -- RX_WAITED and RX_OFDM, clip enabled -- and the MAC
	 * suspended. The block toggles the MAC internally during the polls and
	 * ends with it suspended; the outer framing -- finalize's two arm
	 * toggles, the watchdog's suspend/enable pair -- belongs to the callers.
	 */
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
	 * clear. The plans live in test/main.c.
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
	 * Rxcal cleanup preamble (vendor #52805-#52809, 5 op).
	 *   peek 0x019e + peek 0x040f + MOD 0x040f clr bit 9 +
	 *   peek 0x0394 + peek 0x0393
	 */
	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_read_log(dev, 0x040f);
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, 0);
	b43_phy_read_log(dev, 0x0394);
	b43_phy_read_log(dev, 0x0393);

	/*
	 * Tail comune (vendor #52810-#52827, 18 op): stesso pattern di
	 * rxgain_perchan_config / iqcal_meas_readback_kick_tail — 3 peek+WR
	 * pair per core + 6 WR standalone reversed. Riuso helper condiviso.
	 */
	b43_phy_ac_rxgain_perchan_tail(dev);

	/* Arm tone gen (vendor #52828-#52830, 3 op) — 0x0394 = 0x0110. */
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
		static const struct { u16 off; u16 val; } gain_reset[14] = {
			{ 0x073e, 0x0000 },
			{ 0x0727, 0x0004 },
			{ 0x073c, 0x0000 },
			{ 0x0721, 0x5000 },
			{ 0x0729, 0x1000 },
			{ 0x0720, 0x0180 },
			{ 0x0728, 0x0880 },
			{ 0x0724, 0x0000 },
			{ 0x0736, 0x0000 },
			{ 0x0725, 0x0600 },
			{ 0x0739, 0x0000 },
			{ 0x073a, 0x0180 },
			{ 0x0722, 0x0000 },
			{ 0x0734, 0x0000 },
		};
		unsigned int c, i;

		for (c = 0; c < 2; c++) {
			u16 s = (u16)(c * 0x200);

			for (i = 0; i < ARRAY_SIZE(gain_reset); i++)
				b43_phy_write(dev,
					      gain_reset[i].off + s,
					      gain_reset[i].val);
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
	 * Finalize (vendor #53037-#53041, 5 op).
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
 * sweep in router-data/d6220/full-sweep.zip; the reference tick is extracted
 * to router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt.
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
 */
static void b43_phy_ac_wd_sample_phase_opt(struct b43_wldev *dev, bool peek,
					   bool arm_tone)
{
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

/* SHM statistics poll, with the MAC active. Order and repetitions are
 * transcribed from the reference tick: four scattered words, the 0x0768-0x078a
 * sweep, two hi/lo/hi passes over the six 32-bit counters, the three counters
 * at 0x07e0, 0x07e4 and 0x07dc, the 0x07d6-0x07da group and two trailing
 * words. */
/*
 * Poll delle statistiche SHM. Le catture mostrano tre forme, e le due varianti
 * qui le coprono tutte:
 *
 *   @head_sweep, @ctr32_passes   dove
 *   true, 2                      la forma piena, 54 letture in 0x0768-0x078a
 *   true, 0                      sola spazzata, 18 letture: su cold01 i tre
 *                                poll #12961, #13481 e #13540, nel path di up
 *   false, 1                     sola seconda meta' con una passata: cold01
 *                                #12894-#12956, dentro il blocco di config MAC
 *
 * Perche' le passate non ci siano sempre e' plausibile e non provato: leggono i
 * contatori come valori a 32 bit stabili, e prima che il MAC abbia contato
 * qualcosa non c'e' niente da leggere in quel modo. La spazzata piatta resta
 * perche' fa parte del latch della finestra.
 */
static void b43_phy_ac_wd_stats_poll_opt(struct b43_wldev *dev,
					 bool head_sweep,
					 unsigned int ctr32_passes)
{
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
	b43_phy_ac_wd_stats_poll_opt(dev, true, 2);
}

void b43_phy_ac_watchdog(struct b43_wldev *dev, bool noise_cal)
{
	B43_AC_FN();

	b43_phy_ac_wd_sample_phase(dev);
	b43_phy_ac_wd_stats_poll(dev);

	if (noise_cal) {
		b43_mac_suspend(dev);
		b43_phy_ac_measure_block(dev);
		b43_mac_enable(dev);
	}

	b43_phy_ac_wd_stats_tail(dev);
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
 * RXIQ cal finalize (vendor #52453-#55154, ~2700 op).
 * Vedi commento in phy_ac.h.
 */
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev)
{
	B43_AC_FN();
	/* LO DAC readback, 0x?002-0x?005 in block D, rewritten at the end. */
	u16 lo_dac[2][4];
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
	 * Block D, 49 ops: a table write at offset 0x5f, pairs of table reads
	 * per core, four radio reads and two peeks of 0x?a0/0x?a1 -- the
	 * readback of each core's RX-IQ corrector state.
	 */
	{
		static const u16 tbl_5f_val = 0xacdc;
		u16 discard16[2];
		unsigned int c;

		/* TBL.WR id=0x000c off=0x005f len=1 val=0xacdc (7 op) */
		b43_actab_write_bulk_scoped(dev, 0x000c, 0x005f, 16, 1,
					    &tbl_5f_val);

		for (c = 0; c < 2; c++) {
			u16 stride = (u16)(c * 0x200);

			/* TBL.RD off=0x60+c*4 len=2 (8 op) */
			b43_actab_read_bulk(dev, 0x000c,
					    (u16)(0x0060 + c * 4),
					    16, 2, discard16);
			/* MOD unlock esplicito (fine scope actab_read_bulk) */
			b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE, (u16)~0x0002, 0);
			/* TBL.RD off=0x62+c*4 len=1 (7 op) */
			b43_actab_read_bulk(dev, 0x000c,
					    (u16)(0x0062 + c * 4),
					    16, 1, discard16);
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
	 * Between the MHF and the first GPIO write, the attach capture shows
	 * two suspend/enable pairs that the port used to emit as structure. The
	 * timestamps refute that reading: they sit 1.37 s and 1.29 s after the
	 * MHF and come from a different CPU, so they are MAC activity
	 * interleaved by the up context during a wait -- the same species as
	 * the probe-pacing pairs. The warm path does not have them. They are
	 * declared in test/cmp_skip.py rather than emitted.
	 */
	bcma_chipco_gpio_out(&dev->dev->bdev->bus->drv_cc, 0x0004, 0x0004);
	bcma_chipco_gpio_out(&dev->dev->bdev->bus->drv_cc, 0x0400, 0x0000);

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
			: b43_phy_ac_crs_min_pwr(0,
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
	 * sola abilitazione (#30918) e subito dopo il poll (#30919); una coppia
	 * enable+suspend lascerebbe il MAC sospeso e sfaserebbe di un'op tutti
	 * e venti i tick.
	 */
	b43_mac_enable(dev);

	/*
	 * Probe and measure sequence.
	 *
	 * Non e' una schedule: la fase corre a una scadenza di orologio ed
	 * emette un tick per secondo. @probe_ticks e' la scadenza e
	 * @probe_watchdog_tick i tick su cui cade il measure block; entrambi
	 * vengono dal chiamante perche' l'harness non ha un clock. In un driver
	 * vero la scadenza e' jiffies e il measure block appartiene al lavoro
	 * periodico, non a qui.
	 *
	 * Un tick e' un giro del watchdog periodico, non un blocco a se': poll
	 * delle statistiche, latch della finestra, peek, clear e mode change.
	 * Le stesse funzioni che b43_phy_ac_watchdog() chiama, nell'ordine in
	 * cui questa fase le emette -- il tick a regime e' lo stesso ciclo
	 * tagliato in un altro punto, non una sequenza diversa.
	 *
	 * I 26 segmenti a freddo del d6220 inchiodano il measure block: cade sul
	 * tick 9 in tutti e 26, e sul tick 19 in ogni segmento la cui scadenza
	 * arriva fin la'. Sono quindi un timer di periodo dieci, e la coppia
	 * delle catture a caldo -- 5 e 15 -- e' lo stesso periodo a fase
	 * diversa. Puo' cadere sul tick di chiusura, oltre l'ultimo tick della
	 * fase, e allora quel tick porta solo il poll e il blocco.
	 *
	 * Le irregolarita' dei primi tre tick sono in
	 * b43_phy_ac_wd_sample_phase_opt(), che le documenta una per una.
	 */
	{
		struct b43_phy_ac *ac = dev->phy.ac;
		unsigned int ticks = ac->probe_ticks;
		unsigned int tick;

		for (tick = 0; tick < ticks; tick++) {
			b43_phy_ac_wd_stats_poll(dev);

			if (b43_phy_ac_watchdog_on_tick(ac, tick)) {
				b43_mac_suspend(dev);
				b43_phy_ac_measure_block(dev);
				b43_mac_enable(dev);
			}

			/*
			 * Il primo tick non latcha: la finestra e' stata letta
			 * poco sopra, prima del blocco CRS, e non ha avuto il
			 * tempo di riempirsi.
			 */
			if (tick)
				b43_phy_ac_wd_stats_tail(dev);

			b43_phy_ac_wd_sample_phase_opt(dev,
						       tick != 1 && tick != 2,
						       tick == 0);
		}

		/*
		 * Il tick di chiusura, quando la scadenza cade su un tick di
		 * misura: porta il poll, il blocco e il latch, e poi la fase
		 * finisce -- niente peek, clear o mode change.
		 */
		if (b43_phy_ac_watchdog_on_tick(ac, ticks)) {
			b43_phy_ac_wd_stats_poll(dev);
			b43_mac_suspend(dev);
			b43_phy_ac_measure_block(dev);
			b43_mac_enable(dev);
			b43_phy_ac_wd_stats_tail(dev);
		}

		ac->last_cal_channel = ac->cal_channel;
	}

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
	{
		/*
		 * Scaffolding: the maximum TX power index, and the most dangerous
		 * value in this file. On a different RF chain an index that is too
		 * high overdrives the PA.
		 *
		 * On a first bring-up the stock driver writes a constant 0x38 on
		 * every board captured; from a later channel setup on it writes
		 * the same target as txpwrctrl_setup(), so the derivation is
		 * shared with that site rather than computed again.
		 *
		 * Emitted from the highest core down, to match the vendor's order.
		 */
		bool first_bu = dev->phy.ac->status_mask &
				B43_PHY_AC_STATE_FIRST_BRINGUP;
		u16 lim1 = b43_phy_ac_txpwr_target(dev, 1) & 0x00ff;
		u16 lim0 = b43_phy_ac_txpwr_target(dev, 0) & 0x00ff;
		u16 mi1 = first_bu ? min_t(u16, 0x0038, lim1) : lim1;
		u16 mi0 = first_bu ? min_t(u16, 0x0038, lim0) : lim0;

		/*
		 * The clamp is not cosmetic. The 0x38 constant is scaffolding read
		 * off one board, while the derived limit describes this board's PA,
		 * through maxp5ga. On the three boards in the repository maxp5ga
		 * gives 0x42, 0x46 and 0x44, all above 0x38, so here the clamp is
		 * a no-op and the gates do not change. On a board with a lower
		 * maxp5ga, writing a raw 0x38 would exceed the maximum its front
		 * end declares.
		 *
		 * The general rule: a transcribed power value is never written
		 * without checking it against what the SROM declares. chip_id does
		 * not describe the PA -- femctrl and pdgain5g are identical across
		 * the three boards, while pa5ga and maxp5ga differ on all three.
		 */
		if (first_bu && lim0 < 0x0038)
			b43warn(dev->wl,
				"AC-PHY: max index TX di impalcatura (0x38) sopra il limite "
				"SROM di questa board (0x%02x): clampato.\n", lim0);

		b43_phy_maskset(dev, 0x0846, (u16)~0x00ff, mi1);
		b43_phy_maskset(dev, 0x0646, (u16)~0x00ff, mi0);
	}

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
	b43_maccontrol_set(dev, ~0x00040000u, 0);           /* clr bit 18 */
	b43_mac_enable(dev);
	b43_phy_ac_mhf_maskset(dev, 0, (u16)~0x4000, 0);
	b43_mac_suspend(dev);

	/*
	 * Per-core coefficient write; core 0 uses radio 0x0002-0x0005.
	 *
	 * Cells 0x62 and 0x66 hold 0x0002 and 0x0200 in both d6220 captures
	 * that record read values, so they are constants and do not depend on
	 * the phase. Cells 0x60/0x61 and 0x64/0x65 are the AFE cal's pass-1
	 * results and flow from afe_res_cal: the attach path gives
	 * (0x0066, 0x000e) and a later switch (0x0069, 0x000e). The readback of
	 * the 0x8056 iteration already carries the right value for the phase.
	 */
	{
		const u16 tbl_c_62 = 0x0002;

		b43_actab_write_bulk_scoped(dev, 0x000c, 0x0060, 16, 2,
					    dev->phy.ac->afe_res_cal[0].v);
		b43_actab_write_bulk_scoped(dev, 0x000c, 0x0062, 16, 1, &tbl_c_62);
	}
	b43_radio_write(dev, 0x0002, lo_dac[0][0]);
	b43_radio_write(dev, 0x0003, lo_dac[0][1]);
	b43_radio_write(dev, 0x0004, lo_dac[0][2]);
	b43_radio_write(dev, 0x0005, lo_dac[0][3]);
	/*
	 * Reapply the solved coefficients rather than recomputing them: the
	 * stock driver rewrites the same values as the first apply here.
	 */
	{
		s16 a, b;

		b43_phy_ac_iq_solve(&dev->phy.ac->iq_acc[0], &a, &b);
		b43_phy_write(dev, 0x06a0, (u16)(a & 0x03ff));
		b43_phy_write(dev, 0x06a1, (u16)(b & 0x03ff));
	}

	/* Core 1 (RAD.WR 0x0202-0x0205): come il core 0, risultato pass 1
	 * (attach 0x0027/0x0003, switch 0x0026/0x0004). */
	{
		const u16 tbl_c_66 = 0x0200;

		b43_actab_write_bulk_scoped(dev, 0x000c, 0x0064, 16, 2,
					    dev->phy.ac->afe_res_cal[2].v);
		b43_actab_write_bulk_scoped(dev, 0x000c, 0x0066, 16, 1, &tbl_c_66);
	}
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

	/* Arm the analog front-end for RX/TX: the bss-up radio-ON step. */
	b43_phy_ac_enable_afe(dev, B43_PHY_AC_AFE_ON);

	/* Chan-select final + PMU release. */
	b43_phy_maskset(dev, 0x0408, (u16)~0x0002, 0);
	b43_phy_write(dev, 0x0417, 0x0000);
	b43_phy_write(dev, 0x0416, 0x0001);
	b43_phy_ac_pmu_req(dev, true);
}

static int b43_phy_ac_op_switch_channel(struct b43_wldev *dev, unsigned int new_channel)
{
	struct ieee80211_channel *channel = dev->wl->hw->conf.chandef.chan;
	enum nl80211_channel_type channel_type = cfg80211_get_chandef_type(&dev->wl->hw->conf.chandef);
	int entry_suspended;
	int ret;

	if (b43_current_band(dev->wl) == NL80211_BAND_2GHZ) {
		b43dbg(dev->wl,
		       "AC-PHY: 2.4 GHz channel %u not supported on this board\n",
		       new_channel);
		return -EOPNOTSUPP;
	}

	/*
	 * Unlike wl, which needs a full down and up to change channel at
	 * runtime, b43 keeps the MAC active between channel switches, and
	 * set_channel() operates with it suspended.
	 *
	 * Suspend only if it is not already suspended: on bring-up the MAC
	 * arrives disabled from the core init, and a nested suspend would raise
	 * the refcount without writing, turning the calibration's
	 * enable/suspend pairs into no-ops. entry_suspended distinguishes "this
	 * function suspended it" from "it was already suspended", which the
	 * state no longer tells apart by the end of the function.
	 *
	 * The preamble writes MACCTL through b43_maccontrol_set(), which does
	 * not go through the refcount: two routes to the same bit, to be
	 * unified.
	 */
	entry_suspended = dev->mac_suspended;
	if (!entry_suspended)
		b43_mac_suspend(dev);
	/*
	 * b43_mac_suspend() and b43_mac_enable() maintain the MAC_EN mirror.
	 * Setting it by hand here used to put it out of step with the refcount
	 * whenever the suspend was conditional: on bring-up the MAC was not
	 * re-enabled but MAC_EN was raised anyway, and rxiqcal_apply() then
	 * failed the precondition that forbids it.
	 */

	/* 5 GHz: the channel table is the filter (unknown channels -ESRCH). */
	ret = b43_phy_ac_set_channel(dev, channel, channel_type);

	/*
	 * L'enable incondizionato non e' qui. Non e' il rilascio di cio' che
	 * questa funzione ha acquisito: e' un requisito delle calibrazioni
	 * post-canale, che girano a MAC attivo e lo pretendono nelle loro
	 * precondizioni. Siccome quelle le invoca il chiamante, l'enable tocca
	 * a lui -- e questo lascia libera la finestra fra le due fasi, dove il
	 * core scrive la configurazione BSS con il MAC ancora sospeso.
	 */

	/*
	 * Post-channel calibrations, which the vendor emits after the MAC.MCTRL
	 * enable: post_cal_finalize() for iterations 2 and 3, rxiqcal
	 * iterations 1 to 24, the rxcal AFE, the first and second txpwr rounds
	 * with their RX-IQ measurement and the gainctrl_final loop, then the
	 * final RX-IQ teardown.
	 */
	/*
	 * Le calibrazioni post-canale non sono qui: le invoca il chiamante,
	 * come in b43 fa il core dopo il ritorno di b43_switch_channel(). Sono
	 * centinaia di op, e tenerle in coda a questa funzione impedisce di
	 * inserire fra le due fasi la configurazione BSS che il core scrive.
	 */
	return ret;
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

static void b43_phy_ac_farrow_setup(struct b43_wldev *dev,
				    struct ieee80211_channel *channel)
{
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

	b43_phy_read_log(dev, 0x0601);

	b43_phy_write(dev, 0x1601, 0x0049);

	/* Raw tap tables, not consumed by this block. */
	(void)b43_phy_ac_farrow_vals;
	(void)b43_phy_ac_farrow_vals_432x_media_a1;
}
