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
 * Il core 802.11 del d6220, dai suoi dump: chip 0x4352, corerev 42 (ucode42,
 * AC rev1). Sono i valori che decidono i gate, quindi vengono dai dati della
 * board e non da una scelta qui -- vedi router-data/d6220/wl1_revinfo.txt.
 */
#define D6220_CHIP_ID		0x4352
#define D6220_CHIP_REV		0x3
#define D6220_CORE_REV		42
#define D6220_CORE_ID		BCMA_CORE_80211

/*
 * Il device PCI. b43_supported_bands (main.c:5494) legge
 * `bus->host_pci->device` quando l'hosttype e' PCI, quindi dichiarare PCI
 * senza fornirlo e' una promessa non mantenuta: il puntatore nullo la' non e'
 * un ramo che b43 gestisce.
 *
 * Il device id e' quello del 4352 su questo hardware, 0x43b3, che
 * patches/0009 aggiunge alla tabella del bridge PCI di bcma. b43 lo usa per
 * decidere le bande su alcuni chip: sceglierne uno a caso farebbe registrare
 * le bande sbagliate in silenzio.
 */
#define D6220_PCI_DEVICE	0x43b3

static struct pci_dev test_pci;
static struct bcma_bus test_bus;
static struct bcma_device test_core;
static struct bcma_device test_cc_core;

static void build_core(void)
{
	test_bus.ops = &b43_test_bcma_ops;
	test_bus.hosttype = BCMA_HOSTTYPE_PCI;
	test_pci.device = D6220_PCI_DEVICE;
	test_bus.host_pci = &test_pci;
	test_bus.chipinfo.id = D6220_CHIP_ID;
	test_bus.chipinfo.rev = D6220_CHIP_REV;
	test_bus.chipinfo.pkg = 0x1;

	/*
	 * b43 legge la ChipCommon per la SPROM e i GPIO. Il core c'e' perche'
	 * un puntatore nullo la' dentro non e' un ramo che b43 gestisce.
	 */
	test_cc_core.bus = &test_bus;
	test_cc_core.id.id = BCMA_CORE_CHIPCOMMON;
	test_bus.drv_cc.core = &test_cc_core;

	test_core.bus = &test_bus;
	test_core.id.id = D6220_CORE_ID;
	test_core.id.rev = D6220_CORE_REV;
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
		if (!up && b43_test_hw_ops->stop)
			b43_test_hw_ops->stop(b43_test_hw);
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
