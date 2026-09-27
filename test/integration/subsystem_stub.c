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
#include <linux/etherdevice.h>
#include <linux/jiffies.h>
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

	/* Il dovere radar, come lo marca cfg80211: U-NII-2A e U-NII-2C. */
	for (i = 0; i < sb->n_channels; i++) {
		u32 f = sb->channels[i].center_freq;

		if ((f >= 5260 && f <= 5320) || (f >= 5500 && f <= 5720))
			sb->channels[i].flags |= IEEE80211_CHAN_RADAR;
	}
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

/*
 * I delayed work sono i timer del driver: il periodic work, il poll del
 * radar. Si tengono qui con la loro scadenza in jiffies, e li fa scattare
 * b43_test_run_until() quando main.c porta avanti l'orologio. Un work gia'
 * accodato non si riaccoda, come nel kernel.
 */
#define SHIM_DWORKS	8

static struct {
	struct delayed_work *work;
	unsigned long due;
} dworks[SHIM_DWORKS];

void ieee80211_queue_delayed_work(struct ieee80211_hw *hw,
				  struct delayed_work *work,
				  unsigned long delay)
{
	int i, free = -1;

	for (i = 0; i < SHIM_DWORKS; i++) {
		if (dworks[i].work == work)
			return;
		if (!dworks[i].work && free < 0)
			free = i;
	}
	if (free < 0) {
		b43_trace_note("troppi delayed work in coda%d\n", 0);
		return;
	}
	dworks[free].work = work;
	dworks[free].due = jiffies + delay;
}

int b43_test_dwork_cancel(void *w)
{
	int i;

	for (i = 0; i < SHIM_DWORKS; i++) {
		if (dworks[i].work == w) {
			dworks[i].work = NULL;
			return 1;
		}
	}
	return 0;
}

/* Fa scattare, in ordine di scadenza, i work che scadono entro @until. */
void b43_test_run_until(unsigned long until)
{
	for (;;) {
		struct delayed_work *w;
		int i, next = -1;

		for (i = 0; i < SHIM_DWORKS; i++)
			if (dworks[i].work && time_before_eq(dworks[i].due, until) &&
			    (next < 0 || time_before(dworks[i].due, dworks[next].due)))
				next = i;
		if (next < 0)
			break;
		w = dworks[next].work;
		if (time_after(dworks[next].due, jiffies))
			jiffies = dworks[next].due;
		dworks[next].work = NULL;
		w->work.func(&w->work);
	}
	if (time_after(until, jiffies))
		jiffies = until;
}

/*
 * Il beacon che mac80211 darebbe a b43_update_templates(): intestazione,
 * SSID, rate e TIM, cioe' quel che b43_write_beacon_template() legge per
 * TIMBPOS e DTIMPER. La lunghezza dell'SSID e' B43_SSID_LEN, come AC_SSID_LEN
 * di ../unit, perche' entra nella lunghezza del template.
 */
static struct sk_buff shim_beacon_skb;
static u8 shim_beacon[256];

struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw,
					 struct ieee80211_vif *vif,
					 u16 *tim_offset, u16 *tim_length,
					 unsigned int link_id)
{
	static const u8 rates[] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
	struct ieee80211_mgmt *m = (struct ieee80211_mgmt *)shim_beacon;
	long ssid_len = b43_test_env_long("B43_SSID_LEN", 7);
	struct ieee80211_tx_info *info;
	u8 *p;

	memset(shim_beacon, 0, sizeof(shim_beacon));
	m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
				       IEEE80211_STYPE_BEACON);
	eth_broadcast_addr(m->da);
	memcpy(m->sa, vif->addr, ETH_ALEN);
	memcpy(m->bssid, vif->addr, ETH_ALEN);
	m->u.beacon.beacon_int = cpu_to_le16(vif->bss_conf.beacon_int);
	m->u.beacon.capab_info = cpu_to_le16(WLAN_CAPABILITY_ESS);
	p = m->u.beacon.variable;
	*p++ = WLAN_EID_SSID;
	*p++ = (u8)ssid_len;
	memset(p, 'X', ssid_len);
	p += ssid_len;
	*p++ = WLAN_EID_SUPP_RATES;
	*p++ = sizeof(rates);
	memcpy(p, rates, sizeof(rates));
	p += sizeof(rates);
	*p++ = WLAN_EID_TIM;
	*p++ = 4;
	*p++ = 0;				/* DTIM count */
	*p++ = vif->bss_conf.dtim_period;
	*p++ = 0;
	*p++ = 0;

	memset(&shim_beacon_skb, 0, sizeof(shim_beacon_skb));
	shim_beacon_skb.data = shim_beacon;
	shim_beacon_skb.len = p - shim_beacon;
	info = IEEE80211_SKB_CB(&shim_beacon_skb);
	info->band = NL80211_BAND_5GHZ;
	info->control.rates[0].idx = 0;
	return &shim_beacon_skb;
}

void ieee80211_radar_detected(struct ieee80211_hw *hw)
{
	b43_trace_note("ieee80211_radar_detected()%.0d\n", 0);
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

/*
 * leds.c e rfkill.c si compilano: sotto di loro stanno il LED core e i
 * trigger di mac80211, che qui non ci sono. La registrazione riesce e i
 * trigger hanno un nome, perche' con un nome nullo b43_register_led()
 * ritorna -EINVAL e il percorso dei LED -- il mask per b43_gpio_init() e le
 * scritture di GPIO_CONTROL in b43_leds_init() -- non gira affatto. Il LED
 * core non richiama mai brightness_set: nessun trigger scatta senza traffico.
 */
int led_classdev_register_ext(struct device *parent,
			      struct led_classdev *led_cdev,
			      struct led_init_data *init_data) { return 0; }
void led_classdev_unregister(struct led_classdev *led_cdev) { }
const char *__ieee80211_get_tx_led_name(struct ieee80211_hw *hw) { return "tx"; }
const char *__ieee80211_get_rx_led_name(struct ieee80211_hw *hw) { return "rx"; }
const char *__ieee80211_get_assoc_led_name(struct ieee80211_hw *hw) { return "assoc"; }
const char *__ieee80211_get_radio_led_name(struct ieee80211_hw *hw) { return "radio"; }
void wiphy_rfkill_set_hw_state_reason(struct wiphy *wiphy, bool blocked,
				      enum rfkill_hard_block_reasons reason) { }

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
