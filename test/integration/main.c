// SPDX-License-Identifier: GPL-2.0
/*
 * Il chiamante: fa partire b43 vero e ne raccoglie le op.
 *
 * L'aggancio non e' una chiamata diretta a `b43_bcma_probe`, che e' statica.
 * `module_init(b43_init)` con -DMODULE crea l'alias `init_module`, quindi:
 *
 *     init_module()  ->  b43_init()  ->  __bcma_driver_register(&b43_bcma_driver)
 *
 * e il nostro `__bcma_driver_register` cattura la `probe`. Da li' la chiamiamo
 * con un core finto. Cosi' non si tocca b43 e non si duplica la sua sequenza
 * di attach: la percorre lui.
 *
 * Compilato coi flag del KERNEL, perche' costruisce `struct bcma_device` e
 * `struct bcma_bus` veri: i loro campi sono ingresso dei gate di b43
 * (`core->id.rev`, `chipinfo.id`, `chipinfo.rev`), e con strutture inventate
 * i gate prenderebbero il ramo sbagliato in silenzio.
 *
 * Le op le emette bcma_stub.c attraverso la vtable. Le letture le serve
 * l'oracolo: senza B43_READ_ORACLE ogni lettura e' zero e la traccia non e'
 * una misura -- vedi trace_out.c.
 */
#include <linux/bcma/bcma.h>
#include <linux/pci.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ssb/ssb.h>
#include <linux/string.h>
#include <linux/etherdevice.h>

#include "../board_profile.h"
#include "trace_out.h"

extern const struct bcma_host_ops b43_test_bcma_ops;

/*
 * Compilato coi flag del kernel, quindi senza stdio: il messaggio va a una
 * funzione di trace_out.c, che e' codice utente. Includere <stdio.h> qui
 * romperebbe -nostdinc, ed e' proprio la separazione che tiene le due meta'
 * della suite compilabili.
 */
void b43_trace_note(const char *fmt, int arg);

/* Catturata da __bcma_driver_register in kernel_shim.c. */
extern int (*b43_test_probe)(struct bcma_device *core);
extern void (*b43_test_remove)(struct bcma_device *core);

int init_module(void);
void cleanup_module(void);

/*
 * Il core 802.11: corerev 42 su tutte le board della collezione (ucode42, AC
 * rev1, `corerev 0x2a` in ogni wl1_revinfo.txt). Chip, chiprev e deviceid PCI
 * cambiano da board a board e vengono dal profilo: sono i valori che decidono
 * i gate -- b43_supported_bands (main.c:5494) legge `bus->host_pci->device`
 * quando l'hosttype e' PCI -- quindi sono dati della board e non una scelta
 * qui.
 */
#define TEST_CORE_REV		42
#define TEST_CORE_ID		BCMA_CORE_80211

static struct pci_dev test_pci;
static struct bcma_bus test_bus;
static struct bcma_device test_core;
static struct bcma_device test_cc_core;

static void build_core(void)
{
	const struct board_profile *board =
		board_profile_lookup(b43_test_env("B43_BOARD"));

	test_bus.ops = &b43_test_bcma_ops;
	test_bus.hosttype = BCMA_HOSTTYPE_PCI;
	test_pci.device = board->pci_device;
	test_bus.host_pci = &test_pci;
	test_bus.chipinfo.id = board->chip_id;
	test_bus.chipinfo.rev = board->chip_rev;
	test_bus.chipinfo.pkg = 0x1;

	/*
	 * b43 legge la ChipCommon per la SPROM e i GPIO. Il core c'e' perche'
	 * un puntatore nullo la' dentro non e' un ramo che b43 gestisce.
	 */
	test_cc_core.bus = &test_bus;
	test_cc_core.id.id = BCMA_CORE_CHIPCOMMON;
	test_bus.drv_cc.core = &test_cc_core;

	/*
	 * La SROM. Senza, b43 gira su una board azzerata: femctrl=0 -- che il
	 * driver segnala -- rxchain=0, coremask=0, subband5gver=0 e pa5ga tutta
	 * a zero, cioe' ogni valore che dipende dalla board sbagliato in
	 * silenzio. Il profilo e' condiviso con ../unit, che e' la stessa
	 * board, e si sceglie con B43_BOARD come la' con argv.
	 *
	 * Su hardware la riempie bcma_sprom_extract_r11(); qui il profilo sta
	 * al suo posto, con le stesse maschere.
	 */
	board_profile_to_sprom(board, &test_bus.sprom);

	test_core.bus = &test_bus;
	test_core.id.id = TEST_CORE_ID;
	test_core.id.rev = TEST_CORE_REV;
	test_core.id.manuf = BCMA_MANUF_BCM;
	test_core.core_index = 0;
}

/*
 * Il canale NON si inietta da qui, e il tentativo e' stato istruttivo: `dev`
 * nasce dentro la probe, quindi l'harness non lo raggiunge prima. Ma il
 * problema non e' il banco -- vedi il README, sezione "il difetto che la
 * suite ha trovato".
 */
extern const struct ieee80211_ops *b43_test_hw_ops;
extern struct ieee80211_hw *b43_test_hw;

/* Il kernel e il microcodice, dagli stub. */
void b43_test_run_until(unsigned long until);
void b43_test_irq(void);
void b43_test_raise_irq(u32 reason);
void b43_test_raise_dma0(u32 reason);

/*
 * Il campione di rumore pronto, bit di B43_MMIO_GEN_IRQ_REASON: e' il
 * registro dell'hardware, lo stesso valore di b43.h.
 */
#define TEST_IRQ_NOISESAMPLE_OK	0x00040000

static struct ieee80211_vif *test_vif;

static void test_config(u32 changed)
{
	b43_test_hw_ops->config(b43_test_hw, changed);
}

static void test_bss(u64 changed)
{
	b43_test_hw_ops->bss_info_changed(b43_test_hw, test_vif,
					  &test_vif->bss_conf, changed);
}

/*
 * Quel che mac80211 fa all'ifup di un'interfaccia AP, dopo drv_start():
 * drv_add_interface(), ieee80211_hw_config(~0) e i parametri WMM di default
 * dell'AP, una conf_tx per coda.
 */
static void test_ifup(void)
{
	static const struct ieee80211_tx_queue_params wmm[IEEE80211_NUM_ACS] = {
		[IEEE80211_AC_VO] = { .cw_min = 3,  .cw_max = 7,    .aifs = 1,
				      .txop = 47 },
		[IEEE80211_AC_VI] = { .cw_min = 7,  .cw_max = 15,   .aifs = 1,
				      .txop = 94 },
		[IEEE80211_AC_BE] = { .cw_min = 15, .cw_max = 63,   .aifs = 3 },
		[IEEE80211_AC_BK] = { .cw_min = 15, .cw_max = 1023, .aifs = 7 },
	};
	unsigned int ac;

	test_vif = kzalloc(sizeof(*test_vif) + b43_test_hw->vif_data_size,
			   GFP_KERNEL);
	test_vif->type = NL80211_IFTYPE_AP;
	memcpy(test_vif->addr, b43_test_hw->wiphy->perm_addr, ETH_ALEN);
	b43_test_hw_ops->add_interface(b43_test_hw, test_vif);
	test_config(~0);
	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
		b43_test_hw_ops->conf_tx(b43_test_hw, test_vif, 0, ac, &wmm[ac]);
}

/*
 * start_ap: hostapd da' il BSS e mac80211 lo passa al driver, beacon
 * compreso, in un solo bss_info_changed. DTIM 3 e il basic set {6, 12, 24}
 * sono quelli della cattura.
 */
static void test_start_ap(void)
{
	struct ieee80211_bss_conf *bss = &test_vif->bss_conf;

	bss->bssid = test_vif->addr;
	bss->beacon_int = 100;
	bss->dtim_period = 3;
	bss->basic_rates = BIT(0) | BIT(2) | BIT(4);
	bss->use_short_slot = true;
	bss->enable_beacon = true;
	test_bss(BSS_CHANGED_BSSID | BSS_CHANGED_BEACON_INT |
		 BSS_CHANGED_BASIC_RATES | BSS_CHANGED_ERP_SLOT |
		 BSS_CHANGED_BEACON | BSS_CHANGED_BEACON_ENABLED);
}

/*
 * Il channel availability check come lo fa mac80211 per un driver senza
 * channel context: il tune col radar acceso, poi a check finito il rilascio
 * -- 20 MHz senza HT sullo stesso primario, radar spento -- e il tune di
 * start_ap, col radar di nuovo acceso.
 */
static void test_cac_start(void)
{
	b43_test_hw->conf.radar_enabled = true;
	test_config(IEEE80211_CONF_CHANGE_CHANNEL);
}

static void test_cac_end(void)
{
	struct cfg80211_chan_def def = b43_test_hw->conf.chandef;

	b43_test_hw->conf.radar_enabled = false;
	b43_test_hw->conf.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
	b43_test_hw->conf.chandef.center_freq1 = def.chan->center_freq;
	test_config(IEEE80211_CONF_CHANGE_CHANNEL);
	b43_test_hw->conf.chandef = def;
	b43_test_hw->conf.radar_enabled = true;
	test_config(IEEE80211_CONF_CHANGE_CHANNEL);
	test_start_ap();
}

/*
 * L'ambiente dopo il bring-up, dalla timeline della cattura. L'orologio del
 * driver sono i jiffies, che qui avanzano solo quando lo dice la timeline:
 * l'istante di ogni evento si porta in jiffies e prima dell'evento scattano i
 * delayed work scaduti -- il periodic work, il poll del radar. I giri del
 * watchdog del vendor quindi non si consegnano, li fa il periodic work di
 * b43; servono a mettere in scala l'orologio: il vendor dichiara un secondo e
 * la cattura ne misura 1.004, e il primo giro cade un secondo dopo lo start,
 * come quello di b43.
 *
 *   NOISE   il campione pronto: B43_IRQ_NOISESAMPLE_OK dall'hard handler
 *   IRQ     un'interruzione del microcodice come la cattura l'ha vista:
 *           la causa in GEN_IRQ_REASON e lo stato del canale DMA 0, RX_DONE
 *           quando c'e' un frame. Cosa b43 ne fa -- l'ACK, le maschere, la
 *           lettura dell'anello di ricezione -- e' suo. Quelle cadute
 *           durante il bring-up del vendor arrivano tutte insieme all'inizio,
 *           perche' qui il bring-up e' una sola chiamata e l'orologio parte
 *           dopo.
 *   TPL     il beacon cambiato: bss_info_changed(BSS_CHANGED_BEACON)
 *   BSS_UP  il check chiuso: il rilascio del canale e start_ap
 *   WD, POLL  i timer del driver, che scattano da se'
 */
static void test_environment(bool cac)
{
	long long t0, t1, t;
	char kind[16];
	u32 a0, a1;
	int n;

	if (!b43_test_timeline_wd(&t0, &t1, &n) || n < 2) {
		if (cac)
			test_cac_end();
		return;
	}
	while (b43_test_timeline_next(&t, kind, sizeof(kind), &a0, &a1)) {
		long long rel = t - t0;
		unsigned long j = HZ + (unsigned long)
			(rel < 0 ? 0 : rel * (n - 1) * HZ / (t1 - t0));

		b43_test_run_until(j);
		if (!strcmp(kind, "NOISE")) {
			b43_test_raise_irq(TEST_IRQ_NOISESAMPLE_OK);
			b43_test_irq();
		} else if (!strcmp(kind, "IRQ")) {
			b43_test_raise_irq(a0);
			b43_test_raise_dma0(a1);
			b43_test_irq();
		} else if (!strcmp(kind, "TPL")) {
			test_bss(BSS_CHANGED_BEACON);
		} else if (!strcmp(kind, "BSS_UP")) {
			if (cac) {
				cac = false;
				test_cac_end();
			}
		}
	}
}

int main(void)
{
	int err;

	build_core();

	err = init_module();
	if (err) {
		b43_trace_note("init_module: %d\n", err);
		return 1;
	}
	if (!b43_test_probe) {
		b43_trace_note("b43 non ha registrato una probe bcma; "
			       "compilato senza CONFIG_B43_BCMA?%d\n", 0);
		return 1;
	}

	err = b43_test_probe(&test_core);
	b43_trace_note("probe: %d\n", err);

	/*
	 * Il bring-up. La probe fa solo l'attach: cio' che emette le ~29k op
	 * del segmento e' il percorso di `ifconfig up`, che nel kernel entra
	 * da mac80211 con hw->ops->start(). Chiamarlo qui e' l'aggancio
	 * giusto, e non tocca b43: la vtable la cattura
	 * ieee80211_alloc_hw_nm() nello stub, come __bcma_driver_register
	 * cattura la probe.
	 */
	if (!err && b43_test_hw_ops && b43_test_hw_ops->start) {
		int up = b43_test_hw_ops->start(b43_test_hw);

		b43_trace_note("start: %d\n", up);
		if (!up) {
			bool cac = b43_test_hw->conf.chandef.chan->flags &
				   IEEE80211_CHAN_RADAR;

			test_ifup();
			if (cac)
				test_cac_start();
			else
				test_start_ap();
			test_environment(cac);
			b43_test_hw_ops->remove_interface(b43_test_hw, test_vif);
			if (b43_test_hw_ops->stop)
				b43_test_hw_ops->stop(b43_test_hw);
		}
	}

	/*
	 * L'esito NON e' il risultato del test: il risultato e' la traccia.
	 * Una probe che ritorna 0 e non emette op e' un fallimento, e una che
	 * ritorna un errore dopo aver emesso il bring-up e' informazione --
	 * di solito il punto in cui uno stub non basta piu'.
	 */
	/* Solo se la probe e' riuscita: altrimenti non c'e' drvdata da
	 * liberare e la remove va a leggere un puntatore che nessuno ha
	 * scritto. */
	if (!err && b43_test_remove)
		b43_test_remove(&test_core);
	cleanup_module();
	return 0;
}
