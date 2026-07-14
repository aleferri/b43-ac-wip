// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 AC-PHY -- RX I/Q calibration SKELETON.
 *
 * PROVENANCE / METODO. Struttura e algoritmo generico portati da brcmsmac
 * (drivers/net/wireless/broadcom/brcm80211/brcmsmac/phy/phy_n.c), funzioni
 * wlc_phy_cal_rxiq_nphy_rev3 / wlc_phy_calc_rx_iq_comp_nphy /
 * wlc_phy_rx_iq_est_nphy. brcmsmac NON ha AC-PHY: quella e' cal N-PHY su radio
 * 2056. Qui se ne prende SOLO la forma -- l'ossatura setup->gainctrl->misura->
 * solve->applica->cleanup e la matematica del solve IQ-imbalance, che e'
 * PHY-independent -- e la si adatta ad AC-PHY rev 1 / radio 2069.
 *
 * COSA E' GENERICO (portato, rivedibile ora):
 *   - l'ordine delle fasi in b43_phy_ac_rxiqcal (== rev3 N-PHY);
 *   - il solve fixed-point in b43_phy_ac_calc_rx_iq_comp (verbatim N-PHY).
 *
 * STATO FILL (dalla trace down-to-bss-up):
 *   - b43_phy_ac_rxiq_est: RIEMPITO e confermato (#82533-82556).
 *   - b43_phy_ac_rxcal_apply_gain / tx_tone / stopplayback: RIEMPITI
 *     verbatim dagli episodi (gain #82514-82531, tono #82499-82512, teardown
 *     #82558-82559). Le op and/or con mask=0 sono euristica (il tracer non
 *     distingue and da or).
 *   - b43_phy_ac_rxcal_gainctrl: struttura hill-climb N-PHY, MA vedi FINDING 3.
 *   - rxcal_phy_setup/radio_setup/cleanup: NON riempiti -- bulk (~300 op RMW,
 *     #82151-82478), da fare a pezzi verificati col correlatore, non a occhio.
 *   - b43_phy_ac_rxiq_coeffs: NON corroborato qui -- vedi FINDING 2.
 *
 * FINDING 1. La cal a bss-up (ch36) e' gain-cal (WI-5): stime -> tabella gain
 * (id 0xc), non coeff a/b.
 * FINDING 2.
 * FINDING 2. il comp AC e' reale, solo
 * non esercitato in questo bss-up. calc_rx_iq_comp resta TEMPLATE.
 * FINDING 3. Il gain di misura e' quasi FISSO tra le iterazioni (setting
 * principale ~6x + alt 2x): la trace NON mostra un hill-climb ampio. La
 * struttura gainctrl portata resta come riferimento N-PHY, ma il runtime reale
 * qui e' "misura ripetuta a gain fisso" -- il criterio di scelta/ripetizione
 * dipende dalle misure UNDEFINED e non e' ricavabile dalla trace.
 *
 * Finche' gli stub non sono riempiti, b43_phy_ac_rxiqcal ritorna
 * -EOPNOTSUPP e non tocca il silicio. NON e' wirato in set_channel: e' Fase B,
 * ha senso solo dopo che la RX base su UNII-1 ch.34 funziona.
 */

#include <linux/kernel.h>	/* int_sqrt */
#include "b43.h"
#include "phy_ac.h"
#include "tables_phy_ac.h"
#include "radio_2069.h"
#include "rxiqcal_phy_ac.h"

/* Estimator per-core: potenze I/Q e prodotto incrociato dal correlatore.
 * (== struct phy_iq_est di brcmsmac.) */
struct b43_phy_ac_iq_est {
	s32 iq_prod;
	u32 i_pwr;
	u32 q_pwr;
};

/* Coefficienti di compensazione RX per-core (== struct nphy_iq_comp). Nomi
 * a0/b0/a1/b1 tenuti per parallelismo con la reference; su AC-PHY 2x2 solo
 * core 0/1 sono usati. */
struct b43_phy_ac_iq_comp {
	s16 a0, b0, a1, b1;
};

/*
 * Soglia minima di potenza I+Q sotto cui la misura non e' affidabile
 * (== NPHY_MIN_RXIQ_PWR). Valore N-PHY tenuto come segnaposto: la scala degli
 * accumulatori AC-PHY va confermata sull'hardware. TODO(trace/hw).
 */
#define B43_PHY_AC_MIN_RXIQ_PWR		0x100
#define B43_PHY_AC_RXIQ_CAL_RETRY	2

/* Gain-cal (percorso (a)): costanti di decisione. Prese da N-PHY
 * gainctrl_rev5 come TEMPLATE -- DA VERIFICARE su AC-PHY. */
#define B43_PHY_AC_RXCAL_THRESH_PWR	10000	/* soglia di potenza */
#define B43_PHY_AC_RXCAL_DESIRED_LOG2	13	/* log2 potenza desiderata */
#define B43_PHY_AC_RXCAL_MAXGAININDEX	7	/* fondo scala ladder */
#define B43_PHY_AC_RXCAL_NUM_SAMPS	1024	/* == 0x400 nella trace */

enum b43_phy_ac_rxcal_dir {
	B43_RXCAL_GAIN_INIT,
	B43_RXCAL_GAIN_UP,
	B43_RXCAL_GAIN_DOWN,
};

/* Flip a 1 quando gli stub hardware sotto sono riempiti dalla trace. */
#define B43_PHY_AC_RXIQCAL_REGMAP_FILLED	0

/*
 * Registri del correlatore RX-IQ, CONFERMATI dalla trace (wl-diag down-to-bss-
 * up, episodio #82533-82556 e altri 7). Sono le write/cmd, quindi catturate
 * verbatim; solo i valori delle read (accumulatori) sono UNDEFINED nel trace,
 * ma li legge il silicio a runtime.
 */
#define B43_PHY_AC_RXIQ_CMD		0x0270	/* bit0 = start, bit1 = iqMode */
#define  B43_PHY_AC_RXIQ_START		0x0001
#define  B43_PHY_AC_RXIQ_IQMODE		0x0002
#define B43_PHY_AC_RXIQ_WAIT		0x0271	/* [7:0] = wait_time */
#define B43_PHY_AC_RXIQ_NSAMP		0x0272	/* num_samps */
/* Accumulatori per core: 3 coppie Hi/Lo a base 0x06c0 + core*0x200. Ordine di
 * lettura nella trace: +3,+2 / +5,+4 / +1,+0. */
#define B43_PHY_AC_RXIQ_ACC(core)	(u16)(0x06c0 + (core) * 0x200)

/* Numero di bit significativi di |value| (== wlc_phy_nbits). */
static int b43_phy_ac_nbits(s32 value)
{
	s32 abs = value < 0 ? -value : value;
	int n = 0;

	while (abs) {
		n++;
		abs >>= 1;
	}
	return n;
}

/* ============================ STUB DA RIEMPIRE ============================ */

/*
 * Correlatore RX-IQ: arma <num_samps> campioni, attende il completamento,
 * legge gli accumulatori i_pwr/q_pwr/iq_prod per core. Reference:
 * wlc_phy_rx_iq_est_nphy. Registri CONFERMATI dalla trace (#82533-82556):
 *   WR 0x0272 = num_samps ; MOD 0x0271[7:0] = wait_time (32) ;
 *   MOD 0x0270 clr iqMode ; MOD 0x0270 set start ; poll RD 0x0270 ;
 *   read accumulatori 0x06c0+core*0x200 (+0x200 per core1: 0x08c0).
 *
 * DA VERIFICARE (SALAME): quale delle 3 coppie e' i_pwr / q_pwr / iq_prod e
 * quale meta' e' Hi vs Lo. Qui si tiene l'ordine di lettura della trace
 * (+3,+2 / +5,+4 / +1,+0) e si assegna per analogia N-PHY (i, q, iq). Sul
 * silicio e' falsificabile: i_pwr,q_pwr >= 0 e grandi, iq_prod piu' piccolo/
 * con segno.
 */
static int b43_phy_ac_rxiq_est(struct b43_wldev *dev,
			       struct b43_phy_ac_iq_est *est,
			       u16 num_samps, u8 wait_time)
{
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
 * Legge (write=0) o scrive (write=1) i 4 coefficienti di comp RX.
 * Reference: wlc_phy_rx_iq_coeffs_nphy (phy reg 0x9a/0x9b/0x9c/0x9d).
 *
 * FINDING (trace): NON corroborato. Gli 8 episodi di stima IQ nel trace
 * (#82533..#84330) NON sono seguiti da scritture di coeff di comp a/b: dopo la
 * stima 0x4000 finale il blob scrive valori di TABELLA GAIN (id 0xc off
 * 0x63/0x73/0x67/0x77 = 0x44) + bias PA. Cioe' su questo board l'estimatore IQ
 * alimenta una cal di GAIN (WI-5), non l'applicazione di coeff di comp N-PHY.
 * Quindi questo accessor resta template: i registri dei coeff AC-PHY, se
 * esistono e vengono usati, non compaiono in questa cattura. Da NON riempire
 * a caso.
 */
static void b43_phy_ac_rxiq_coeffs(struct b43_wldev *dev, u8 write,
				   struct b43_phy_ac_iq_comp *comp)
{
	/* Non corroborato dal trace -- vedi FINDING sopra. */
}

/* phy-side setup del loopback di misura. Ref: wlc_phy_rxcal_physetup_nphy. */
static void b43_phy_ac_rxcal_phy_setup(struct b43_wldev *dev, u8 rx_core)
{
	/* TODO(trace): loopback PHY. Ancora WI-7 seq 82533/83649/84049. */
}

/*
 * Tone-setup pre-rxcal (vendor #39709-#39734, 26 op). PHY-side setup del
 * generatore di tone di calibrazione:
 *
 *   1. peek gate (0x019e) + peek + clr-bit-9 0x040f + peek 0x0394/0x0393
 *   2. Pass 1: per ogni active core, peek + WR di 3 registri
 *      (0x0739+s, 0x073a+s, 0x0725+s) con valori {0x00fa, 0x01d3, 0x07e6}.
 *      Ordine core: 0 → 1 (forward).
 *   3. Pass 2: solo WR (no peek), 6 op con valori "dithered"
 *      {0x007a, 0x01d3, 0x07e2}. Ordine: core-reverse (1 → 0), reg-reverse
 *      (0x0725/0x0925 → 0x073a/0x093a → 0x0739/0x0939).
 *   4. peek 0x0393 + WR 0x0394=0x0110 + WR 0x0393=0x8000 (arm tone gen).
 *
 * Non è per-core (chiamata unica).
 */
void b43_phy_ac_rxcal_tone_setup(struct b43_wldev *dev)
{
	static const u16 reg_off[3] = { 0x0039, 0x003a, 0x0025 }; /* rel a 0x0700 */
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
	/* Nota: peek 0x0393 + WR 0x0394 + WR 0x0393=0x8000 (arm tone) sono
	 * emessi separatamente in b43_phy_ac_rxcal_tone_arm, chiamato una
	 * volta per core con un 0x0394-val leggermente diverso. */
}

/*
 * Arm della generazione tone per calibrazione del core `rx_core`.
 * Emette peek 0x0393 + WR 0x0394 = 0x0110|core + WR 0x0393 = 0x8000.
 * Vendor: #39732-#39734 (core 0) → WR 0x0394 = 0x0110
 *         #39815-#39817 (core 1) → WR 0x0394 = 0x0111
 */
void b43_phy_ac_rxcal_tone_arm(struct b43_wldev *dev, u8 rx_core)
{
	b43_phy_read_log(dev, 0x0393);
	b43_phy_write(dev,    0x0394, (u16)(0x0110 | rx_core));
	b43_phy_write(dev,    0x0393, 0x8000);
}

/*
 * Un singolo step del gainctrl sweep: 4 radio_maskset (12 op tracer) + 8 peek
 * di settling (8 op). Totale 20 op per step. I bit di controllo su radio
 * 0x000e sono {bit1, bit2} — passati come `e_bit1_val` / `e_bit2_val`
 * (già mascherati: 0x0002/0 e 0x0004/0). Il registro 0x016e/0x036e (stride
 * +0x200 per core) è togliato coi bit 0 e 1 in modo idempotente.
 *
 * Gli 8 valori letti da PHY 0x0013 vengono SALVATI in
 * dev->phy.ac->rxcal_imbalance[rx_core][step_idx][0..7]. Sul HW reale
 * questi contengono l'accumulator dopo il settling della config; sul test
 * framework sono UNDEFINED (0 di default) e i peek servono solo al match
 * op-per-op.
 *
 * INTERPRETAZIONE: le 4 combinazioni {bit1, bit2} corrispondono a config
 * distinte del loopback measurement. I peek 0x0013 sono l'accumulator di
 * misura globale (comune ai core). Il valore stabilizzato (probabilmente
 * l'ultimo sample o media degli 8) è quello utile per la formula I/Q comp.
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

/* radio-side setup del loopback. Ref: wlc_phy_rxcal_radio_setup_nphy (2056).
 * Vendor #39641-#39674 (core 0), #39675-#39708 (core 1): 34 op per core:
 *   7 peek iniziali (saved values dei 7 registri: 0x016e, 0x000e, 0x0161,
 *     0x0017, 0x015f, 0x0024, 0x0025 + core stride 0x200)
 *   9 maskset (ognuno espanso in tripletta MOD+RD+WR dal wrap tracer)
 * Totale 34 op = 7 + 9×3. */
void b43_phy_ac_rxcal_radio_setup(struct b43_wldev *dev, u8 rx_core)
{
	u16 s = (u16)(rx_core * 0x200);

	/* 7 peek saved values (#39641-#39647 per core 0) */
	b43_radio_read(dev, 0x016e + s);
	b43_radio_read(dev, 0x000e + s);
	b43_radio_read(dev, 0x0161 + s);
	b43_radio_read(dev, 0x0017 + s);
	b43_radio_read(dev, 0x015f + s);
	b43_radio_read(dev, 0x0024 + s);
	b43_radio_read(dev, 0x0025 + s);

	/* 9 maskset di programma. Ognuno espanso dal wrap in MOD+RD+WR. */
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

/* Definiti piu' sotto, usati dal gainctrl. */
static void b43_phy_ac_tx_tone(struct b43_wldev *dev, u32 freq_hz, u16 amp);
static void b43_phy_ac_stopplayback(struct b43_wldev *dev);

/*
 * Programma il gain di misura sul core <rx_core> (stride +0x200).
 * FINDING (trace, #82514-82531 e ripetuto): il gain e' quasi FISSO tra le
 * iterazioni -- NON un hill-climb ampio. Setting principale usato ~6x, un
 * setting alt (0x0725=0x0600 / 0x0739=0x0000 / 0x073a=0x0180) 2x. Quindi il
 * parametro idx del modello N-PHY non mappa a un ladder ampio: qui si trascrive
 * verbatim il setting principale + il suo micro-settle (0x07e6->0x07e2,
 * 0x00fa->0x007a). Le stesse write vanno su core1 (0x09xx = +0x200).
 * TODO: se serve il setting alt, aggiungere il secondo step e capire il
 * criterio di scelta (non visibile: dipende dalle misure UNDEFINED).
 */
/*
 * RX-IQ cal gainctrl AC-PHY: sweep di 4 config di loopback per il core
 * `rx_core`. Ogni step programma 2 bit di controllo su radio 0x000e+s
 * (via 0x016e+s che pilota il gate del bit) e attende 8 letture di 0x0013
 * (accumulator globale) per il settling.
 *
 * INTERPRETAZIONE (cross-core measurement): un core inietta il tono
 * calibration con una specifica configurazione (bit1, bit2 di 0x000e+s),
 * e l'accumulator letto misura la risposta rx dei core attivi. Le 4
 * combinazioni sono probabilmente:
 *   (1, 0) → config A (baseline injection)
 *   (0, 0) → injection off / null measurement
 *   (1, 1) → config B (secondo mode)
 *   (0, 1) → config C
 * Il ciclo esterno itera su rx_core, così ogni core-i "trasmette" mentre
 * gli altri "ricevono".
 *
 * TODO(formula): la formula che combina i 4×N valori 0x0013 per core in
 * coefficienti I/Q comp non è ancora derivata. I peek nel trace sono
 * val=UNDEFINED (il tracer wl-diag non registra i valori letti), quindi
 * serve HW test o RE del blob per ricostruire la funzione. Il valore da
 * scrivere nei registri di comp finali dovrebbe essere una combinazione
 * lineare dei accumulator misurati alle 4 config (matrix inversion tipica
 * di IQ compensation).
 *
 * Vendor #39735-#39814 (core 0), #39818-#39897 (core 1): 80 op/core.
 */
void b43_phy_ac_rxcal_gainctrl(struct b43_wldev *dev, u8 rx_core)
{
	/* 4 combinazioni {bit1, bit2} di 0x000e+s, ordine emesso dal vendor.
	 * I readings vengono salvati in phy.ac->rxcal_imbalance[core][step]. */
	rxcal_gainctrl_step(dev, rx_core, 0, 0x0002, 0x0000);   /* (1, 0) */
	rxcal_gainctrl_step(dev, rx_core, 1, 0x0000, 0x0000);   /* (0, 0) baseline */
	rxcal_gainctrl_step(dev, rx_core, 2, 0x0002, 0x0004);   /* (1, 1) */
	rxcal_gainctrl_step(dev, rx_core, 3, 0x0000, 0x0004);   /* (0, 1) */
}

/*
 * Inietta il tono di calibrazione (setup loopback). Trascritto verbatim da
 * #82499-82512. NB: le op con "mask=0x0000" nel trace sono phy_reg_and/or e il
 * tracer NON distingue le due; qui l'and/or e' dedotto dal valore (0xff..=
 * clear-mask -> and ; piccolo -> or). EURISTICA, da confermare. La freq/amp del
 * tono e' codificata nei registri sotto (0x0463/0x0461/0x0462), non nei
 * parametri.
 */
static void b43_phy_ac_tx_tone(struct b43_wldev *dev, u32 freq_hz, u16 amp)
{
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

/* Ferma il tono (teardown post-stima). Trascritto da #82558-82559; and/or
 * euristico come sopra. */
static void b43_phy_ac_stopplayback(struct b43_wldev *dev)
{
	b43_phy_set(dev, 0x0460, 0x0002);		/* #82558 or */
	b43_phy_mask(dev, 0x0460, (u16)~0x0004);	/* #82559 and 0xfffb */
}

/* Ripristina phy/radio dopo la misura del core. Ref: rxcal_*cleanup_nphy. */
/*
 * PHY-side cleanup per-core: reset dei 14 registri gain-control ai loro
 * valori "off/idle". Vendor #39899-#39912 (core 0), #39913-#39926 (core 1).
 * Ordine: tutti i core-0 prima, poi tutti i core-1 (chiamato in loop
 * for-core dal caller).
 */
void b43_phy_ac_rxcal_cleanup(struct b43_wldev *dev, u8 rx_core)
{
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
 * Radio-side cleanup per-core: reset dei 7 registri radio toccati da
 * rxcal_radio_setup ai valori "off". Vendor #39927-#39933 (core 0),
 * #39934-#39940 (core 1). Ordine: tutti core-0, poi tutti core-1.
 */
void b43_phy_ac_rxcal_radio_cleanup(struct b43_wldev *dev, u8 rx_core)
{
	static const struct { u16 off; u16 val; } wr[7] = {
		{ 0x016e, 0x0000 }, { 0x000e, 0x0001 }, { 0x0161, 0x0100 },
		{ 0x0017, 0x0011 }, { 0x015f, 0x0000 }, { 0x0024, 0x0003 },
		{ 0x0025, 0x0000 },
	};
	u16 s = (u16)(rx_core * 0x200);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(wr); i++)
		b43_radio_write(dev, wr[i].off + s, wr[i].val);
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
	struct b43_phy_ac_iq_est est[3];
	u16 save_gain[6]; /* 0x0739, 0x073a, 0x0725 × 2 cores */
	u16 save_tone[9]; /* 0x0730, 0x0731, 0x0734 × 3 cores */
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
	for (core = 0; core < 2 && core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		save_gain[core * 3 + 0] = b43_phy_read(dev, 0x0739 + s);
		save_gain[core * 3 + 1] = b43_phy_read(dev, 0x073a + s);
		save_gain[core * 3 + 2] = b43_phy_read(dev, 0x0725 + s);
	}
	for (core = 0; core < 3 && core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		save_tone[core * 3 + 0] = b43_phy_read(dev, 0x0730 + s);
		save_tone[core * 3 + 1] = b43_phy_read(dev, 0x0731 + s);
		save_tone[core * 3 + 2] = b43_phy_read(dev, 0x0734 + s);
	}
	saved_040f = b43_phy_read_log(dev, 0x040f);

	/*
	 * Tone engine init: sequenza vendor scalare #50155-#50200 in ch36
	 * bw20, identica come op-list a #72757-#72802 in ch36 bw40 (i valori
	 * scritti sono gli stessi). Programma il classifier RXIQ e azzera i
	 * coefficienti IQ per-core prima del measurement.
	 *
	 * NON portato qui: le due TBL.WR 0x000c off 0x0060/0x0064 (2 word
	 * ciascuna) che il vendor fa in mezzo a questa sequenza. I valori
	 * SONO channel/bw-dipendenti (bw20 ch36 = {0x0062,0xfffd}/{0x0023,
	 * 0x0003}; bw40 ch36 = {0x0060,0xfffe}/{0x0025,0x0002}; bw20 ch44 =
	 * {0x0061,0xfffe}/{0x0022,0x0002}). Senza la formula del vendor che
	 * li deriva è meglio non scrivere niente che scrivere i valori
	 * sbagliati per il channel/bw corrente.
	 *
	 * Analogamente non portiamo il gain-override massivo (#50201-#50525,
	 * ~230 MODs sui 0x072x-074x per core) né la TBL.WR 0x000e off=0
	 * len=40 (#50652): sono i due grossi blocchi che restano fuori da
	 * questo helper "measure-only".
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
	for (core = 0; core < 3 && core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		b43_phy_write(dev, 0x0730 + s, save_tone[core * 3 + 0]);
		b43_phy_write(dev, 0x0731 + s, save_tone[core * 3 + 1]);
		b43_phy_write(dev, 0x0734 + s, save_tone[core * 3 + 2]);
	}
	/* Restore gain registers. */
	for (core = 0; core < 2 && core < num_cores; core++) {
		u16 s = (u16)(core * 0x200);

		b43_phy_write(dev, 0x0739 + s, save_gain[core * 3 + 0]);
		b43_phy_write(dev, 0x073a + s, save_gain[core * 3 + 1]);
		b43_phy_write(dev, 0x0725 + s, save_gain[core * 3 + 2]);
	}

	/*
	 * Cleanup vendor-style: restore il gate 0x040f, poi CCA pulse per
	 * committare lo stato. Trace ch36 #51726-#51727 fa questo pulse alla
	 * fine del blocco RXIQ, prima che il vendor passi alla fase successiva.
	 * Gli altri restore vendor (tbl 0x000c off 0x0063/0067/0073/0077 al
	 * #51702-51720) sono cleanup del gain-override massivo che non abbiamo
	 * portato — non c'è nulla da ripristinare qui.
	 */
	b43_phy_maskset(dev, 0x040f, (u16)~0x0200, saved_040f & 0x0200);
	b43_phy_set(dev, B43_PHY_AC_BBCFG, B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
	b43_phy_mask(dev, B43_PHY_AC_BBCFG, (u16)~B43_PHY_AC_BBCFG_RSTCCA);
	dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;

	b43dbg(dev->wl, "phy-ac: rxiq_est_debug — done\n");
}

/* ===================== ALGORITMO GENERICO (PORTATO) ====================== */

/*
 * Misura col correlatore e risolve i coefficienti di compensazione IQ.
 * Struttura e matematica prese verbatim da wlc_phy_calc_rx_iq_comp_nphy:
 * dalla stima (ii=I^2, qq=Q^2, iq=I*Q) ricava la coppia (a,b) che azzera lo
 * sbilanciamento di gain/fase. La forma e' PHY-independent; cambiano solo gli
 * accessi (est/coeffs, sopra) e il formato dei coeff.
 *
 * DA VERIFICARE su AC-PHY: (1) mask &0x3ff (10 bit) e ordinamento a/b nei
 * campi -- su N-PHY dipende da phy_rev; (2) la scala di B43_PHY_AC_MIN_RXIQ_PWR.
 */
static int b43_phy_ac_calc_rx_iq_comp(struct b43_wldev *dev, u8 core_mask)
{
	struct b43_phy_ac_iq_est est[3];
	struct b43_phy_ac_iq_comp old_comp, new_comp;
	unsigned int core;
	uint retry = 0;
	int err;

	if (!core_mask)
		return 0;

	b43_phy_ac_rxiq_coeffs(dev, 0, &old_comp);
	new_comp.a0 = new_comp.b0 = new_comp.a1 = new_comp.b1 = 0;
	b43_phy_ac_rxiq_coeffs(dev, 1, &new_comp);

retry_cal:
	err = b43_phy_ac_rxiq_est(dev, est, 0x4000, 32);
	if (err)
		return err;

	new_comp = old_comp;

	for (core = 0; core < dev->phy.ac->num_cores; core++) {
		s32 iq, a, b, temp;
		u32 ii, qq;
		s16 iq_nbits, qq_nbits, brsh, arsh;
		bool bad = false;

		if (!((core_mask >> core) & 1))
			continue;

		iq = est[core].iq_prod;
		ii = est[core].i_pwr;
		qq = est[core].q_pwr;

		if ((ii + qq) < B43_PHY_AC_MIN_RXIQ_PWR) {
			bad = true;
			goto check_bad;
		}

		iq_nbits = b43_phy_ac_nbits(iq);
		qq_nbits = b43_phy_ac_nbits(qq);

		/* a = -(iq / ii), in Q10 (== N-PHY). */
		arsh = 10 - (30 - iq_nbits);
		if (arsh >= 0) {
			a = -(iq << (30 - iq_nbits)) + (ii >> (1 + arsh));
			temp = (s32)(ii >> arsh);
		} else {
			a = -(iq << (30 - iq_nbits)) + (ii << (-1 - arsh));
			temp = (s32)(ii << -arsh);
		}
		if (temp == 0) {
			bad = true;
			goto check_bad;
		}
		a /= temp;

		/* b = sqrt(qq/ii - a^2) - 1.0, in Q10. */
		brsh = qq_nbits - 31 + 20;
		b = (qq << (31 - qq_nbits));
		temp = (brsh >= 0) ? (s32)(ii >> brsh) : (s32)(ii << -brsh);
		if (temp == 0) {
			bad = true;
			goto check_bad;
		}
		b /= temp;
		b -= a * a;
		b = (s32)int_sqrt((unsigned long)b);
		b -= (1 << 10);

		/* TODO(hw): mask/ordinamento da confermare su AC-PHY. */
		if (core == 0) {
			new_comp.a0 = (s16)a & 0x3ff;
			new_comp.b0 = (s16)b & 0x3ff;
		} else if (core == 1) {
			new_comp.a1 = (s16)a & 0x3ff;
			new_comp.b1 = (s16)b & 0x3ff;
		}

check_bad:
		if (bad) {
			if (retry < B43_PHY_AC_RXIQ_CAL_RETRY) {
				retry++;
				goto retry_cal;
			}
			new_comp = old_comp;	/* rinuncia: tieni i vecchi */
			break;
		}
	}

	b43_phy_ac_rxiq_coeffs(dev, 1, &new_comp);
	return 0;
}

/* ======================= ORCHESTRATORE (STRUTTURA) ======================= */

/*
 * cal_type: 0/2 = RX-IQ, 1/2 = RC-cal LPF (qui non trattato: e' un percorso
 * separato, vedi rev3). Ossatura == wlc_phy_cal_rxiq_nphy_rev3:
 *   quiesce -> salva banco gain -> per core {phy/radio setup; se IQ: gainctrl,
 *   tono, misura+solve, stop; cleanup; RESET2RX} -> ripristina.
 */
int b43_phy_ac_rxiqcal(struct b43_wldev *dev, u8 cal_type)
{
	u16 orig_bbcfg;
	u16 gain_save[3];
	unsigned int rx_core;
	u8 coremask = dev->phy.ac->coremask;

	/* Non riempito: nessun accesso al silicio finche' gli stub sono vuoti. */
	if (!B43_PHY_AC_RXIQCAL_REGMAP_FILLED)
		return -EOPNOTSUPP;

	/* Quiesce PHY (== mod 0x01[15]=0 + stay_in_carriersearch). */
	orig_bbcfg = b43_phy_read_log(dev, B43_PHY_AC_BBCFG);
	b43_phy_mask(dev, B43_PHY_AC_BBCFG, (u16)~0x8000);
	dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_PHY_RUN;

	/* Salva il banco gain RF-seq (== tbl RFSEQ off 0x110 su N-PHY).
	 * FILL(trace): id tabella + offset del banco gain su AC-PHY. */
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
			b43_phy_ac_calc_rx_iq_comp(dev, (u8)(1 << rx_core));
			b43_phy_ac_stopplayback(dev);
		}

		/* cal_type 1/2 (RC-cal LPF): percorso separato, non portato. */

		b43_phy_ac_rxcal_cleanup(dev, rx_core);
		b43_phy_ac_force_rf_sequence(dev, B43_PHY_AC_RF_SEQ_RST2RX,
					     B43_PHY_AC_RF_SEQ_OVERRIDE_GATE);
	}

	/* Ripristina: banco gain, BBCFG, CCA, RESET2RX finale. */
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

	b43dbg(dev->wl, "phy-ac: rxiqcal skeleton (cal_type %u) -- regmap non riempito\n",
	       (unsigned int)cal_type);
	return 0;
}
