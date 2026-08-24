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
/*
 * Bit 0 dello stesso registro: abilita la sequenza di tuning radio, non e' un
 * secondo lock. Il driver stock lo alza prima del banco PLL del 2069 e lo
 * abbassa dopo l'ultima write (d6220 attach-to-bss-up #4497 e #4616); subito
 * dopo la chiusura reinizializza il registro con 0x01c0 / 0x0200 / 0x003c.
 */
#define  B43_PHY_AC_TBL_WRITE_GATE_RADIO_TUNE	0x0001
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
#define B43_PHY_AC_STATE_AFE_ON		0x0200	/* RF front-end armed (enable_afe ON) */
/*
 * Primo bring-up. b43_phy_init azzera phy->do_full_init fra ops->init e
 * set_channel, quindi da set_channel in avanti quel flag e' sempre falso:
 * op_init lo latcha qui mentre e' ancora valido, per le costanti che il driver
 * stock sceglie in base alla fase.
 */
#define B43_PHY_AC_STATE_FIRST_BRINGUP	0x0800
/*
 * Richiesta di risorsa PMU (regctl 0 bit 1) alzata dal driver. Il bit hardware
 * non e' rileggibile attraverso l'API bcma, quindi lo si traccia qui: serve a
 * distinguere "va abbassato" da "e' gia' basso", come il refcount fa per il MAC.
 */
#define B43_PHY_AC_STATE_PMU_REQ	0x0400
#define B43_PHY_AC_STATE_FAULTED	0x8000	/* sticky: a precondition failed */

/* Per-device PHY state. */

/*
 * Hard maximum of RF chains the driver programs. Every per-core array here
 * and in phy_ac.c / rxiqcal_phy_ac.c is sized on this, and num_cores is
 * clamped to it in b43_phy_ac_probe_cores.
 */
#define B43_PHY_AC_MAX_CORES		3

/*
 * Accumulatori RXIQ per core. Il solve somma i **due** round di stima piu'
 * recenti -- verificato: con un round solo i coefficienti non combaciano. Le
 * letture di 0x?c0-0x?c5 avvengono nella misura, che gira piu' volte; i valori
 * si conservano qui perche' il solve sta in un'altra funzione.
 */
struct b43_phy_ac_iq_acc {
	/* Finestra scorrevole degli ultimi due round: [0] e' il piu' recente. */
	u32 ii[2];
	u32 qq[2];
	s32 iq[2];
	unsigned int rounds;
	/* Coefficienti risolti, calcolati una volta e riapplicati: il driver stock
	 * riscrive gli stessi valori alla seconda apply invece di ricalcolare. */
	s16 a;
	s16 b;
	bool solved;
};

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
	 * Stato del toggle su 0x0520[3:2] usato dai probe cycle di
	 * rxiqcal_finalize. Il driver stock lo alterna a ogni gruppo di peek per
	 * tutta la fase, senza ripartire fra un blocco e il successivo, e
	 * l'origine dipende dalla fase: 0x0000 al primo bring-up, 0x0004 su uno
	 * switch di canale successivo. Verificato sulle due catture d6220 con i
	 * RETVAL: alternanza perfetta su 15 e 19 occorrenze rispettivamente.
	 */
	u16 probe_mode;
	/*
	 * Contatore di cicli di calibrazione della sessione, per il gate del
	 * bump a freddo di recalc_txpower (crsmin): il blob bumpa la ladder
	 * per le prime due calibrazioni. Satura a 2 a quanto pare.
	 */
	u8 cal_cycles;
	/* Accumulatori RXIQ raccolti dalla misura, consumati dal solve. */
	struct b43_phy_ac_iq_acc iq_acc[B43_PHY_AC_MAX_CORES];
	/* Salvati da rxcal_radio_setup, riscritti da rxcal_radio_cleanup. */
	u16 rxcal_radio_saved[B43_PHY_AC_MAX_CORES][7];
	/*
	 * Risultati degli iter di commit della cal AFE, indicizzati dall'offset di
	 * scrittura. La coda di rxcal_afe_calibrate li duplica per antenna.
	 */
	struct {
		u16 off;
		u16 v[2];
		u8 n;
	} afe_res[6];
	/*
	 * Snapshot di afe_res a fine b43_phy_ac_rxcal_afe_calibrate (pass 1
	 * della cal). Serve perchè il blob riscrive gli stessi offset con
	 * i risultati del secondo tono, applicati una volta temporaneamente,
	 * mentre le riapplicazioni finali (finalize kick e rxiqcal_finalize)
	 * usano di nuovo i risultati della pass 1.
	 * osservato sull'attach d6220: #25697 scrive la pass 2,
	 * #28651 e #30097 di nuovo la pass 1.
	 */
	struct {
		u16 off;
		u16 v[2];
		u8 n;
	} afe_res_cal[6];
	/* pa5ga/maxp5ga sub-band group (0..3) for the current channel, cached
	 * by txpwrctrl_setup so later cal blocks can derive per-core power. */
	u8 pa5g_grp;
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
	u16 rxcal_imbalance[B43_PHY_AC_MAX_CORES][4][8];
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
 * NOTE on STATE_PHY_RUN: mirrors BBCFG[15], kept only for parity with the
 * annotator (annotate_enables.py tracks the same bit). No driver code sets or
 * clears it, by design: the vendor never writes BBCFG[15] in the captured
 * flows (attach-ch36 and down-to-bss-up show only RSTCCA / bit 0x4000 pulses
 * on BBCFG), so the baseband run-state is established at chip power-on and is
 * never toggled in the sequences the driver reproduces. Do not use
 * STATE_PHY_RUN in `want`/`forbid`: it has no mutator and reflects nothing
 * observable in these flows.
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
 * Calibrazioni post-channel-setup, nell'ordine in cui le chiama
 * b43_phy_ac_set_channel_calibrations (phy_ac.c), che documenta i round.
 * Ogni funzione e' una fase trascritta dalla cattura vendor; la struttura
 * interna, gli op count e i valori ancora cablati stanno accanto al codice.
 * Mappa fase -> intervallo di op nella cattura: docs/vendor-op-map.md.
 */
void b43_phy_ac_post_cal_finalize(struct b43_wldev *dev);
void b43_phy_ac_post_cal_finalize_iter3(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply(struct b43_wldev *dev);
void b43_phy_ac_post_rxiqcal_stage2(struct b43_wldev *dev);
void b43_phy_ac_rxcal_afe_calibrate(struct b43_wldev *dev);
void b43_phy_ac_rxcal_afe_finalize_gain_luts(struct b43_wldev *dev);

/* Open-loop TX power. INDEX_DEFAULT e' il valore fisso della cattura. */
#define B43_PHY_AC_TXPWR_INDEX_DEFAULT	0x40
void b43_phy_ac_txpwr_by_index(struct b43_wldev *dev, u8 idx);

void b43_phy_ac_rxgain_defaults_pulse(struct b43_wldev *dev);
void b43_phy_ac_radio_chain_range_setup(struct b43_wldev *dev, bool with_tune);
void b43_phy_ac_rxgain_perchan_config(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply_tx_gain_bbmult(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_dds_seed(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_prep_second_iter(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_run_meas_iters(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_apply_tx_bbmult_kick(struct b43_wldev *dev);
void b43_phy_ac_iqcal_coeff_tables_reset(struct b43_wldev *dev);
void b43_phy_ac_iqcal_apply_second_stage(struct b43_wldev *dev);
void b43_phy_ac_rxgain_config_readback(struct b43_wldev *dev);
void b43_phy_ac_rxgain_config_apply(struct b43_wldev *dev);
void b43_phy_ac_radio_iqcal_config(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_dds_seed_second_tone(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_dds_seed_third_tone(struct b43_wldev *dev);
void b43_phy_ac_iqcal_meas_post_dds_apply(struct b43_wldev *dev);
void b43_phy_ac_iqcal_meas_post_dds_apply_v2(struct b43_wldev *dev);
void b43_phy_ac_rxiq_apply_coefficients(struct b43_wldev *dev);
void b43_phy_ac_radio_iqcal_teardown(struct b43_wldev *dev);
void b43_phy_ac_rxiq_teardown_apply_defaults(struct b43_wldev *dev);
void b43_phy_ac_rxiqcal_finalize(struct b43_wldev *dev);

/*
 * Un iter della cal AFE: arma un comando su 0x0380, attende il bit di busy,
 * rilegge il risultato e lo riscrive su wr_off. Usato dai due gruppi di iter.
 * core_off = core * 0x200.
 */
void b43_phy_ac_rxcal_afe_iter(struct b43_wldev *dev,
			       u16 cmd, u16 core_off,
			       const u16 *pre_clear_offs, u8 n_pre_clear,
			       u16 rd_off, u8 rw_len, u16 wr_off);

/*
 * Un round della ricerca del guadagno di loopback. core_mask seleziona i core
 * del round, r734_vals[] e' indicizzato per numero di core. Il criterio di
 * convergenza e il chiamante sono in phy_ac.c.
 */
void b43_phy_ac_gainctrl_final_apply(struct b43_wldev *dev,
				     bool with_peek_preamble,
				     u8 core_mask,
				     const u16 r734_vals[3]);

/*
 * Watchdog periodico a regime: poll TSSI/statistiche SHM ogni tick, con
 * una tornata singola del measure block (noise_cal=true) alla cadenza
 * lunga. Sequenza op-per-op del tick vendor ~5 s (sweep d6220); nel
 * driver e' agganciato a pwork_15sec. Vedi il commento in phy_ac.c.
 */
void b43_phy_ac_watchdog(struct b43_wldev *dev, bool noise_cal);

/* Helper trasversali al confine MAC/PHY; razionale in helpers_phy_ac.c. */
void b43_phy_ac_mhf_maskset(struct b43_wldev *dev, u16 slot, u16 mask, u16 val);
void b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set);

/*
 * Function-boundary markers for the userspace test harness. B43_AC_FN() at the
 * top of a function makes the harness bracket the ops that follow with the
 * function name, so localize_functions.py can segment the generated trace by
 * exact boundaries instead of guessing fingerprints from source. The exit
 * marker is emitted automatically on scope exit (any return) via GCC's
 * cleanup attribute, so nested calls nest correctly. No-op in the kernel
 * build; the harness defines B43_AC_FN_TRACE and provides the hooks, which
 * emit only when enabled at runtime, so the compare.py trace stays clean.
 */
#ifdef B43_AC_FN_TRACE
void b43_ac_fn_enter(const char *fn);
void b43_ac_fn_leave(const char *fn);
static inline void b43_ac_fn_cleanup(const char *const *fn) { b43_ac_fn_leave(*fn); }
#define B43_AC_FN() \
	const char *const __b43_fn __attribute__((cleanup(b43_ac_fn_cleanup))) = __func__; \
	b43_ac_fn_enter(__func__)
#else
#define B43_AC_FN() do { } while (0)
#endif

#endif /* B43_PHY_AC_H_ */
