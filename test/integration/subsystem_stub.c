// SPDX-License-Identifier: GPL-2.0
/*
 * Il resto del livello sotto: controllo bcma, ssb, mac80211 e i sottosistemi
 * di b43 che non emettono op.
 *
 * Compilato coi flag del KERNEL, non come utente, e non per gusto: qui
 * servono i tipi veri. `ieee80211_alloc_hw_nm` deve restituire una
 * `struct ieee80211_hw` la cui `priv` sia allineata come nel kernel, perche'
 * b43 ci mette la sua `b43_wl` e la rilegge con `hw_to_b43_wl()`. Con una
 * struttura inventata il puntatore cadrebbe altrove e il difetto si vedrebbe
 * mille op piu' tardi.
 *
 * Cosa emette op e cosa no, per non cercarlo dopo: le op passano SOLO dalla
 * vtable di bcma_stub.c. I dieci simboli `bcma_*` qui sotto sono clock,
 * enable, GPIO e PLL, e nella traccia del vendor non compaiono -- il tracer di
 * wl aggancia le funzioni di accesso, non quelle di gestione del core. Quindi
 * no-op e' la risposta giusta, non una scorciatoia.
 *
 * `ssb_*` sono no-op perche' qui serve solo BCMA: b43 compila con
 * CONFIG_B43_BCMA e il ramo ssb non viene percorso, ma il link li chiede
 * comunque.
 */
#include <linux/bcma/bcma.h>
#include <linux/ssb/ssb.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <net/mac80211.h>

#include "trace_out.h"

/* --- bcma: gestione del core, non accesso ai registri ------------------ */

int bcma_core_enable(struct bcma_device *core, u32 flags) { return 0; }
void bcma_core_disable(struct bcma_device *core, u32 flags) { }
bool bcma_core_is_enabled(struct bcma_device *core) { return true; }
void bcma_core_set_clockmode(struct bcma_device *core,
			     enum bcma_clkmode clkmode) { }
void bcma_core_pll_ctl(struct bcma_device *core, u32 req, u32 status,
		       bool on) { }
void bcma_host_pci_up(struct bcma_bus *bus) { }
void bcma_host_pci_down(struct bcma_bus *bus) { }
int bcma_host_pci_irq_ctl(struct bcma_bus *bus, struct bcma_device *core,
			  bool enable)
{
	return 0;
}

/* --- ChipCommon: QUESTE EMETTONO OP ------------------------------------
 *
 * A differenza dei simboli qui sopra, i GPIO e i registri del PMU compaiono
 * nella traccia del vendor, con le classi che il decoder gli da': GPIO.CTL,
 * GPIO.OUT, GPIO.OE, PMU.PLL, PMU.RC. Il port li usa nel bring-up, quindi
 * stubbarli a no-op cancellerebbe op che devono essere confrontate -- e il
 * punteggio salirebbe togliendo lavoro, che e' il modo peggiore di sbagliare.
 *
 * `gpio_outen` la traccia la chiama GPIO.OE: e' lo stesso registro con due
 * nomi, l'harness lo prende dal simbolo bcma e il tracer vendor dal registro,
 * e la normalizzazione di reverse-tools/tracelib.py li allinea gia'.
 */
u32 bcma_chipco_gpio_control(struct bcma_drv_cc *cc, u32 mask, u32 value)
{
	b43_trace_gpio("GPIO.CTL", value, mask);
	return value;
}

u32 bcma_chipco_gpio_out(struct bcma_drv_cc *cc, u32 mask, u32 value)
{
	b43_trace_gpio("GPIO.OUT", value, mask);
	return value;
}

u32 bcma_chipco_gpio_outen(struct bcma_drv_cc *cc, u32 mask, u32 value)
{
	b43_trace_gpio("GPIO.OUTEN", value, mask);
	return value;
}

u32 bcma_chipco_pll_read(struct bcma_drv_cc *cc, u32 offset)
{
	u32 v = b43_trace_read("PMU.PLL", (u16)offset, 32);

	b43_trace_op("PMU.PLL", (u16)offset, v, 0, -1);
	return v;
}

/* La mask del tracer sono i bit toccati, cioe' il complemento di quella
 * del kernel: la stessa convenzione di MAC.MCTRL e dei MOD. */
void bcma_chipco_regctl_maskset(struct bcma_drv_cc *cc, u32 offset,
				u32 mask, u32 set)
{
	b43_trace_op32("PMU.RC", (u16)offset, set, ~mask);
}

/* --- ssb: non percorso, ma il link lo chiede -------------------------- */

int ssb_bus_powerup(struct ssb_bus *bus, bool dynamic_pctl) { return 0; }
int ssb_bus_may_powerdown(struct ssb_bus *bus) { return 0; }
void ssb_device_enable(struct ssb_device *dev, u32 core_specific_flags) { }
void ssb_device_disable(struct ssb_device *dev, u32 core_specific_flags) { }
int ssb_device_is_enabled(struct ssb_device *dev) { return 1; }
void ssb_commit_settings(struct ssb_bus *bus) { }
void ssb_driver_unregister(struct ssb_driver *drv) { }
void ssb_set_devtypedata(struct ssb_device *dev, void *data) { }
int ssb_pcicore_dev_irqvecs_enable(struct ssb_pcicore *pc,
				   struct ssb_device *dev)
{
	return 0;
}

/* --- mac80211: la sola che deve restituire qualcosa di usabile -------- */

struct ieee80211_hw *ieee80211_alloc_hw_nm(size_t priv_data_len,
					   const struct ieee80211_ops *ops,
					   const char *requested_name)
{
	struct ieee80211_hw *hw;

	extern const struct ieee80211_ops *b43_test_hw_ops;
	extern struct ieee80211_hw *b43_test_hw;

	/*
	 * Nel kernel `priv` sta dopo la struttura, allineata; qui si fa lo
	 * stesso con una sola allocazione, cosi' `hw->priv` e' un puntatore
	 * valido e b43 ci scrive la sua b43_wl.
	 */
	hw = kzalloc(sizeof(*hw) + priv_data_len, GFP_KERNEL);
	if (!hw)
		return NULL;
	hw->priv = (char *)hw + sizeof(*hw);
	hw->wiphy = kzalloc(sizeof(*hw->wiphy), GFP_KERNEL);
	b43_test_hw_ops = ops;
	b43_test_hw = hw;
	return hw;
}

void ieee80211_free_hw(struct ieee80211_hw *hw)
{
	if (hw)
		kfree(hw->wiphy);
	kfree(hw);
}

/*
 * La vtable di b43, catturata come `__bcma_driver_register` cattura la probe:
 * b43_op_start e' statica, e senza questo l'harness non ha modo di far
 * partire il bring-up dopo la probe.
 */
const struct ieee80211_ops *b43_test_hw_ops;
struct ieee80211_hw *b43_test_hw;

/*
 * register_hw sceglie il canale, che nel kernel fa mac80211 e non b43:
 * b43_phy_init() (phy_common.c:93) legge
 * dev->wl->hw->conf.chandef.chan->hw_value, e se nessuno ha impostato quel
 * puntatore il bring-up muore la'.
 *
 * Il canale e la larghezza si prendono dall'ambiente, `B43_CHANNEL` e
 * `B43_BW`, perche' i 26 segmenti dello sweep sono 26 configurazioni e una
 * suite che sa fare solo la prima non li misura. Sono gli stessi due
 * parametri che `../unit` prende come AC_CHANNEL e AC_BW; il prefisso e'
 * B43_ per stare con B43_READ_ORACLE e B43_TRACE_OUT di questa suite.
 *
 * Il canale non si inventa: si cerca fra quelli che b43 ha registrato in
 * b43_setup_bands() (main.c:5267), che e' la stessa restrizione che ha
 * mac80211. Se `B43_CHANNEL` non e' fra quelli lo si dice e si prende il
 * primo, invece di lasciare un puntatore a un canale che il driver non
 * conosce.
 *
 * `center_freq1` per BW40 e BW80 e' il centro del blocco legato, non del
 * canale: e' quello che b43_phy_ac_write_chanspec() legge per comporre il
 * chanspec. La formula e' la stessa di ../unit/main.c.
 */
static enum nl80211_chan_width shim_width(long bw)
{
	switch (bw) {
	case 40:  return NL80211_CHAN_WIDTH_40;
	case 80:  return NL80211_CHAN_WIDTH_80;
	default:  return NL80211_CHAN_WIDTH_20;
	}
}

/*
 * Il regdomain che cfg80211 applica ai canali alla registrazione, abbassando
 * il max_power che il driver ha dichiarato. La cattura a freddo e' di un wl
 * con ccode= vuoto nel NVRAM, cioe' con la sua locale interna, e i tetti che
 * quella scrive sono board-independent: 21 dBm EIRP su ch36-48 a 20 MHz e 26
 * su ch100 (vedi b43_phy_ac_reg_ceiling in src/phy_ac.c). Non c'e' niente
 * nel bordo da cui ricavarli, e sul ferro cfg80211 applicherebbe il world
 * regdomain, che e' un altro numero per policy: qui vale la locale del
 * vendor, perche' e' la sua traccia che si confronta. Gli altri canali
 * restano al max_power che b43 registra e il tetto non lega, come sul
 * vendor a caldo.
 */
static const struct {
	u16 chan;
	s8 dbm;
} wl_default_locale_5g[] = {
	{ 36, 21 }, { 40, 21 }, { 44, 21 }, { 48, 21 }, { 100, 26 },
};

static struct ieee80211_hw *g_hw;

static void apply_regdomain(struct ieee80211_supported_band *sb)
{
	unsigned int i, j;

	for (i = 0; i < sb->n_channels; i++)
		for (j = 0; j < ARRAY_SIZE(wl_default_locale_5g); j++)
			if (sb->channels[i].hw_value == wl_default_locale_5g[j].chan &&
			    sb->channels[i].max_power > wl_default_locale_5g[j].dbm)
				sb->channels[i].max_power = wl_default_locale_5g[j].dbm;
}

int ieee80211_register_hw(struct ieee80211_hw *hw)
{
	long want = b43_test_env_long("B43_CHANNEL", 0);
	long bw = b43_test_env_long("B43_BW", 20);
	struct ieee80211_channel *first = NULL, *pick = NULL;
	enum nl80211_band band;
	int i;

	g_hw = hw;
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *sb = hw->wiphy->bands[band];

		if (!sb || !sb->n_channels)
			continue;
		apply_regdomain(sb);
		if (!first)
			first = &sb->channels[0];
		for (i = 0; i < sb->n_channels; i++) {
			if (sb->channels[i].hw_value == want) {
				pick = &sb->channels[i];
				break;
			}
		}
		if (pick)
			break;
	}

	if (!pick) {
		if (want)
			b43_trace_note("B43_CHANNEL=%d non e' fra i canali che "
				       "b43 ha registrato; uso il primo\n",
				       (int)want);
		pick = first;
	}
	if (!pick) {
		b43_trace_note("b43 non ha registrato nessuna banda%d\n", 0);
		return 0;
	}

	hw->conf.chandef.chan = pick;
	hw->conf.chandef.width = shim_width(bw);
	hw->conf.chandef.center_freq1 = pick->center_freq +
		(bw == 40 ? 10 : bw == 80 ? 30 : 0);
	/*
	 * mac80211 hands the driver a power level with the channel, the
	 * channel's own max_power when userspace has not asked for less. Zero
	 * is the value b43_op_config() reads as "no power level", and on it
	 * the TX power check -- and adjust_txpower behind it -- never runs.
	 */
	hw->conf.power_level = pick->max_power;
	return 0;
}
void ieee80211_unregister_hw(struct ieee80211_hw *hw) { }
void ieee80211_wake_queues(struct ieee80211_hw *hw) { }
void ieee80211_stop_queue(struct ieee80211_hw *hw, int queue) { }
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb) { }
void ieee80211_handle_wake_tx_queue(struct ieee80211_hw *hw,
				    struct ieee80211_txq *txq) { }
/* In linea, come queue_work_on(): la suite e' a un thread, e il work che
 * b43_phy_txpower_check() accoda e' adjust_txpower, cioe' op da confrontare. */
void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work)
{
	if (work && work->func)
		work->func(work);
}
void ieee80211_queue_delayed_work(struct ieee80211_hw *hw,
				  struct delayed_work *work,
				  unsigned long delay) { }
struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw,
					 struct ieee80211_vif *vif,
					 u16 *tim_offset, u16 *tim_length,
					 unsigned int link_id)
{
	return NULL;
}
const struct ieee80211_rate *
ieee80211_get_response_rate(struct ieee80211_supported_band *sband,
			    u32 basic_rates, int bitrate)
{
	return sband && sband->bitrates ? &sband->bitrates[0] : NULL;
}

/* --- i sottosistemi di b43: nessuno emette op sul percorso di init ----
 *
 * DMA e PIO sono i percorsi dati: init deve RIUSCIRE, o
 * b43_wireless_core_init esce prima di b43_security_init. Non emettono op
 * sulla traccia che confrontiamo, ma il loro esito e' un gate.
 */
struct b43_wldev;

int b43_dma_init(struct b43_wldev *dev) { return 0; }
void b43_dma_free(struct b43_wldev *dev) { }
void b43_dma_tx(struct b43_wldev *dev, struct sk_buff *skb) { }
void b43_dma_rx(void *ring) { }
void b43_dma_handle_rx_overflow(void *ring) { }
int b43_pio_init(struct b43_wldev *dev) { return 0; }
void b43_pio_free(struct b43_wldev *dev) { }
void b43_pio_tx(struct b43_wldev *dev, struct sk_buff *skb) { }
void b43_pio_rx(void *q) { }
/*
 * Non stubbati: stanno in xmit.c, che si compila. Erano qui quando xmit.c
 * non era nella lista, e tenerli darebbe "multiple definition".
 *   b43_handle_txstatus, b43_generate_plcp_hdr,
 *   b43_plcp_get_ratecode_cck, b43_plcp_get_ratecode_ofdm
 */

int b43_leds_register(struct b43_wldev *dev) { return 0; }
void b43_leds_unregister(void *wl) { }
void b43_leds_init(struct b43_wldev *dev) { }
void b43_leds_exit(struct b43_wldev *dev) { }
void b43_leds_stop(struct b43_wldev *dev) { }
void b43_rfkill_poll(struct ieee80211_hw *hw) { }

/*
 * Sospensione e ripresa delle code TX, e lo stato TX: percorso dati, nessuna
 * op sul bring-up. `handle_txstatus` non viene chiamato affatto qui, non
 * essendoci trasmissione.
 */
void b43_dma_tx_suspend(struct b43_wldev *dev) { }
void b43_dma_tx_resume(struct b43_wldev *dev) { }
void b43_pio_tx_suspend(struct b43_wldev *dev) { }
void b43_pio_tx_resume(struct b43_wldev *dev) { }
void b43_dma_handle_txstatus(struct b43_wldev *dev, const void *status) { }
void b43_pio_handle_txstatus(struct b43_wldev *dev, const void *status) { }

/*
 * Il ricarico del beacon: nell'harness delle unit lo fornisce wrap.c, perche'
 * il port lo chiama a ogni tick del watchdog e la traccia del vendor mostra
 * quante volte. Qui e' un no-op: senza mac80211 non c'e' un beacon da
 * ricaricare, e le op che ne conseguono le emette il PHY.
 */
void b43_ac_beacon_reload(struct b43_wldev *dev, unsigned int which) { }

/* --- mac80211: utilita' pure, nessuna emette op ----------------------- */

unsigned int ieee80211_hdrlen(__le16 fc) { return 24; }
u32 ieee80211_channel_to_freq_khz(int chan, enum nl80211_band band)
{
	/* 5 GHz: la formula di mac80211, non un valore inventato. */
	if (band == NL80211_BAND_5GHZ)
		return (5000 + chan * 5) * 1000;
	return 0;
}
struct ieee80211_channel *ieee80211_get_channel_khz(struct wiphy *wiphy,
						    u32 freq)
{
	enum nl80211_band band;
	int i;

	if (!g_hw)
		return NULL;
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *sb = g_hw->wiphy->bands[band];

		if (!sb)
			continue;
		for (i = 0; i < sb->n_channels; i++)
			if (sb->channels[i].center_freq * 1000 == freq)
				return &sb->channels[i];
	}
	return NULL;
}
void ieee80211_rts_get(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       const void *frame, size_t frame_len,
		       const struct ieee80211_tx_info *frame_txctl,
		       struct ieee80211_rts *rts) { }
void ieee80211_ctstoself_get(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif, const void *frame,
			     size_t frame_len,
			     const struct ieee80211_tx_info *frame_txctl,
			     struct ieee80211_cts *cts) { }
__le16 ieee80211_generic_frame_duration(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					enum nl80211_band band,
					size_t frame_len,
					struct ieee80211_rate *rate)
{
	return 0;
}
void ieee80211_get_tkip_p1k_iv(struct ieee80211_key_conf *keyconf,
			       u32 iv32, u16 *p1k) { }
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
		       struct sk_buff *skb, struct napi_struct *napi) { }

/*
 * Le vtable degli altri PHY: a NULL, e non e' pigrizia. b43_phy_allocate()
 * dispaccia su phy->type, quindi su questo hardware prende solo il ramo AC:
 * se un giorno prendesse un altro ramo il puntatore nullo lo fa vedere
 * subito, invece di far girare una vtable finta e produrre una traccia
 * plausibile del PHY sbagliato.
 */
const struct b43_phy_operations *b43_phyops_g;
const struct b43_phy_operations *b43_phyops_n;
const struct b43_phy_operations *b43_phyops_lp;
const struct b43_phy_operations *b43_phyops_ht;

/*
 * I work item si eseguono in linea, non si accodano.
 *
 * Non e' una comodita': b43_bcma_probe() (main.c:5851) chiede il firmware da
 * un work -- INIT_WORK(&wl->firmware_load, b43_request_firmware) piu'
 * schedule_work -- e con un queue_work no-op quel lavoro non gira mai.
 * dev->fw.ucode.data resta nullo e b43_upload_microcode() muore a main.c:2761
 * dereferenziandolo, dentro il bring-up e non nella probe, cioe' lontano
 * dalla causa.
 *
 * Eseguirlo qui e' lo stesso ragionamento dei lock: la suite e' a un thread,
 * e se il lavoro girasse davvero in parallelo la traccia non sarebbe piu'
 * ordinata e il confronto posizionale non avrebbe senso. La differenza
 * rispetto al kernel e' che la' il work parte dopo che la probe e' ritornata,
 * qui parte dentro schedule_work(): se una fase dipendesse da quell'ordine si
 * vedrebbe come op fuori posto, non come un crash.
 */
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	if (work && work->func)
		work->func(work);
	return true;
}
