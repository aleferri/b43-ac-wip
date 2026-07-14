/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_PHY_AC_H_
#define B43_PHY_AC_H_

#include "phy_common.h"

struct ieee80211_channel;


/* PHY register offsets, relative to PHY MMIO space. */

#define B43_PHY_AC_BBCFG			0x001
#define  B43_PHY_AC_BBCFG_RSTCCA		0x4000	/* Reset CCA */
#define B43_PHY_AC_BANDCTL			0x003	/* Band control */
#define  B43_PHY_AC_BANDCTL_5GHZ		0x0001
#define B43_PHY_AC_TABLE_ID			0x00d
#define B43_PHY_AC_TABLE_OFFSET			0x00e
#define B43_PHY_AC_TABLE_DATA1			0x00f
#define B43_PHY_AC_TABLE_DATA2			0x010
#define B43_PHY_AC_TABLE_DATA3			0x011

/* PHY-table access registers, used in pairs for bulk writes. */
#define B43_PHY_AC_TABLE_DATA_LO		0x00f
#define B43_PHY_AC_TABLE_DATA_HI		0x010
#define B43_PHY_AC_TABLE_DATA_2		0x011
#define B43_PHY_AC_CLASSCTL			0x140	/* Classifier control */
#define  B43_PHY_AC_CLASSCTL_CCKEN		0x0001	/* CCK enable */
#define  B43_PHY_AC_CLASSCTL_OFDMEN		0x0002	/* OFDM enable */
#define  B43_PHY_AC_CLASSCTL_WAITEDEN		0x0004	/* Waited enable */


/* RF Control & RF Sequencer. */
#define B43_PHY_AC_RFCTL1			0x400	/* override save/restore + OR 0x3 */
#define B43_PHY_AC_RF_SEQ_MODE			0x401
#define B43_PHY_AC_RF_SEQ_TRIG			0x402
#define B43_PHY_AC_RF_SEQ_STATUS		0x403

/*
 * Trigger values for RF_SEQ_TRIG (and corresponding bits read from
 * RF_SEQ_STATUS).
 */
#define  B43_PHY_AC_RF_SEQ_RST2RX		0x0020	/* force_rfseq cmd 2 */
/* force_rfseq cmd->bit: 0=0x01 1=0x02 2=0x20(RST2RX) 3=0x04 4=0x08 5=0x10 */
/* PHY resampler/BW per-canale 0x371-0x376: valori da chan_tuning u16[52..57]
 * (campo phy_bw[]), NON i valori radio chan_raw6. */
#define B43_PHY_AC_BW1A				0x371
#define B43_PHY_AC_BW2				0x372
#define B43_PHY_AC_BW3				0x373
#define B43_PHY_AC_BW4				0x374
#define B43_PHY_AC_BW5				0x375
#define B43_PHY_AC_BW6				0x376
#define B43_PHY_AC_RFCTL_CMD			0x408

/* Analog Front End (AFE), per RF core. */
#define B43_PHY_AC_AFE_C1			0x725
#define B43_PHY_AC_AFE_C1_OVER			0x739
#define B43_PHY_AC_AFE_C2			0x925	/* by stride from C1 */
#define B43_PHY_AC_AFE_C2_OVER			0x939

#define B43_PHY_AC_C1_CLIP			0x6d4
#define  B43_PHY_AC_C1_CLIP_DIS			0x4000
#define B43_PHY_AC_C2_CLIP			0x8d4
#define  B43_PHY_AC_C2_CLIP_DIS			0x4000
#define B43_PHY_AC_C3_CLIP			0xad4
#define  B43_PHY_AC_C3_CLIP_DIS			0x4000

/* PHY-table-write gate. */
#define B43_PHY_AC_REG_TBL_WRITE_GATE		0x19E
#define  B43_PHY_AC_TBL_WRITE_GATE_LOCK		0x0002
#define  B43_PHY_AC_RF_SEQ_OVERRIDE_GATE	0x0001

/*
 * Software mirror of hardware enable/gate bits that gate correctness of
 * downstream PHY sequences. Every mutation of one of the tracked HW
 * registers must update status_mask in the same code path so the mirror
 * stays in sync. Precondition checks read this field via
 * B43_PHY_AC_REQUIRE() below; a failed check sets STATE_FAULTED sticky
 * and every subsequent tracked entry point bails immediately, keeping
 * the driver from touching HW in an inconsistent state.
 *
 * Bit assignments mirror the raw HW bits tracked by
 * reverse-tools/annotate_enables.py, so the trace annotator and the
 * driver agree on the state model.
 */
#define B43_PHY_AC_STATE_MAC_EN		0x0001	/* MAC.MCTRL bit 0: MAC running */
#define B43_PHY_AC_STATE_RX_CCK		0x0002	/* CLASSCTL bit 0: CCK classifier on */
#define B43_PHY_AC_STATE_RX_OFDM	0x0004	/* CLASSCTL bit 1: OFDM classifier on */
#define B43_PHY_AC_STATE_RX_WAITED	0x0008	/* CLASSCTL bit 2: WAITED classifier on */
#define B43_PHY_AC_STATE_RX_ANY		(B43_PHY_AC_STATE_RX_CCK  | \
					 B43_PHY_AC_STATE_RX_OFDM | \
					 B43_PHY_AC_STATE_RX_WAITED)
#define B43_PHY_AC_STATE_CLIP_C0_DIS	0x0010	/* PHY 0x6d4 bit 0x4000 */
#define B43_PHY_AC_STATE_CLIP_C1_DIS	0x0020	/* PHY 0x8d4 bit 0x4000 */
#define B43_PHY_AC_STATE_CLIP_C2_DIS	0x0040	/* PHY 0xad4 bit 0x4000 */
#define B43_PHY_AC_STATE_CLIP_ALL_DIS	(B43_PHY_AC_STATE_CLIP_C0_DIS | \
					 B43_PHY_AC_STATE_CLIP_C1_DIS | \
					 B43_PHY_AC_STATE_CLIP_C2_DIS)
#define B43_PHY_AC_STATE_PHY_RUN	0x0080	/* BBCFG bit 0x8000: 1=running, 0=quiesced */
#define B43_PHY_AC_STATE_CCA_RESET	0x0100	/* BBCFG bit 0x4000 (RSTCCA active) */
#define B43_PHY_AC_STATE_FAULTED	0x8000	/* sticky: a precondition failed */

/* Per-device PHY state. */

struct b43_phy_ac {
	/* active RF-chain count (PHY reg 0x0B & 0x07), set at op_init */
	u8 num_cores;
	/* populated-chain bitmask (1 bit per present core), set at op_init */
	u8 coremask;
	/* Analog LPF / DAC-buffer caps; attach defaults, set in op_allocate. */
	u8 lpf_cap0;	/* default 0x80 */
	u8 lpf_cap1;	/* default 0x80 */
	u8 dacbuf_cap;	/* default 0x0c */
	/* Software mirror of tracked HW gate bits; see B43_PHY_AC_STATE_*. */
	u16 status_mask;
	/*
	 * RX-IQ imbalance accumulator readings dal probe sweep in
	 * b43_phy_ac_rxcal_gainctrl. Indicizzato [core][step_idx][sample]:
	 *   step_idx = ordine vendor delle 4 combinazioni {bit1, bit2}
	 *     di radio 0x000e+s:
	 *       [0]: (bit1=1, bit2=0)
	 *       [1]: (bit1=0, bit2=0)  — baseline (injection off)
	 *       [2]: (bit1=1, bit2=1)
	 *       [3]: (bit1=0, bit2=1)
	 *   sample 0..7 = le 8 letture consecutive di PHY 0x0013
	 *     (accumulator globale) usate dal vendor per settling.
	 *
	 * Uso previsto: calcolo dei coefficienti I/Q compensation
	 * (formula ancora TBD — vedi TODO in rxcal_gainctrl). Non usato
	 * per il match op-per-op, che valuta solo le op emesse; sul HW
	 * reale questi sono i valori "trovati" durante la cal e vanno
	 * consumati dalla fase successiva (rxiq comp write).
	 */
	u16 rxcal_imbalance[3][4][8];
};

/*
 * Precondition check. `want` bits MUST be set, `forbid` bits MUST be
 * clear. On mismatch: log an error, set FAULTED, and return. Once
 * FAULTED is set every subsequent REQUIRE returns immediately, so a
 * single broken invariant does not cascade into HW damage.
 *
 * `dev` is evaluated more than once (must be a plain lvalue).
 * `want` and `forbid` must be disjoint or the check is unsatisfiable;
 * a zero `want`/`forbid` means "no requirement on that side".
 *
 * REQUIRE()	   -> use in functions returning void
 * REQUIRE_RET()   -> use in functions returning a value
 *
 * NOTE on STATE_PHY_RUN: this bit is mirrored by the rxiqcal
 * quiesce/restore path but is NOT settable at driver bring-up (the
 * BBCFG[15] transition to 1 happens as part of the vendor init sequence,
 * which the current driver does not route through a tracked mutator).
 * Do not use STATE_PHY_RUN in `want`/`forbid` until a real mutator exists.
 */
#define B43_PHY_AC_REQUIRE(dev, want, forbid) do {			\
	struct b43_phy_ac *__ac = (dev)->phy.ac;			\
	u16 __sm = __ac->status_mask;					\
	if (__sm & B43_PHY_AC_STATE_FAULTED)				\
		return;							\
	if ((__sm & (want)) != (want) || (__sm & (forbid))) {		\
		b43err((dev)->wl,					\
		       "phy_ac: %s precondition failed: "		\
		       "status=0x%04x want=0x%04x forbid=0x%04x\n",	\
		       __func__, __sm, (u16)(want), (u16)(forbid));	\
		__ac->status_mask |= B43_PHY_AC_STATE_FAULTED;		\
		return;							\
	}								\
} while (0)

#define B43_PHY_AC_REQUIRE_RET(dev, want, forbid, ret) do {		\
	struct b43_phy_ac *__ac = (dev)->phy.ac;			\
	u16 __sm = __ac->status_mask;					\
	if (__sm & B43_PHY_AC_STATE_FAULTED)				\
		return (ret);						\
	if ((__sm & (want)) != (want) || (__sm & (forbid))) {		\
		b43err((dev)->wl,					\
		       "phy_ac: %s precondition failed: "		\
		       "status=0x%04x want=0x%04x forbid=0x%04x\n",	\
		       __func__, __sm, (u16)(want), (u16)(forbid));	\
		__ac->status_mask |= B43_PHY_AC_STATE_FAULTED;		\
		return (ret);						\
	}								\
} while (0)

extern const struct b43_phy_operations b43_phyops_ac;


bool b43_phy_ac_force_rf_sequence(struct b43_wldev *dev, u16 rf_seq, u16 gate);
u16  b43_phy_ac_classifier(struct b43_wldev *dev, u16 mask, u16 val);

/*
 * The wl-diag trace records the address of every register read but not the
 * value returned (the tracer only captures input args). So each read below is
 * a hole in the trace. Route value-consuming reads through these to capture
 * the real value on hardware; the log is compiled out when B43_DEBUG is 0
 * (dead branch elided), so release builds pay nothing. `dev` must be a plain
 * lvalue -- it is evaluated more than once.
 */
#define b43_phy_read_log(dev, reg) ({					\
	u16 __r = (reg), __v = b43_phy_read((dev), __r);		\
	if (B43_DEBUG)							\
		b43dbg((dev)->wl, "phy   rd 0x%04x = 0x%04x\n",		\
		       __r, __v);					\
	__v;								\
})
#define b43_radio_read_log(dev, reg) ({					\
	u16 __r = (reg), __v = b43_radio_read((dev), __r);		\
	if (B43_DEBUG)							\
		b43dbg((dev)->wl, "radio rd 0x%04x = 0x%04x\n",		\
		       __r, __v);					\
	__v;								\
})

void b43_phy_ac_reset_cca(struct b43_wldev *dev);

/*
 * RX-cal iterazioni successive alla prima (iter 1 è dentro set_channel).
 *
 * post_cal_finalize (iter 2): chiamata a MAC UP dopo op_switch_channel.
 *   A1 restore + idle_tssi_meas con base_indices { 0x0207, 0x0200 }.
 *
 * post_cal_finalize_iter3 (iter 3): chiamata dopo iter 2. Inizia con
 *   mac_suspend + 4 op preambulo diagnostiche + A1 + idle_tssi_meas con
 *   base_indices { 0x0206, 0x0200 } (identici a iter 1).
 *
 * TODO(rename): unificare come `rxcal_iter(dev, i)` quando la semantica
 * delle iterazioni successive (iter 4+) sarà chiara.
 */
void b43_phy_ac_post_cal_finalize(struct b43_wldev *dev);
void b43_phy_ac_post_cal_finalize_iter3(struct b43_wldev *dev);

/*
 * RX-IQ compensation apply (vendor #41135+): quarta iterazione del ciclo cal,
 * strutturalmente diversa dalle iter idle-TSSI (1/2/3). NON scrive base index
 * su 0x0645, ma:
 *   - Rilegge la compensation table 0x0020 (readback 32-bit per-core)
 *   - Disabilita il path RX-IQ per-core (0x?78 bit 0)
 *   - Riesegue A1 + radio commit con parametri "coeff apply" (diverso da A3)
 *   - Body-loop iter 4 legge gain regs e scrive coefficienti (consumatore di
 *     rxcal_imbalance[core][step][sample])
 *
 * TODO: formula di scrittura coefficienti ancora aperta (analogo N-PHY:
 * wlc_phy_calc_rx_iq_comp_acphy). Per ora i valori dei coefficienti sono
 * hardcoded dal trace vendor d6220 ch36; su altri canali o boards
 * differiranno.
 */
void b43_phy_ac_rxiqcal_apply(struct b43_wldev *dev);

/*
 * Post-rxiqcal stage 2 (vendor #41500+, fase B4). Sequenza che segue la
 * conclusione di B3c e che il vendor emette con gate 019e in "auto-contained"
 * mode (ogni TBL.WR fa lock+write+unlock nel proprio scope):
 *   B4 preamble (3 op): MOD 019e lock (idempotente) + MOD 019e unlock +
 *                       WR 0x0382 = 0x8a09
 *   B4a (87 op): bulk clear di 12 slot della TBL 0x000c range 0x40-0x55,
 *                organizzato in 3 gruppi per-core (base 0x40/0x48/0x50) con
 *                offset base(len=2)/+3(len=1)/+4(len=1)/+5(len=1).
 * Sotto-blocchi B4b/B4c/B4d successivi ancora da implementare.
 *
 * TODO(nome-def): il nome "stage2" è provvisorio; da rivedere quando la
 * struttura complessiva delle fasi post-rxiqcal fino a mac_enable sarà chiara.
 */
void b43_phy_ac_post_rxiqcal_stage2(struct b43_wldev *dev);

/*
 * RX AFE calibration (vendor #41892+, fase B5). Esegue una sequenza di 18
 * iter (6 comandi × 3 core) di HW cal via reg 0x0380 (bit 15 = busy).
 * Ogni iter: [pre-clears TBL 0x000c] → armare cmd → poll HW → readback
 * RAD.RD 0x0144+core*0x200 + TBL.RD 0x000c off=0x8X+group*7 → write-back
 * TBL.WR 0x000c off=0x4X+group*8.
 *
 * Iter 6/12 hanno un tail speciale: iter 6 emette un "fast batch commit" di
 * 36 TBL.WR (aggiorna gain override core 0/1), iter 12 emette 2 TBL.WR
 * pre-clears per iter 13. Iter 18 tail e gruppo 4 (iter 19-24) pending.
 *
 * Il chiamante DEVE programmare `b43_test_plan_phy_reads(0x0380, ...)`
 * con la sequenza di valori attesi prima di invocare questa funzione.
 *
 * TODO(formula): valori scritti nei TBL.WR post-poll (0x45, 0x44, ecc.)
 * derivati dai readback (0x0144, TBL.RD 0x8X). Formula runtime pending —
 * al momento hardcoded dal trace d6220 ch36.
 */
void b43_phy_ac_rxcal_afe_calibrate(struct b43_wldev *dev);

/*
 * RX AFE cal finalize gain LUTs (vendor #43271-#45962, fase C). Riempie 3
 * grandi tabelle (id=0x42/0x62/0x82) 128 slot per core con valori uniformi
 * derivati dai commit iter di B5:
 *   TBL 0x42 (core 0) → 0xfe02 (coincide con iter 6 result)
 *   TBL 0x62 (core 1) → 0x0100 (coincide con iter 12 result)
 *   TBL 0x82 (core 2) → 0xff60 per off < 0x21, poi 0xfd56 (transizione a
 *                       0x21, significato preciso da approfondire)
 *
 * Preamble (3 op): MOD 0x0001 pulse bit 14 (set+clr) + WR 0x0382 = 0.
 * Body: 128 × 3 TBL.WR interleaved (core 0 → core 1 → core 2 per ogni off).
 *
 * TODO(formula): il valore per core 2 dovrebbe derivare dai risultati iter
 * 6/12/18 di quel core (iter 18 core 2 result = 0x0764) ma la relazione
 * esatta non è chiara (0xff60/0xfd56 non hanno derivazione ovvia).
 */
void b43_phy_ac_rxcal_afe_finalize_gain_luts(struct b43_wldev *dev);

/*
 * TX-power by index (vendor pattern osservato a #38132, #41420, #45964,
 * #46369, #50019, #50539, ..., emesso 11 volte nel trace d6220 ch36).
 * Emette 59 op:
 *   Preamble (2): peek 0x019e + MOD lock idempotente
 *   Per core attivo:
 *     [bridge tra core: 1 op MOD lock — solo dal 2° in poi]
 *     Batch A (15): 3× fast TBL.WR TBL 0x0007 off 0x0100+c/0x0103+c/0x0106+c
 *                    (gain code g0/g1/g2 derivati da idx)
 *     Sync (2): peek 0x019e + MOD lock idempotente
 *     Batch B (10): 2× fast TBL.WR TBL 0x000c off bbmult_lo[c]/bbmult_hi[c]
 *                    (bbmult per-antenna derivato da idx)
 *   Postamble (2): MOD lock idempotente + MOD unlock
 *
 * NB: alcune varianti del vendor (es. #38132) aggiungono side-effect prima
 * o dopo (restore 0x0070 bit 15:13, WR 0x1641) — sono responsabilità del
 * chiamante.
 *
 * TXPWR_INDEX default = 0x40 (open-loop fixed, verificato op-per-op contro
 * vendor d6220 ch36).
 */
#define B43_PHY_AC_TXPWR_INDEX_DEFAULT	0x40
void b43_phy_ac_txpwr_by_index(struct b43_wldev *dev, u8 idx);

/*
 * RX gain block defaults + commit pulse (vendor #46021-#46056, 36 op).
 * Chiamato immediatamente dopo b43_phy_ac_txpwr_by_index (nel flow d6220
 * a #45962-#46056).
 *
 * Struttura:
 *   Block 2 (32 op): 16 WR PHY per core × 2 core sui gain regs
 *     0x0720-0x073e (stride +0x200 per core 1). Valori identici tra core.
 *   Block 3 preamble (4 op): WR 0x019e=0x03d0 + WR 0x040f=0x09ff +
 *     MOD 0x0001 pulse bit 14 (trigger commit).
 */
void b43_phy_ac_rxgain_defaults_pulse(struct b43_wldev *dev);

/*
 * Radio chain range setup (vendor #46057-#46169, fase D block 3 body, 113 op).
 * Setup misto RX+TX chain (era erroneamente chiamata "rxgain_radio_ranging"):
 * scrive un set base di regs RAD (0x001a-0x0170 per core 0, mirror stride
 * +0x200 per core 1), abilita 4 gain-path bits via MOD 0x02ed/f1/f5/f9,
 * tocca il registro `phy_bw` 0x?d4 (RX+TX chain bandwidth, evidenza in
 * radio_2069.c) e il registro 0x0140 (BW/RX-gate/ADC/clip_det, mixed),
 * poi setta RX-IQ accumulator via 0x0339 e RX-IQ path disable via 0x?78.
 * Termina con readback + tuning per-core.
 *
 * SALAME: la semantica esatta di 0x02ed/f1/f5/f9 (bit 4) e del pulse
 * finale su 0x0001 bit 14 non è identificata.
 */
void b43_phy_ac_radio_chain_range_setup(struct b43_wldev *dev, bool with_tune);

/*
 * RX gain per-channel config (vendor #46170-#46351, 181 op). Configura in
 * modo elaborato il gain block PHY per ciascun core:
 *   - Global preamble (2 op): peek 0x040f + MOD 0x040f clr bit 9.
 *   - Per-core (79 op × 2 core): peek+WR 0x?73e + 6 MOD 0x?73e (bit config)
 *     + 15 peek readback (0x?720-0x?73c range) + 54 MOD/WR di config
 *     bit-field su gain regs (0x?720-0x?73c).
 *   - Bridge (3 op): 3 MOD 0x019e set bit 6/7/8 (unico registro, 3 op
 *     invece di un WR unico — pattern osservato).
 *   - Tail (18 op): 3 peek+WR paia per-core (0x?739=0x00fa, 0x?73a=0x01d3,
 *     0x?725=0x07e6) + 6 WR standalone finali con valori rimappati
 *     (0x0925=0x07e2 ecc.) che sovrascrivono ciò appena scritto.
 *
 * TODO: 3-core hardcoded per bridge (0x019e set bits 6/7/8), 2-core
 * hardcoded per il resto. Formula runtime dei valori pending.
 */
void b43_phy_ac_rxgain_perchan_config(struct b43_wldev *dev);

/*
 * RX-IQ apply TX gain + bbmult (vendor #46352-#46450, fase E block 1, 99 op).
 * Scrive compensazione RXIQ nel path TX-side: aggiorna il TX gain code
 * (TBL 0x0007 offsets 0x100+c/0x103+c/0x106+c — la stessa area scritta da
 * txpwr_by_index, cioè TX gain LUT) e il baseband multiplier bbmult
 * (TBL 0x000c offsets 0x63/0x67 stride +4, TX-side). I nuovi valori sono
 * i coefficienti derivati dalla misura RXIQ (per compensare l'imbalance).
 *
 * Struttura op-count-verified:
 *   Preamble (2): peek 0x019e + MOD lock
 *   Per core (47 op):
 *     Batch A (15): 3× fast TBL.RD id=0x0007 (readback pre-compensation)
 *     Batch B (15): 3× fast TBL.WR stessi offset con nuovi val
 *                    (g0=0, g1=comp_g1[c], g2=0x00f3)
 *     Batch C (7):  1× TBL.RD id=0x000c (readback bbmult) + sync explicit
 *     Batch D (10): 2× fast TBL.WR id=0x000c con bbmult compensato
 *   Bridge (1): MOD lock idempotente
 *   Postamble (2): MOD lock + MOD unlock
 *
 * TODO: valori comp_g1[]/comp_bbmult[] hardcoded dal trace; devono
 * derivare runtime dai risultati RXIQ estimation.
 */
void b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(struct b43_wldev *dev);

/*
 * RX-IQ DDS/NCO seed (vendor #46451-#46561, fase E block 2, 111 op).
 * Arma un comando su 0x0382, azzera 3 zone da 2 slot in TBL 0x000c, poi
 * carica una sequenza di 40 u32 in TBL 0x000e (probabilmente DDS/NCO
 * per il prossimo tone-sweep RXIQ).
 *
 * Struttura op-count-verified:
 *   1 op:  WR 0x0382 = 0x8a09 (arm command)
 *   8 op:  TBL.WR id=0x000c off=0x40 len=2 [0,0] (auto-contained)
 *   8 op:  TBL.WR id=0x000c off=0x48 len=2 [0,0]
 *   8 op:  TBL.WR id=0x000c off=0x50 len=2 [0,0]
 *   86 op: TBL.WR id=0x000e off=0x00 len=40 u32 auto-contained
 *          (5 preamble + 80 body + 1 postamble)
 *
 * La sequenza 40 u32 ha simmetria periodica 20+20 (metà1=metà2),
 * consistent con DDS/NCO period 20 sample (stesso pattern di B4b).
 *
 * TODO: Semantica di WR 0x0382 = 0x8a09 non identificata (arm/kick
 * di un correlatore RXIQ). Valori DDS hardcoded dal trace.
 */
void b43_phy_ac_rxiqcal_dds_seed(struct b43_wldev *dev);

/*
 * RX-IQ prep second iteration (vendor #46562-#46777, fase E block 3, 216 op).
 * Prepara le condizioni per la seconda tornata di RXIQ measurement:
 *   Seg A (14): 2× TBL.RD id=0x000c auto-contained + MOD unlock esplicito
 *               (readback bbmult per-core post-compensation).
 *   Seg B (19): kick sequence per il correlatore RXIQ — 13 op standalone
 *               su 0x0471/0x0463/0x0461/0x0462/0x0400/0x0460/0x0382/0x0403
 *               + 6 MOD per-core (3-core hardcoded stride +0x200):
 *               MOD 0x?73a clr bit 8 + MOD 0x?725 set bit 10.
 *   Seg C (183): preamble (2) + 36× fast TBL.WR id=0x000c interleaved
 *                offset (i, i+0x20) per i=0..17 + postamble MOD unlock (1).
 *                LUT di 36 valori hardcoded dal trace (prime 7 pair uguali,
 *                dalle 8-esima in poi divergono).
 *
 * TODO: significato preciso della LUT e della kick sequence non identificato.
 * Valori hardcoded dal trace d6220 ch36.
 */
void b43_phy_ac_rxiqcal_prep_second_iter(struct b43_wldev *dev);

/*
 * Building block usato sia dalla fase B5 (iter 1-18) sia dal gruppo 4
 * (iter 19-24). Emette la sequenza kick + poll + readback + writeback:
 *   WR 0x0381 = 0x7976
 *   [n_pre_clear × TBL.WR id=0x000c off=pre_clear_offs[i] val=0]
 *   WR 0x0383 = 0x003d
 *   WR 0x0380 = cmd (arm bit 15)
 *   poll 0x0380 finché bit 15 = 0 (HW busy)
 *   RAD.RD 0x0144 + core_off (result readback)
 *   TBL.RD id=0x000c off=rd_off len=rw_len auto-contained + unlock esplicito
 *   TBL.WR id=0x000c off=wr_off len=rw_len con wr_data (scoped)
 *
 * core_off = 0x0000 / 0x0200 / 0x0400 per core 0 / 1 / 2.
 */
void b43_phy_ac_rxcal_afe_iter(struct b43_wldev *dev,
			       u16 cmd, u16 core_off,
			       const u16 *pre_clear_offs, u8 n_pre_clear,
			       u16 rd_off, u8 rw_len,
			       u16 wr_off, const u16 *wr_data);

/*
 * RX-IQ measurement iters (vendor #46778-#47296, gruppo 4, 519 op).
 * Sei iter di misura RXIQ post gain compensation, 2 per core × 3 core:
 *   iter 19 (core 0, cmd 0x8084):  63 op (43 polls)
 *   iter 20 (core 0, cmd 0x8056): 263 op (60 polls + batch fast 36×5+3)
 *   iter 21 (core 1, cmd 0x9084):  55 op (35 polls)
 *   iter 22 (core 1, cmd 0x9056):  32 op (12 polls)
 *   iter 23 (core 2, cmd 0xa084):  28 op (8 polls)
 *   iter 24 (core 2, cmd 0xa056):  78 op (58 polls)
 *
 * Ogni iter usa il pattern b43_phy_ac_rxcal_afe_iter (senza pre-clear).
 * Iter 20 aggiunge un batch fast 36× TBL.WR id=0x000c len=1 (LUT
 * simile a rxiqcal_prep_second_iter ma con valori aggiornati).
 *
 * TODO: valori TBL.WR len=2 per-iter e batch fast LUT hardcoded dal trace.
 * Devono derivare runtime dai risultati della misura (RAD.RD + TBL.RD).
 */
void b43_phy_ac_rxiqcal_run_meas_iters(struct b43_wldev *dev);

/*
 * RX-IQ apply TX bbmult + kick (vendor #47297-#47328, fase F seg A, 32 op).
 * Post-measurement, riscrive i bbmult TX-side compensati (gli stessi
 * valori scritti da rxiqcal_apply_tx_gain_bbmult) e emette un pulse su
 * 0x0001 bit 14 (kick "commit" latch, SALAME sulla semantica precisa),
 * poi resetta 0x0382 a 0.
 *
 * Struttura:
 *   3 op standalone: peek 0x0464 + AND 0x0382 clr bit 15 + AND 0x0460 clr bit 2
 *   Sub-batch core 0 (13 op): preamble (2) + 2× fast TBL.WR bbmult (10) + postamble (1)
 *   Sub-batch core 1 (13 op): idem con offset +4
 *   3 op standalone: MOD 0x0001 set bit 14 + MOD 0x0001 clr bit 14 + WR 0x0382 = 0
 *
 * TODO: valori bbmult (0x40, 0x3c) sono gli stessi di
 * rxiqcal_apply_tx_gain_bbmult. Semantica del pulse su 0x0001 bit 14
 * e del peek 0x0464 non identificata (probabile classifier reset).
 */
void b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(struct b43_wldev *dev);

/*
 * IQ coeff tables reset (vendor #47329-#50016, fase F seg B, 2688 op).
 * Emette 384 TBL.WR auto-contained (7 op ciascuna) interleaved su 3
 * TBL id per ogni offset:
 *   (0x0042, off), (0x0062, off), (0x0082, off) per off = 0x00..0x7f.
 *
 * Valori:
 *   TBL 0x0042: 128 × 0x0000
 *   TBL 0x0062: 128 × 0x0000
 *   TBL 0x0082: 33 × 0xf8fc (off 0x00-0x20) + 95 × 0xf6f2 (off 0x21-0x7f)
 *
 * NB: rimosso il prefix "rx" dal nome perché non ho evidenza che queste
 * tabelle siano RX-only. Il pattern (2 tabelle a zeri + 1 tabella con
 * valori fissi) suggerisce inizializzazione di coefficient tables ma
 * la direction TX vs RX non è determinabile dal solo trace. SALAME:
 * i due valori distinti in TBL 0x0082 (boundary a off 0x21) potrebbero
 * indicare un sub-band/rate-set split.
 */
void b43_phy_ac_iqcal_coeff_tables_reset(struct b43_wldev *dev);

/*
 * IQ-cal secondary stage apply (vendor #50152-#50198, 47 op).
 * Sequenza post radio_chain range-setup (variante corta) che kicka una
 * unità di correlator (registri 0x0400-0x0403), poi zera 4 slot in
 * 0x06a0/06a1/08a0/08a1, MOD 0x0211 bit 0 clr, poi 2 pair TBL.RD+TBL.WR
 * len=2 su 0x000c off=0x60 e off=0x64 applicando gli STESSI VALORI
 * misurati durante iter 20 core 0 e iter 22 core 1. Chiude con sync
 * (peek 0x019e + gate re-lock).
 *
 * Struttura op-count-verified (47 op):
 *   Kick (7): peek 0x0400 + peek 0x019e + MOD 0x019e set bit 0 +
 *             OR 0x0400 (0x0003) + OR 0x0402 (0x0020) + peek 0x0403 +
 *             WR 0x0400 = 0
 *   Gate reset (1): WR 0x019e = 0x03d0
 *   Zeros (4): 4 WR 0x06a0/06a1/08a0/08a1 = 0
 *   MOD (1): MOD 0x0211 clr bit 0
 *   Pair 1 (16): TBL.RD 0x000c off=0x60 auto + unlock + TBL.WR scoped
 *                len=2 vals=[0x0062, 0xfffd]  (iter 20 core 0 vals)
 *   Pair 2 (16): TBL.RD 0x000c off=0x64 auto + unlock + TBL.WR scoped
 *                len=2 vals=[0x0023, 0x0003]  (iter 22 core 1 vals)
 *   Close (2): peek 0x019e + MOD gate re-lock
 *
 * SALAME: il nome "second_stage" è mio, senza evidenza. Semantica dei
 * registri 0x0400-0x0403 (correlator kick) e 0x06a0/8a0 (zero slots)
 * non identificata. Nomenclatura "iqcal_" senza rx/tx prefix perché il
 * flow tocca sia lato TX (0x06a0 unknown) sia sync gate.
 */
void b43_phy_ac_iqcal_apply_second_stage(struct b43_wldev *dev);

/*
 * RX-gain config readback (vendor #50199-#50292, 94 op).
 * Legge lo stato attuale dei gain regs per-core prima dell'apply
 * massivo che segue. Il read è "trace-only" — i valori sono scartati
 * a livello di codice ma emessi come PHY.RD nel trace per matching
 * op-per-op contro il vendor.
 *
 * Struttura op-count-verified:
 *   Preamble globale (2): peek 0x040f + MOD 0x040f clr bit 9
 *   Per-core (46 op × 2 core = 92):
 *     25 peek gain regs (ordine hardcoded 0x0720-0x0747)
 *     1× fast TBL.RD id=0x000c bbmult (5 op)
 *     3× fast TBL.RD id=0x0007 gain code (5 op each = 15)
 *     1 peek 0x?73e
 *
 * SALAME: nome speculativo. La sequenza è chiaramente readback ma la
 * ratio precisa (perché leggere 25 regs specifici, in quest'ordine)
 * non è identificata. Ordinato dal trace vendor.
 */
void b43_phy_ac_rxgain_config_readback(struct b43_wldev *dev);

/*
 * RX-gain config apply (vendor #50293-#50438, 146 op).
 * Fase B post-readback: applica le configurazioni finali dei gain regs
 * dopo il round completo di RXIQ measurement.
 *
 * Struttura op-count-verified:
 *   Header (3): peek 0x0401 + 2× MOD 0x0401 (bit-field mask 0x0007 e 0x7000)
 *   Per core (71 × 2 core = 142):
 *     52 MOD (phase 1 bit-field config su 0x?720-0x?73e area)
 *     1× fast TBL.RD id=0x0007 off=0x140+core*0x10 (5 op) — readback
 *     12 MOD (phase 2 bit-field config su 0x?723/0x?735/0x?737/0x?729/0x?721)
 *     1 peek 0x?78 + 1 MOD 0x?78 clr bit 0 (2 op)
 *   Trailer (1): MOD 0x019e clr bit 1 (gate unlock)
 *
 * TODO: i 64 MOD per-core sono hardcoded dal trace. Formula runtime
 * non identificata. Semantica dei bit-field non chiara (SALAME).
 */
void b43_phy_ac_rxgain_config_apply(struct b43_wldev *dev);

/*
 * Radio 2069 IQ-cal config per-core (vendor #50439-#50522, 84 op).
 * Setup dei radio regs 0x0020-0x003d per-core post-cal:
 *   Per core (42 op × 2 core):
 *     6 RAD.RD (readback stato attuale)
 *     6 RAD.WR = 0 (zero out)
 *     10 RAD.MOD (bit-field config, ogni MOD emette 3 op via wrapper)
 *
 * Regs toccati per core: 0x0020, 0x0021, 0x0022, 0x0023, 0x003a, 0x003d
 * (core 1 con stride +0x200).
 *
 * SALAME: la semantica dei radio regs 0x0020-0x003d non è documentata.
 * Il pattern zero-then-config è tipico di setup post-cal ma la funzione
 * esatta è ignota.
 */
void b43_phy_ac_radio_iqcal_config(struct b43_wldev *dev);

/*
 * Gain control final apply (vendor #50523-#50651 e #50837-#50962, 126-129 op).
 * Fase che finalizza il chain gain e riscrive i gain code + bbmult con
 * valori runtime-derived dalla RXIQ measurement. Pattern 3-core.
 *
 * Parametri:
 *   with_peek_preamble: se true, emette 3 peek 0x?dc preamble.
 *   r734_vals: array di num_cores u16, uno per core, per WR 0x?734.
 *   num_cores: 2 o 3. Osservato: 6°/7°/8° txpwr = 3 core, 9° txpwr = 2 core.
 *
 * Op count = (with_peek_preamble ? 3 : 0) + num_cores * 42
 *   - 6° txpwr: preamble=true,  3 core, r734={4,4,4}       → 129 op
 *   - 7° txpwr: preamble=false, 3 core, r734={2,2,1}       → 126 op
 *   - 8° txpwr: preamble=false, 3 core, r734={1,1,0}       → 126 op
 *   - 9° txpwr: preamble=false, 2 core, r734={0,0}         →  84 op
 *
 * Struttura op-count-verified per core (42 op):
 *   3 WR: 0x?730=0x00b0 + 0x?731=0x0004 + 0x?734=r734_vals[core]
 *   3 MOD 0x?722: set bit 1 + set bit 2 + set bit 3
 *   Preamble (2): peek 0x019e + MOD lock
 *   Fast TBL.RD id=0x0020 off=0x0000 (5 op) — gain LUT readback
 *   3× fast TBL.WR id=0x0007 off=0x100+c/0x103+c/0x106+c gain code (15)
 *     Values (hardcoded dal trace): g0=0, g1=0xff7f, g2=0x00f3
 *   Sync (2): peek 0x019e + MOD lock
 *   2× fast TBL.WR id=0x000c off=bbmult_lo/hi (10 op) — bbmult=0x0044
 *   Bridge (2): MOD 0x019e set + MOD clr
 *
 * Preamble globale opzionale (3): 3 peek 0x?dc (3-core stride +0x200)
 *
 * SALAME: nome "final" mio. Il pattern **è chiaramente un secondo giro di
 * txpwr apply** (marker TBL.WR 0x0007 off=0x0100 presente) ma con valori
 * diversi da txpwr_by_index(INDEX_DEFAULT). Runtime formula per g1, bbmult
 * e r734_vals pending.
 *
 * NOTA: pattern 3-core hardcoded (a differenza delle prime 5 chiamate
 * txpwr che erano 2-core sul d6220).
 */
void b43_phy_ac_gainctrl_final_apply(struct b43_wldev *dev,
				     bool with_peek_preamble,
				     const u16 *r734_vals,
				     unsigned int num_cores);

/*
 * DDS/NCO second tone seed (vendor #50652-#50737, 86 op).
 * Analogo a rxiqcal_dds_seed ma emette **solo** la TBL.WR id=0x000e
 * off=0x0000 len=40 (senza il preambolo WR 0x0382 + 3× TBL.WR len=2).
 * I 40 u32 sono una sequenza DDS/NCO **diversa** dal primo dds_seed
 * (probabile secondo tone frequency per il post-cal validation).
 * Anche questa ha simmetria periodica 20+20 (metà1 == metà2).
 *
 * SALAME: sospetto "second tone" mio, senza evidence formale — è possibile
 * che sia una diversa modulazione (non solo diverso tone).
 */
void b43_phy_ac_rxiqcal_dds_seed_second_tone(struct b43_wldev *dev);

/*
 * DDS/NCO third tone seed (vendor #51957-#52042, 86 op).
 * Come dds_seed_second_tone (solo TBL.WR len=40) ma con LUT diversa:
 * i valori sono gli stessi del secondo tone MA in ordine "reversed"
 * (index i → 20-i per i ∈ [1..19], con simmetria 20+20 preservata).
 * Probabile secondo tone con **fase o frequenza invertita** — non
 * confermato.
 *
 * SALAME: relazione "reversed" è pattern osservato ma la ratio precisa
 * non è identificata.
 */
void b43_phy_ac_rxiqcal_dds_seed_third_tone(struct b43_wldev *dev);

/*
 * IQ-cal measurement + apply post second DDS (vendor #50738-#50836, 99 op).
 * Sequenza post secondo dds_seed:
 *   Blocco A (14): 2× TBL.RD auto-contained bbmult (7 op each) — readback
 *   Blocco B (2):  MOD 0x0001 pulse bit 14 (commit)
 *   Blocco C (13): kick sequence per il correlatore, variante del
 *                  rxiqcal_prep_second_iter seg B — differenza: al posto di
 *                  OR 0x0382 set 0x8000 c'è OR 0x0460 set 0x0001
 *   Blocco D (18): tail di rxgain_perchan_config (3 peek+WR paia per-core
 *                  + 6 WR standalone finali con valori rimappati)
 *   Blocco E (variabile): setup 0x0272/0x0271/0x0270 + N peek 0x0270 (poll)
 *                          — 5 peek nel 1° ciclo, 6 nel 2°, ...
 *   Blocco F (12): 12 peek gain regs (0x?c0-0x?c5 per 2 core)
 *   Blocco G (29): apply bbmult per-core — variante di
 *                  rxiqcal_apply_tx_bbmult_kick (differenza: OR 0x0460 nel
 *                  header al posto di AND 0x0382; footer senza WR 0x0382=0)
 *   Blocco H (2):  MOD 0x0001 pulse finale
 *
 * Op count total = 94 + n_peek270
 *
 * SALAME: nome speculativo. La sequenza è chiaramente post-second-DDS ma
 * la ratio esatta non è identificata.
 */
void b43_phy_ac_iqcal_meas_post_dds_apply(struct b43_wldev *dev,
					  unsigned int n_peek270);

/*
 * Variante v2 del meas_post_dds_apply (vendor #51814-#51956, 143 op).
 * Chiamata **una sola volta**, dopo il 4° meas_apply e il 5° dds_seed.
 * Struttura:
 *   Blocchi A+B+C+D (47) — identici alla versione originale
 *   Blocco E' variante (65 op):
 *     Setup (3): WR 0x0272=0x4000, MOD 0x0271, MOD 0x0270 clr bit 1
 *     Sub-block core 1 (27): 4 peek + 4 MOD + arm + n_poll_c1 peek 0x0270 +
 *                            4 WR + 1 peek extra
 *     6 peek core 0 0x?c0-?c5 (6)
 *     Sub-block core 0 (23): 4 peek + 4 MOD + arm + n_poll_c0 peek 0x0270 +
 *                            4 WR + 1 peek extra
 *     6 peek core 1 0x?c0-?c5 (6)
 *   Blocco G+H (31) — identico alla versione originale
 *
 * Parametri: n_poll_c1 (13 nel d6220), n_poll_c0 (9 nel d6220).
 * Op count = 47 + 65 + 31 = 143.
 *
 * SALAME: la variante fa un "config + arm + poll + commit" per-core dei
 * gain regs 0x?20/0x?21/0x?28/0x?29 con valori WR finali (0x0321, 0x7761,
 * 0x0080, 0x0182). Sembra una fase di sequenziamento hardware post-cal ma
 * la semantica precisa non è identificata.
 */
void b43_phy_ac_iqcal_meas_post_dds_apply_v2(struct b43_wldev *dev,
					     unsigned int n_poll_c1,
					     unsigned int n_poll_c0);

/*
 * RXIQ correction coefficients per-chan write (vendor #52252-#52255, 4 op).
 * Scrive 2 coefficienti I/Q per core in 0x?a0/0x?a1. I valori sono
 * derivati runtime dalla RXIQ measurement (hardcoded qui dal trace).
 * Osservato (d6220 ch36):
 *   Core 0: WR 0x06a0=0x03f2, WR 0x06a1=0x004c
 *   Core 1: WR 0x08a0=0x03db, WR 0x08a1=0x0037
 *
 * SALAME: interpretazione "correction coefficients" mia. I registri
 * 0x?a0/0x?a1 sono stati usati (letti/scritti a zero) in fase A/B (turno 8),
 * ma la loro semantica precisa non è documentata.
 */
void b43_phy_ac_rxiq_apply_coefficients(struct b43_wldev *dev);

/*
 * Radio 2069 IQ-cal teardown (vendor #52256-#52267, 12 op).
 * Reset dei radio regs 0x0020-0x003d dopo la RXIQ measurement.
 * Analogo a radio_iqcal_config ma solo WR (no readback, no MOD):
 *   Per core: 6 RAD.WR = 0 su {0x20, 0x21, 0x22, 0x23, 0x3a}, 1 RAD.WR
 *             = **0x000f** su 0x3d (NOTA: valore non zero!)
 */
void b43_phy_ac_radio_iqcal_teardown(struct b43_wldev *dev);

/*
 * RXIQ cal teardown + apply defaults (vendor #52268-#52452, 185 op).
 * 10° chiamata txpwr (marker TBL.WR 0x0007 off=0x0100) + 11° chiamata
 * txpwr (stesso marker a #52293). Le due chiamate sono così ravvicinate
 * perché il vendor **ripete** l'apply gain/bbmult prima e dopo un
 * TBL.RD 0x0020 off=**0x0040** (readback di gain LUT alternativa).
 *
 * Struttura:
 *   Preamble globale (3): peek 019e + MOD lock + WR 0x0401 = 0x7733
 *   Per core (91 op):
 *     TXPWR sequence (64 op):
 *       3× fast TBL.WR id=0x0007 gain code (15) — INDEX_DEFAULT values:
 *         g0=0, g1=0x2f13, g2=0x00f3
 *       Sync (2): peek 019e + MOD lock
 *       Fast TBL.RD id=0x0020 off=**0x0040** (5) — gain LUT alt readback
 *       3× fast TBL.WR gain code RIPETUTI (15) — same values
 *       Sync (2)
 *       2× fast TBL.WR id=0x000c bbmult (10) — bbmult=0x0035
 *       Bridge (4): MOD × 2 idempotent + peek + MOD lock
 *       2× fast TBL.WR bbmult RIPETUTI (10)
 *       Trailer (1): MOD 0x019e set bit 1
 *     Reset gain regs (27 WR): riscrive 27 gain regs core 0x?720-0x?747
 *       (+0x?678, 0x?73e) a valori di startup (per core, ordine osservato)
 *
 * SALAME: la ratio del "double-apply" (2 volte prima e dopo TBL.RD 0x0040)
 * non è documentata. Ipotesi: il TBL.RD 0x0040 è un commit sync che
 * richiede riscrittura per applicare i nuovi valori dopo il commit.
 */
void b43_phy_ac_rxiq_teardown_apply_defaults(struct b43_wldev *dev);

/*
 * RXIQ cal finalize (vendor #52453-#54732, ~2280 op).
 * Fase finale post-teardown: chiude lo scope RXIQ, ripristina lo stato
 * globale della PHY per la ricezione normale, e programma i registri
 * di controllo MAC/GPIO/probe.
 *
 * Struttura:
 *   Blocco A (10): finalize kick — WR 0x040f + peek 0x0400 + gate save +
 *                  OR 0x0400/0x0402 + peek 0x0403 + WR 0x0400 + gate restore
 *   Blocco B (16): 2× TBL.WR id=0x000c len=2 auto-contained (off=0x60, 0x64)
 *   Blocco C (10): misc setup — WR 0x0140 + 4 MOD 0x02ed-f9 + 3 AND 0x?d4 +
 *                  WR 0x0339
 *   Blocco D (49): TBL.WR 0x5f + 2× (TBL.RD off=0x60 + TBL.RD off=0x62 +
 *                  4 RAD.RD + 2 peek 0x?a0/?a1)
 *   Blocco E (28): AFE/CRS/nfloor reconfig — MOD gain-related + MAC/MHF/GPIO
 *                  seq + MOD crs_reg 0x0324-0x0333 + MOD noise floor clear
 *
 *   Probe cycle 1 (85):  5 iter × 17 op (mode_vals={4,0,4,0,4})
 *   Measure block (393): RX AFE reconfig (86) + Radio 2069 second IQ-cal
 *                        (68) + rxcal cleanup preamble (5) + tail perchan
 *                        (18) + arm tone gen (3) + poll blocks TX AFE (163)
 *                        + reset gain regs PHY (29) + radio reset (14) +
 *                        finalize (5) + 2 MAC toggle arm. Vedi helper
 *                        b43_phy_ac_rxiqcal_measure_block.
 *   Probe cycle 2 (170): 10 iter × 17 op (mode_vals={0,4,0,4,...})
 *   Measure block (393)
 *   Probe cycle 3 (170): stessa struttura
 *   Measure block (393)
 *   Probe cycle 4 (170): stessa struttura
 *   Measure block (393)
 *
 *   Probe cycle 5 (18): 1 iter con MAC toggle esteso (bit 20 clr tra
 *                       mac_enable e mac_suspend del post-mode-change)
 *   Post-probe AFE final config (16): peek gate + lock + MOD gain regs
 *                       0x0070-0x0072 e 0x0644-0x0846
 *   Bulk TBL id=0x0040 len=128 (130): LUT power-vs-index primaria
 *   Bulk TBL id=0x0060 len=128 (130): LUT power-vs-index secondaria
 *   Bulk TBL id=0x0021 len=24 (52):   u32 flag table
 *   Coda finale (72): unlock gate + 5 gain regs re-emit + MAC sequence
 *                     + per-core RXIQ coefficient write + GPIO + PMU.RC
 *
 * SALAME: la ratio delle 4 iterazioni measure+probe non è documentata.
 * Ipotesi: convergenza iterativa sui coefficienti RXIQ/TX-AFE con misura
 * (poll blocks) e mode sweep (probe cycle) alternati.
 */
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev);

/*
 * AC-PHY low-level helpers (helpers_phy_ac.c). Prototypes esposti qui perché
 * i due file di traduzione sono distinti (vedi helpers_phy_ac.c per il
 * razionale del split).
 *
 * b43_phy_ac_mhf_maskset: r/m/w di un HOSTF slot (0..4) via SHM. Differisce
 * da b43_hf_write mainline che ha signature u64 e copre solo i 3 slot
 * bassi; il vendor AC-PHY usa 5 slot indipendenti e wl-diag traccia ogni
 * scrittura come "MAC.MHF addr=<slot>".
 *
 * b43_maccontrol_set: r/m/w di B43_MMIO_MACCTL. Non è esportato dal core
 * b43 mainline, dove il pattern equivalente è b43_maskset32(dev,
 * B43_MMIO_MACCTL, mask, set) inlineato per ogni sito.
 */
void b43_phy_ac_mhf_maskset(struct b43_wldev *dev, u16 slot, u16 mask, u16 val);
void b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set);

#endif /* B43_PHY_AC_H_ */
