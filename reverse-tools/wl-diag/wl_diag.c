// SPDX-License-Identifier: GPL-2.0
/*
 * wl_diag (B) - tracer PHY/radio/PMU per il driver Broadcom "wl" SENZA kprobe.
 *
 * Su questo kernel CONFIG_KPROBES e' disattivato, quindi aggancio gli accessor
 * con un detour all'ingresso funzione: risolvo l'indirizzo via kallsyms,
 * sovrascrivo le prime 4 istruzioni con un salto (lui/ori/jr $t9) verso uno
 * stub eseguibile che registra (op, addr, val, mask), poi riesegue le 4
 * istruzioni originali rilocate e torna a func+16.
 *
 * Cattura SOLO gli argomenti d'ingresso (a1=addr, a2/a3=val/mask). Il valore
 * RESTITUITO dalle read non viene tracciato (scelta di semplificazione): per
 * questo phy_reg_read/read_radio_reg loggano solo l'indirizzo letto, e il
 * decoder li emette come val=UNDEFINED (mai 0x0000).
 *
 * read_radio_reg ha un branch alla 4a istruzione (beq): il detour classico a 4
 * parole e' impossibile. Si aggancia con la variante "short-j" (campo shortj):
 * si sovrascrive la SOLA parola d'ingresso con 'j stub' (patch atomica); la 2a
 * parola resta come delay slot e viene comunque rieseguita dallo stub, che
 * riesegue o[0..1] e rientra a func+8. Richiede lo stub nella stessa regione
 * 256MB (verificato in init). osl_delay (usec=a1) e wlc_phy_table_{read,write}_
 * acphy (id/len/off = a1/a2/a3) usano il detour classico a 4 parole.
 *
 * Sicurezza: default arm=0 (dry-run, solo log del piano). Con arm=1 applica le
 * patch scrivendo la parola d'ingresso PER ULTIMA (transitori benigni: t9 non
 * e' usato dai prologhi, verificato sul binario). Il pool stub e' statico e
 * non viene mai liberato, cosi' uno stub eventualmente ancora in volo allo
 * scarico esegue comunque codice valido.
 *
 * Assunzione runtime (MIPS32R1, niente NX/RODATA per il testo dei moduli):
 * memoria modulo RWX + flush_icache_range esplicito. Da confermare sul device.
 *
 * Target: kernel 3.4.x, MIPS32 big-endian, o32, SMP=2, PREEMPT.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/kfifo.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <asm/cacheflush.h>
/* Serve per BRK_KPROBE_BP, che discrimina se il kernel ha il ramo
 * notify_die(DIE_BREAK) in do_bp: senza questo include il #ifdef piu'
 * sotto sarebbe sempre falso e il percorso break si compilerebbe via in
 * silenzio anche dove e' disponibile. */
#include <asm/break.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>

static int arm;
module_param(arm, int, 0444);
MODULE_PARM_DESC(arm, "0=dry-run (solo log del piano), 1=applica le patch");

/* osl_delay e' rumoroso (una entry per ogni udelay) e il valore usec catturato
 * da a1 non e' affidabile su tutti i percorsi -- a volte e' spazzatura (arg in
 * registro diverso / percorso inline). Lo lasciamo staccato di default; delay=1
 * per ri-agganciarlo quando serve davvero la temporizzazione. */
static int delay;
module_param(delay, int, 0444);
MODULE_PARM_DESC(delay, "0=non agganciare osl_delay (default), 1=aggancia");

/* flush_icache_range non e' esportato ai moduli, e su questo kernel (KALLSYMS
 * senza KALLSYMS_ALL) la variabile-puntatore non e' nemmeno visibile a
 * kallsyms perche' sta in BSS. Risolviamo quindi la funzione di testo del
 * cache-layer R4K (vedi wd_init) e la chiamiamo via questo puntatore. */
typedef void (*flush_fn_t)(unsigned long, unsigned long);
static flush_fn_t p_flush_icache;
static void flush_i(unsigned long s, unsigned long e)
{
	if (p_flush_icache)
		p_flush_icache(s, e);
}

/* ---- record + coda + char device (uguale alla versione kprobe) -------- */
#define WLDIAG_MAGIC 0x57444731u
enum wldiag_op {
	OP_PHY_R = 1, OP_PHY_W, OP_PHY_MOD,
	OP_RADIO_R, OP_RADIO_W, OP_RADIO_MOD,
	OP_PMU_CC, OP_PMU_RC, OP_PMU_PLL,
	OP_CC_GPIOCTL, OP_CC_GPIOOUT, OP_CC_GPIOOE,	/* 10,11,12 (append) */
	OP_TBL_R, OP_TBL_W, OP_DELAY,			/* 13,14,15 (append) */
	OP_MAC_MCTRL, OP_MAC_MHF_W, OP_MAC_MHF_R,	/* 16,17,18 (append) */
	OP_PHY_AND, OP_PHY_OR,				/* 19,20 (append) */
	OP_SI_COREREG,					/* 21 (append) */
	OP_ARGX, OP_RETVAL,				/* 22,23 (append) */
	OP_MAC_OBJ_R, OP_MAC_OBJ_W,			/* 24,25 (append) */
	OP_CHANSPEC,					/* 26 (append) */
	OP_TPL_PTRW, OP_TPL_DATW,			/* 27,28 (append) */
	OP_TPL_PTRR, OP_TPL_DATR, OP_TPL_RAMW,		/* 29,30,31 (append) */
	OP_OTP_INIT, OP_OTP_RDW, OP_OTP_RDR,		/* 32,33,34 (append) */
	OP_MAC_BW, OP_SROMCTL_R, OP_SROMCTL_W,		/* 35,36,37 (append) */
	OP_CAL_INIT,					/* 38 (append) */
	OP_DROP = 255,
};
struct wldiag_rec {
	u64 ts_ns; u32 seq; u32 addr; u32 val; u32 aux;
	u8 op; u8 cpu; u16 _pad;
} __packed;

/*
 * Coda allocata a runtime invece che statica: la dimensione diventa un
 * parametro e un'allocazione che non riesce si vede subito invece di gonfiare
 * l'immagine del modulo in .bss.
 *
 * Perche' serve capiente: sulle catture DSL il ritmo massimo osservato e' 5200
 * record/s (fase 80 MHz, con i RETVAL attivi), e con 32768 record il margine
 * era ~6 secondi. Non bastava: quella fase ha perso 1873 record in 232 gocce
 * da 2-3, cioe' la coda era sul filo per tutta la durata. Con il default di
 * 131072 il margine sale a ~25 s.
 *
 * Costo: 28 byte per record, quindi 131072 record sono 3.5 MB di memoria
 * kernel non paginabile. Su un router con 64 MB e' una fetta, ma la si paga
 * solo mentre il modulo e' caricato. Se l'allocazione fallisce si abbassa il
 * parametro: 65536 sono 1.75 MB e ~12 s di margine.
 */
/*
 * Coda su buffer vmalloc'ato, non kfifo_alloc: __kfifo_alloc usa kmalloc, che
 * vuole pagine fisicamente CONTIGUE, e su un router frammentato un'allocazione
 * da qualche MB fallisce. vmalloc non ha quel vincolo. kfifo_init prende il
 * buffer gia' pronto e ci mette la testa della coda sopra.
 *
 * Perche' serve capiente: sulle catture DSL il ritmo massimo osservato e' 5200
 * record/s (fase 80 MHz, con i RETVAL attivi), e con 32768 record il margine
 * era ~6 s. Non bastava: quella fase ha perso 1873 record in 232 gocce da 2-3.
 * Il default di 131072 porta il margine a ~25 s; 262144 a ~50 s, cioe' un
 * ciclo di canale intero.
 *
 * ATTENZIONE a cosa risolve: un buffer piu' grande assorbe le RAFFICHE, non un
 * ritmo medio superiore al drenaggio. Le 232 gocce da 2-3 record della fase 80
 * dicono che la coda era piena piu' volte, cioe' il lettore era in media piu'
 * lento dello scrittore: in quel caso nessuna dimensione basta e va guardato il
 * lato lettura (TCP, `cat`), oppure si filtra di piu' con skipphyrd.
 *
 * Costo: 28 byte per record, quindi 131072 record sono 3.5 MB di memoria
 * kernel non paginabile, pagati solo mentre il modulo e' caricato.
 *
 * fifo_recs viene arrotondato per difetto a potenza di 2: kfifo_init divide la
 * dimensione in byte per esize e fa rounddown_pow_of_two.
 */
/*
 * Forzare la calibrazione completa, senza remove/rescan del device -- che una
 * volta su due non riesce.
 *
 * Nel driver la logica e' questa, letta dai prologhi del blob D6220:
 *
 *   wlc_phy_cal_init(pi)      { if (pi->[251]) return; ...cal completa... }
 *   wlc_set_phy_uninitted(pi) { pi->[418] = -1; pi->[680] = -1; pi->[251] = 0; }
 *
 * Il byte a 251 e' un "gia' calibrato" e la logica e' INVERTITA rispetto a quel
 * che si aspetterebbe: non si scrive 1, si scrive 0. Azzerandolo prima che
 * cal_init lo legga, la calibrazione si rifa'.
 *
 * DUE parametri, e la distinzione conta:
 *
 *   full_init_off   l'offset nella struct, STRUTTURALE: dipende dalla versione
 *                   (251 su 7.14.89, 227 su 6.30) e viene cotto nello stub
 *                   all'arming, quindi e' di sola lettura. 0 = il codice non
 *                   viene emesso affatto.
 *   force_full_init l'interruttore a RUNTIME, scrivibile da sysfs. Lo stub lo
 *                   rilegge a ogni chiamata, quindi si puo' alternare fra un
 *                   ciclo e il successivo:
 *
 *                     echo 1 > /sys/module/wl_diag/parameters/force_full_init
 *                     wl -i wl1 up ; sleep 10 ; wl -i wl1 down    # cal completa
 *                     echo 0 > /sys/module/wl_diag/parameters/force_full_init
 *                     wl -i wl1 up ; sleep 10 ; wl -i wl1 down    # a caldo
 *
 * Serve cosi' perche' capture_plan.sh gira 30-40 combinazioni dopo un solo
 * insmod, e ognuna va provata in entrambi i modi: con un parametro fisso
 * all'arming sarebbero tutte uguali e non ci sarebbe niente da confrontare.
 *
 * ATTENZIONE: un offset sbagliato scrive dentro la struct del PHY. L'indirizzo
 * scrivibile e' solo pi + offset -- `pi` arriva dalla funzione agganciata -- ma
 * l'offset va verificato sul proprio blob: por_inform scrive 1 al flag POR, e
 * quello e' l'offset meno 2.
 */
static int full_init_off;
module_param(full_init_off, int, 0444);
static int force_full_init;
module_param(force_full_init, int, 0644);

#define FIFO_RECS_DEF 131072
static int fifo_recs = FIFO_RECS_DEF;
module_param(fifo_recs, int, 0444);
static DECLARE_KFIFO_PTR(fifo, struct wldiag_rec);
static void *fifo_buf;
static DEFINE_RAW_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(rq);
static atomic_t seq = ATOMIC_INIT(0);
static atomic_t drops = ATOMIC_INIT(0);

/* Letture di REGISTRO PHY da non registrare, per conservare la fifo. Nasce dal
 * polling del rivelatore radar: sui canali DFS il driver interroga 0x0253 e
 * 0x0254 in continuo -- 192000 e 194000 letture nelle quattro fasi -- e con i
 * RETVAL attivi il doppio, senza c'entrare niente con la configurazione del
 * canale. Filtrando QUI, prima della fifo, si conserva il margine; nel decoder
 * non servirebbe, il collo di bottiglia e' la coda.
 *
 *   skipphyrd="0x253,0x254"
 *
 * VALE SOLO PER OP_PHY_R, e non e' pignoleria: gli spazi di indirizzamento sono
 * separati per classe. Nelle stesse catture ci sono 32 OBJ.WR a 0x252 e 32 a
 * 0x254, che sono offset di object memory e non hanno nulla a che vedere coi
 * registri PHY omonimi: un filtro sul solo indirizzo li avrebbe buttati in
 * silenzio.
 *
 * E si filtrano SOLO 0x253/0x254. La testa del blocco -- 0x251 e 0x252, lette
 * 1558 volte in tutto, una per blocco -- e' plausibilmente lo stato e i dati
 * dell'impulso, cioe' la parte che serve: costa poco e si tiene.
 *
 * I record filtrati NON contano come persi: contatore separato, cosi' gli
 * OP_DROP restano un indicatore di perdita vera.
 *
 * Per il DFS servono catture dedicate senza filtro. Il classificatore ETSI/FCC
 * Linux lo ha gia' (dfs_pattern_detector, 377 righe), quindi serve solo il
 * formato di quei registri, non la classificazione.
 */
#define SKIP_MAX 16
static char *skipphyrd;
module_param(skipphyrd, charp, 0444);
static u32 skip_list[SKIP_MAX];
static int skip_n;
static atomic_t filtered = ATOMIC_INIT(0);

static void parse_skipphyrd(void)
{
	char buf[128], *p, *tok;

	if (!skipphyrd || !*skipphyrd)
		return;
	strncpy(buf, skipphyrd, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	p = buf;
	while ((tok = strsep(&p, ",")) && skip_n < SKIP_MAX) {
		unsigned long v;
		char *end;

		while (*tok == ' ')
			tok++;
		if (!*tok)
			continue;
		/* kstrtoul e' arrivata in 2.6.38: simple_strtoul c'e' su entrambi
		 * i kernel e la validita' si controlla sul puntatore di fine. */
		v = simple_strtoul(tok, &end, 0);
		if (end == tok) {
			pr_warn("wl_diag: skipphyrd: '%s' non e' un numero\n", tok);
			continue;
		}
		skip_list[skip_n++] = (u32)v;
	}
	if (skip_n)
		pr_info("wl_diag: %d letture PHY filtrate per indirizzo\n", skip_n);
}

static u32 emit(u8 op, u32 addr, u32 val, u32 aux)
{
	struct wldiag_rec r;
	unsigned long flags;
	int i;

	if (op == OP_PHY_R) {
		for (i = 0; i < skip_n; i++) {
			if (addr == skip_list[i]) {
				atomic_inc(&filtered);
				return 0;
			}
		}
	}

	r.ts_ns = sched_clock();
	r.seq = (u32)atomic_inc_return(&seq);
	r.addr = addr; r.val = val; r.aux = aux;
	r.op = op; r.cpu = (u8)raw_smp_processor_id(); r._pad = 0;

	raw_spin_lock_irqsave(&fifo_lock, flags);
	if (kfifo_avail(&fifo) >= sizeof(r))
		kfifo_in(&fifo, &r, 1);
	else
		atomic_inc(&drops);
	raw_spin_unlock_irqrestore(&fifo_lock, flags);
	wake_up_interruptible(&rq);
	return r.seq;
}

/* ---- tabella hook ----------------------------------------------------- *
 * Ogni campo prende un arg d'ingresso: 0=assente(->0), 1=a1, 2=a2, 3=a3.   *
 * Reg-style: addr=a1(reg), val/mask da a2/a3. GPIO ChipCommon              *
 * (sih,mask,val,prio) non ha reg: addr=0, val=a2, mask(aux)=a1.            */
struct hook {
	const char *name;
	u8 op, addr_src, val_src, aux_src;
	bool shortj;		/* true: detour a 1 parola 'j' (branch nella finestra a 4) */
	bool retcap;		/* true: cattura il valore di ritorno via trampolino ra */
	u8 nargx;		/* # arg extra su stack da catturare: arg5@16(sp), arg6@20(sp) */
	unsigned long addr;
	u32 saved[4];
	bool armed;
	/* Campi di stato aggiunti dopo: DEVONO stare in coda, perche' la tabella
	 * usa inizializzatori posizionali e inserirli in mezzo li sposta tutti.
	 * E' successo: un `true` destinato a retcap finiva in use_bp, retcap
	 * restava falso per ogni hook e non veniva emesso NESSUN RETVAL. */
	bool use_bp;		/* hook via 'break' + die notifier (non detourabile) */
	bool use_sites;		/* patch delle coppie lui/addiu ai siti di chiamata */
	u32 *bp_stub;		/* stub di ripresa del percorso break */
	s16 zero_off;		/* != 0: lo stub azzera il byte pi->[zero_off] */
};
static struct hook hooks[] = {
	{ "phy_reg_read",       OP_PHY_R,     1, 0, 0, .retcap = true },
	{ "phy_reg_write",      OP_PHY_W,     1, 2, 0 },
	{ "phy_reg_mod",        OP_PHY_MOD,   1, 3, 2 },
	/* and/or: reg unico op (addr,val). Op-code distinti cosi' il decoder sa
	 * l'operazione; val=a2 e' la maschera-AND (bit tenuti) risp. il valore-OR
	 * (bit settati). Niente aux: la funzione non ha un 3o argomento. */
	{ "phy_reg_and",        OP_PHY_AND,   1, 2, 0 },
	{ "phy_reg_or",         OP_PHY_OR,    1, 2, 0 },
	{ "write_radio_reg",    OP_RADIO_W,   1, 2, 0 },
	{ "mod_radio_reg",      OP_RADIO_MOD, 1, 3, 2 },
	{ "si_pmu_chipcontrol", OP_PMU_CC,    1, 3, 2, .retcap = true },
	{ "si_pmu_regcontrol",  OP_PMU_RC,    1, 3, 2, .retcap = true },
	{ "si_pmu_pllcontrol",  OP_PMU_PLL,   1, 3, 2, .retcap = true },
	/* si_corereg(sih, coreidx, regoff, mask, val): accesso generico a un
	 * registro di un core del backplane. addr=regoff(a2), aux=coreidx(a1).
	 * val (a4, 5o arg) e' sullo stack in o32 -> catturato via nargx (record
	 * ARGX di continuazione). retcap: il ritorno (read/rmw) va nel RETVAL. */
	{ "si_corereg",         OP_SI_COREREG,2, 0, 1, .retcap = true, .nargx = 1 },
	/* ChipCommon GPIO (sih, mask, val, prio): mask=a1, val=a2 */
	{ "si_gpiocontrol",     OP_CC_GPIOCTL,0, 2, 1 },
	{ "si_gpioout",         OP_CC_GPIOOUT,0, 2, 1 },
	{ "si_gpioouten",       OP_CC_GPIOOE, 0, 2, 1 },
	/* accesso tabella acphy (pi, id, len, off, width, data): id=a1, len=a2,
	 * off=a3. width/data sono args di stack, non catturati. Verifica l'ordine
	 * len/off sui tuoi header: il disasm fissa id=a1 ma non len-vs-off. */
	{ "wlc_phy_table_read_acphy",  OP_TBL_R, 1, 2, 3 },
	{ "wlc_phy_table_write_acphy", OP_TBL_W, 1, 2, 3 },
	{ "osl_delay",          OP_DELAY,     0, 1, 0 }, /* usec=a1 */
	/* Controllo verso il MAC (core d11). MACCONTROL RMW + MAC host-flags.
	 * Firme dedotte dal ramo brcmsmac (mirror del wl proprietario) --
	 * da riverificare sul disasm come per gli altri hook (cfr. il caveat
	 * len/off di wlc_phy_table_*). Se un prologo ha un branch nelle prime
	 * 4 parole, wd_init lo salta con un pr_warn: nessun rischio.
	 *   wlc_bmac_mctrl(hw, u32 mask, u32 val)   reg fisso: mask=a1, val=a2
	 *   wlc_bmac_mhf(hw, u8 idx, u16 mask, u16 val, int bands)
	 *                                           idx=a1, mask=a2, val=a3
	 *   wlc_bmac_mhf_get(hw, u8 idx, int bands) idx=a1 (val UNDEFINED) */
	{ "wlc_bmac_mctrl",     OP_MAC_MCTRL, 0, 2, 1 },
	{ "wlc_bmac_mhf",       OP_MAC_MHF_W, 1, 3, 2 },
	{ "wlc_bmac_mhf_get",   OP_MAC_MHF_R, 1, 0, 0, .retcap = true },
	/* Object memory del MAC (SHM, SCR, IHR): addr=offset, aux=selettore.
	 * Cattura anche il campione di rumore della crs_min_pwr cal, che passa da
	 * wlc_phy_noise_read_shmem -> wlapi_bmac_read_shm -> wlc_bmac_read_shm ->
	 * qui, non da un registro PHY.
	 * NOME PER VERSIONE: read_objmem su 6.30, read_objmem16 su 7.14.
	 * Non si aggancia read_shm: e' un wrapper con jr alla parola 2. */
	/* Cambio canale: chanspec in a1. Si aggancia la generica, che scatta per
	 * ogni PHY e permette una run unica su piu' canali da splittare dopo. */
	/* Template RAM: dove il PHY carica le forme d'onda dei toni, ingresso di
	 * RXIQ, PAPD e do_dummy_tx. Nessuna classe di op la copriva.
	 * Su 6.30 gli accessor ptr/data non esistono: la' solo il bulk. */
	{ "wlc_bmac_templateptr_wreg",  OP_TPL_PTRW, 1, 0, 0 },
	{ "wlc_bmac_templatedata_wreg", OP_TPL_DATW, 1, 0, 0 },
	{ "wlc_bmac_templateptr_rreg",  OP_TPL_PTRR, 0, 0, 0, .retcap = true },
	{ "wlc_bmac_templatedata_rreg", OP_TPL_DATR, 0, 0, 0, .retcap = true },
	{ "wlc_bmac_write_template_ram", OP_TPL_RAMW, 1, 2, 3 },
	/* OTP: il livello generico ha gli stessi nomi su 6.30 e 7.14 e prologo
	 * pulito, mentre gli hndotp_ e ipxotp_ cambiano. Il contenuto e' l'immagine
	 * SROM, statica e gia' nota dai dump: serve per sapere QUANDO viene letta
	 * e QUALI word, cioe' dove i valori vengono consumati.
	 *   otp_read_word(oh, wn, *data)              wn=a1
	 *   otp_read_region(sih, region, *data, *len) region=a1
	 *   otp_init(sih)                             solo il momento */
	{ "otp_init",        OP_OTP_INIT, 0, 0, 0, .retcap = true },
	{ "otp_read_word",   OP_OTP_RDW,  1, 0, 2, .retcap = true },
	{ "otp_read_region", OP_OTP_RDR,  1, 0, 3, .retcap = true },
	/* Due accessor che il codice acphy chiama e che il port NON fa affatto --
	 * zero riferimenti a bw_set o sromctl in src/. Il grafo delle chiamate
	 * dice anche DOVE vanno:
	 *
	 *   wlc_bmac_bw_set     <- wlapi_bmac_bw_set <- wlc_phy_chanspec_set_acphy
	 *                                            <- wlc_phy_init
	 *     larghezza al livello MAC. Serve per 40 e 80 MHz: con solo BW20 il
	 *     default passa inosservato. Va in set_channel e in op_init.
	 *   si_get/set_sromctl  <- wlc_phy_attach_acphy
	 *     registro di controllo SROM. Va nel punto di op_init che corrisponde
	 *     ad attach_acphy.
	 *
	 * Firme dedotte dal prologo (argomenti intatti in a0-a3):
	 *   wlc_bmac_bw_set(hw, bw)        bw=a1, non e' un indirizzo -> val
	 *   si_get_sromctl(sih)            valore nel RETVAL
	 *   si_set_sromctl(sih, val)       val=a1
	 *
	 * NON si aggancia wlc_bmac_macphyclk_set: i suoi chiamanti sono
	 * init_htphy, init_nphy e wlc_bmac_init, quindi per l'AC-PHY non e' nel
	 * percorso. Ha anche un bne alla parola 1, ma e' irrilevante. */
	/* Solo per forzare la cal completa: il record serve a vedere quando
	 * cal_init viene invocata, e lo stub azzera pi->[full_init_off] se il
	 * parametro e' impostato. Prologo: addiu sp / sw s0 / sw ra / lbu 251(a0),
	 * quindi il branch e' alla parola 4 e il detour a 4 parole regge. La
	 * parola 3 riesegue la lbu DOPO l'azzeramento, quindi legge 0. */
	{ "wlc_phy_cal_init", OP_CAL_INIT,   0, 0, 0 },
	{ "wlc_bmac_bw_set",  OP_MAC_BW,     0, 1, 0 },
	{ "si_get_sromctl",   OP_SROMCTL_R,  0, 0, 0, .retcap = true },
	{ "si_set_sromctl",   OP_SROMCTL_W,  0, 1, 0 },
	{ "wlc_phy_chanspec_set", OP_CHANSPEC, 1, 0, 0 },
	{ "wlc_bmac_read_objmem16",  OP_MAC_OBJ_R, 1, 0, 2, .retcap = true },
	{ "wlc_bmac_write_objmem16", OP_MAC_OBJ_W, 1, 2, 3 },
	/* branch a slot 3 (beq): detour classico a 4 parole impossibile. short-j a
	 * 1 parola: o[0]=j stub; o[1] (addiu $v0,1) resta come delay slot; lo stub
	 * riesegue o[0..1] e rientra a +8 (v0 ri-settato DOPO la hook). addr=a1
	 * grezzo (l'andi 0xffff e' o[0], rieseguito nello stub). */
	{ "read_radio_reg",     OP_RADIO_R,   1, 0, 0, .shortj = true, .retcap = true },
};
#define NHOOK ARRAY_SIZE(hooks)

/* Punto d'atterraggio del detour: chiamato dallo stub con (id, a1, a2, a3). */
static inline u32 pick(u8 src, u32 a1, u32 a2, u32 a3)
{
	return src == 1 ? a1 : src == 2 ? a2 : src == 3 ? a3 : 0;
}
u32 __used noinline
wl_diag_hook(u32 id, u32 a1, u32 a2, u32 a3)
{
	struct hook *h = &hooks[id];

	return emit(h->op, pick(h->addr_src, a1, a2, a3),
			   pick(h->val_src,  a1, a2, a3),
			   pick(h->aux_src,  a1, a2, a3));
}

/* Record di continuazione per gli arg su stack (o32): un secondo record ARGX
 * legato al principale via parent_seq. addr=arg5, val=arg6. */
void __used noinline
wl_diag_hook_argx(u32 parent_seq, u32 x1, u32 x2)
{
	emit(OP_ARGX, x1, x2, parent_seq);
}

/* ---- cattura valore di ritorno (retcap): trampolino su 'ra' ------------ *
 * L'origine di 'ra' e' salvata PER-INVOCAZIONE in un pool indicizzato da     *
 * 'current' (task): sopravvive a preemption/migrazione (SMP+PREEMPT) al       *
 * contrario di uno slot per-CPU. LIFO per gestire il nesting (una read        *
 * agganciata che ne chiama un'altra). Pool pieno -> NON dirotta (nessun       *
 * crash, si perde solo quel valore). */
struct ret_inst {
	struct task_struct *task;
	unsigned long orig_ra;
	u32 seq;
	u32 order;
};
#define RET_POOL 64
static struct ret_inst ret_pool[RET_POOL];
static DEFINE_RAW_SPINLOCK(ret_lock);
static u32 ret_order;
static unsigned long ret_trampoline;	/* indirizzo dello stub di ritorno condiviso */

/* ingresso di un retcap: registra (current, orig_ra, seq); ritorna l'indirizzo
 * di 'ra' da installare (trampolino se c'e' posto, altrimenti orig_ra). */
unsigned long __used noinline
wl_diag_enter_ret(unsigned long orig_ra, u32 seq)
{
	unsigned long f;
	int i;

	if (!ret_trampoline)
		return orig_ra;
	raw_spin_lock_irqsave(&ret_lock, f);
	for (i = 0; i < RET_POOL; i++) {
		if (!ret_pool[i].task) {
			ret_pool[i].task = current;
			ret_pool[i].orig_ra = orig_ra;
			ret_pool[i].seq = seq;
			ret_pool[i].order = ++ret_order;
			raw_spin_unlock_irqrestore(&ret_lock, f);
			return ret_trampoline;
		}
	}
	raw_spin_unlock_irqrestore(&ret_lock, f);
	return orig_ra;
}

/* ritorno di un retcap: preleva LIFO l'istanza di current, emette RETVAL(seq,
 * retval) e ritorna orig_ra. Chiamata solo se enter aveva dirottato -> per
 * costruzione l'istanza esiste; guardia difensiva se best<0. */
unsigned long __used noinline
wl_diag_exit_ret(u32 retval)
{
	unsigned long f, ra = 0;
	int i, best = -1;
	u32 bestord = 0, seq = 0;

	raw_spin_lock_irqsave(&ret_lock, f);
	for (i = 0; i < RET_POOL; i++)
		if (ret_pool[i].task == current && ret_pool[i].order >= bestord) {
			bestord = ret_pool[i].order;
			best = i;
		}
	if (best >= 0) {
		ra = ret_pool[best].orig_ra;
		seq = ret_pool[best].seq;
		ret_pool[best].task = NULL;
	}
	raw_spin_unlock_irqrestore(&ret_lock, f);
	if (best >= 0)
		emit(OP_RETVAL, seq, retval, 0);
	return ra;
}

/* ---- mini-assembler MIPS o32 (codifiche verificate) ------------------- */
#define R_ZERO 0
#define R_V0 2
#define R_V1 3
#define R_A0 4
#define R_A1 5
#define R_A2 6
#define R_A3 7
#define R_T8 24
#define R_T9 25
#define R_SP 29
#define R_RA 31
static inline u32 i_addiu(u8 rt, u8 rs, s16 im){ return (0x09u<<26)|(rs<<21)|(rt<<16)|(u16)im; }
static inline u32 i_sw(u8 rt, u8 b, s16 o){ return (0x2bu<<26)|(b<<21)|(rt<<16)|(u16)o; }
static inline u32 i_lw(u8 rt, u8 b, s16 o){ return (0x23u<<26)|(b<<21)|(rt<<16)|(u16)o; }
static inline u32 i_sb(u8 rt, u8 b, s16 o){ return (0x28u<<26)|(b<<21)|(rt<<16)|(u16)o; }
/* beq rs,rt,off: off in ISTRUZIONI, relativo alla parola dopo il delay slot */
static inline u32 i_beq(u8 rs, u8 rt, s16 off){ return (0x04u<<26)|(rs<<21)|(rt<<16)|(u16)off; }
static inline u32 i_lui(u8 rt, u16 im){ return (0x0fu<<26)|(rt<<16)|im; }
static inline u32 i_ori(u8 rt, u8 rs, u16 im){ return (0x0du<<26)|(rs<<21)|(rt<<16)|im; }
static inline u32 i_jalr(u8 rs){ return (rs<<21)|(R_RA<<11)|0x09u; }
static inline u32 i_jr(u8 rs){ return (rs<<21)|0x08u; }
static inline u32 i_j(unsigned long tgt){ return (0x02u<<26)|(u32)((tgt>>2)&0x03ffffffu); }
#define I_NOP 0u

/* true se l'opcode e' un branch/jump (non rilocabile verbatim nello stub) */
static bool is_branch(u32 insn)
{
	u32 op = insn >> 26;
	if (op == 0) {           /* SPECIAL: jr/jalr */
		u32 f = insn & 0x3f;
		return f == 0x08 || f == 0x09;
	}
	if (op == 0x01) return true;            /* REGIMM: bltz/bgez/...  */
	if (op == 0x02 || op == 0x03) return true; /* j / jal             */
	if (op >= 0x04 && op <= 0x07) return true; /* beq/bne/blez/bgtz   */
	if (op == 0x14 || op == 0x15 ||
	    op == 0x16 || op == 0x17) return true;  /* beql/bnel/...       */
	return false;
}

/* ---- pool stub eseguibile (statico: vive nel modulo, mai liberato) ----- */
#define STUB_WORDS 48
static u32 stub_pool[NHOOK][STUB_WORDS] __attribute__((aligned(8)));
static u32 ret_tramp[16] __attribute__((aligned(8)));	/* trampolino di ritorno condiviso */

/* Percorso a 'break' per prologhi non detourabili (branch nella finestra).
 * Una parola, nessun delay slot. do_bp() chiama notify_die(DIE_BREAK) per
 * BRK_KPROBE_BP fuori da CONFIG_KPROBES, quindi basta un die notifier; con
 * NOTIFY_STOP non si arriva al die_if_kernel. Verificato su Linux 3.4.
 * Su 2.6.30 non esiste (do_bp -> do_trap_or_bp, set_except_vector non
 * esportata): BRK_KPROBE_BP fa da discriminante e il percorso si compila via.
 * La parola 0 viene rieseguita in uno stub, quindi non puo' essere
 * PC-relative: si verifica prima di armare. */
#ifdef BRK_KPROBE_BP
#define WD_HAVE_BP 1

static bool bp_registered;	/* die notifier registrato */

#define BP_INSN (0x0000000dU | (BRK_KPROBE_BP << 6))	/* break BRK_KPROBE_BP */

static int wd_bp_notify(struct notifier_block *nb, unsigned long val, void *data)
{
	struct die_args *args = data;
	struct pt_regs *regs;
	int i;

	if (val != DIE_BREAK || !args || !(regs = args->regs))
		return NOTIFY_DONE;

	for (i = 0; i < (int)ARRAY_SIZE(hooks); i++) {
		u32 seq;

		if (!hooks[i].use_bp || !hooks[i].armed)
			continue;
		if (regs->cp0_epc != hooks[i].addr)
			continue;

		/* o32: a1..a3 sono $a1..$a3 = regs[5..7] */
		seq = wl_diag_hook((u32)i, (u32)regs->regs[5],
				   (u32)regs->regs[6], (u32)regs->regs[7]);
		if (hooks[i].retcap)
			regs->regs[31] =
				wl_diag_enter_ret(regs->regs[31], seq);

		regs->cp0_epc = (unsigned long)hooks[i].bp_stub;
		return NOTIFY_STOP;
	}
	return NOTIFY_DONE;
}

static struct notifier_block wd_bp_nb = {
	.notifier_call = wd_bp_notify,
	.priority = 0x7fffffff,		/* prima di eventuali altri consumatori */
};

/* stub: [0] parola originale, [1] j func+4, [2] nop */
static void build_bp_stub(int idx)
{
	u32 *s = stub_pool[idx];
	unsigned long ret = hooks[idx].addr + 4;

	s[0] = hooks[idx].saved[0];
	s[1] = 0x08000000U | ((ret >> 2) & 0x03ffffffU);	/* j ret */
	s[2] = 0x00000000U;					/* nop */
	hooks[idx].bp_stub = s;
}
#else
#define WD_HAVE_BP 0
#endif

/* Patch dei siti di chiamata. Il modulo e' -mabicalls: zero jal in .text, le
 * chiamate sono lui/addiu + jalr (o jr $t9 per le tail call), quindi si
 * riscrive la coppia perche' carichi lo stub. La funzione resta intatta:
 * nessun vincolo sul prologo, e funziona su 2.6.30 dove il break non c'e'.
 * Un solo stub serve jalr e jr: preserva ra e salta alla funzione vera.
 * Tre condizioni, verificate a runtime: la coppia deve dare l'indirizzo
 * esatto; deve seguirla un salto sullo STESSO registro; l'addiu non deve
 * essere condiviso (il compilatore riusa la parte bassa fra siti diversi:
 * nel blob D6220 quello a +0x1f5a24 serve due funzioni). */
#define MAX_SITES 8

struct site {
	u32 *hi, *lo;
	u32 saved_hi, saved_lo;
};
static struct site sites[NHOOK][MAX_SITES];
static int n_sites[NHOOK];

/* immediati di una coppia lui/addiu per caricare `v`: l'addiu estende il segno
 * della parte bassa, quindi la parte alta va compensata. */
static inline u16 hi16_of(unsigned long v) { return (u16)((v + 0x8000UL) >> 16); }
static inline u16 lo16_of(unsigned long v) { return (u16)(v & 0xffff); }

static int find_sites(int idx, unsigned long target)
{
	struct module *m = __module_text_address(target);
	u32 *base;
	unsigned long words;
	int n = 0, i, k;

	if (!m) {
		pr_warn("wl_diag: '%s' non e' in un modulo, niente scansione siti\n",
			hooks[idx].name);
		return 0;
	}
	base = (u32 *)m->module_core;
	words = m->core_text_size / 4;

	for (i = 0; i + 1 < (int)words && n < MAX_SITES; i++) {
		u32 wi = base[i];
		int rt;

		if ((wi >> 26) != 0x0f)			/* lui */
			continue;
		rt = (wi >> 16) & 31;

		for (k = 1; k <= 8 && i + k < (int)words; k++) {
			u32 wl = base[i + k];
			unsigned long a;
			int j, found = 0;

			if ((wl >> 26) != 0x09)		/* addiu */
				continue;
			if (((wl >> 21) & 31) != rt || ((wl >> 16) & 31) != rt)
				continue;
			a = ((unsigned long)(wi & 0xffff) << 16) +
			    (long)(s16)(wl & 0xffff);
			if (a != target)
				break;
			/* salto sullo stesso registro entro 8 istruzioni */
			for (j = 1; j <= 8 && i + k + j < (int)words; j++) {
				u32 wj = base[i + k + j];

				if ((wj >> 26) != 0 || ((wj >> 21) & 31) != rt)
					continue;
				if ((wj & 0x3f) == 0x08 || (wj & 0x3f) == 0x09) {
					found = 1;
					break;
				}
			}
			if (!found) {
				pr_info("wl_diag: '%s' sito @%px senza salto, ignorato\n",
					hooks[idx].name, &base[i]);
				break;
			}
			sites[idx][n].hi = &base[i];
			sites[idx][n].lo = &base[i + k];
			sites[idx][n].saved_hi = wi;
			sites[idx][n].saved_lo = wl;
			n++;
			break;
		}
	}

	/* scarta i siti che condividono l'addiu con un altro */
	for (i = 0; i < n; i++) {
		int shared = 0;

		for (k = 0; k < n; k++)
			if (k != i && sites[idx][k].lo == sites[idx][i].lo)
				shared = 1;
		if (shared) {
			pr_warn("wl_diag: '%s' sito @%px scartato (addiu condiviso @%px)\n",
				hooks[idx].name, sites[idx][i].hi, sites[idx][i].lo);
			sites[idx][i] = sites[idx][--n];
			i--;
		}
	}
	n_sites[idx] = n;
	return n;
}

static void patch_sites(int idx)
{
	unsigned long s = (unsigned long)stub_pool[idx];
	int i;

	for (i = 0; i < n_sites[idx]; i++) {
		*sites[idx][i].hi = (sites[idx][i].saved_hi & 0xffff0000) |
				    hi16_of(s);
		*sites[idx][i].lo = (sites[idx][i].saved_lo & 0xffff0000) |
				    lo16_of(s);
		flush_i((unsigned long)sites[idx][i].hi,
			(unsigned long)sites[idx][i].hi + 4);
		flush_i((unsigned long)sites[idx][i].lo,
			(unsigned long)sites[idx][i].lo + 4);
	}
}

static void restore_sites(int idx)
{
	int i;

	for (i = 0; i < n_sites[idx]; i++) {
		*sites[idx][i].hi = sites[idx][i].saved_hi;
		*sites[idx][i].lo = sites[idx][i].saved_lo;
		flush_i((unsigned long)sites[idx][i].hi,
			(unsigned long)sites[idx][i].hi + 4);
		flush_i((unsigned long)sites[idx][i].lo,
			(unsigned long)sites[idx][i].lo + 4);
	}
	n_sites[idx] = 0;
}

/* Se il modulo bersaglio se ne va mentre siamo armati, i nostri puntatori
 * restano su memoria liberata e il ripristino allo scarico scriverebbe li'.
 * Su 2.6.30 il rescan PCI scarica wl, quindi succede davvero.
 * Si abbandonano le patch senza toccare la memoria: il modulo sta per sparire. */
static struct module *target_mod;
static bool mod_nb_registered;
static bool mod_ref_held;

/* Tenere un riferimento sul bersaglio: finche' siamo armati `rmmod wl` fallisce
 * con -EBUSY, e le patch non finiscono su memoria liberata. remove/probe del
 * device continuano a funzionare: il refcount del modulo non c'entra con la
 * presenza della funzione PCI -- ed e' per questo che su 3.4 wl resta caricato
 * quando si rimuove il device. Su 2.6.30 e' lo spazio utente del vendor a fare
 * rmmod al rescan, e il riferimento glielo impedisce.
 * try_module_get non e' esportata su 2.6.30; __module_get e' una static inline
 * che incrementa direttamente. Non controlla MODULE_STATE_GOING, ma qui il
 * modulo e' vivo: gli abbiamo appena risolto i simboli. */
static void target_ref_get(struct module *m)
{
	if (!m)
		return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 0, 0)
	mod_ref_held = try_module_get(m);
#else
	__module_get(m);
	mod_ref_held = true;
#endif
	if (!mod_ref_held)
		pr_warn("wl_diag: nessun riferimento sul bersaglio: se si scarica "
			"mentre siamo armati, gli hook vengono abbandonati\n");
}

static int wd_mod_notify(struct notifier_block *nb, unsigned long ev, void *data)
{
	struct module *m = data;
	int i;

	if (ev != MODULE_STATE_GOING || !target_mod || m != target_mod)
		return NOTIFY_DONE;

	for (i = 0; i < NHOOK; i++)
		hooks[i].armed = false;
	mod_ref_held = false;
	target_mod = NULL;
	pr_warn("wl_diag: il modulo bersaglio si sta scaricando: hook abbandonati "
		"senza ripristino. Ricaricare wl_diag dopo il suo re-insmod.\n");
	return NOTIFY_DONE;
}

static struct notifier_block wd_mod_nb = { .notifier_call = wd_mod_notify };

static void build_stub(int idx)
{
	u32 *s = stub_pool[idx];
	u32 *o = (u32 *)hooks[idx].addr;
	unsigned long hookfn = (unsigned long)&wl_diag_hook;
	unsigned long argxfn = (unsigned long)&wl_diag_hook_argx;
	unsigned long enterfn = (unsigned long)&wl_diag_enter_ret;
	/* sui siti la funzione e' intatta: niente da rieseguire, si rientra da 0 */
	int rep = hooks[idx].use_sites ? 0 : (hooks[idx].shortj ? 2 : 4);
	unsigned long ret = hooks[idx].use_sites ? hooks[idx].addr :
		hooks[idx].addr + (hooks[idx].shortj ? 8 : 16);
	int n = 0, k;

	s[n++] = i_addiu(R_SP, R_SP, -32);
	s[n++] = i_sw(R_A0, R_SP, 0);
	s[n++] = i_sw(R_A1, R_SP, 4);
	s[n++] = i_sw(R_A2, R_SP, 8);
	s[n++] = i_sw(R_A3, R_SP, 12);
	s[n++] = i_sw(R_RA, R_SP, 16);
	/* wl_diag_hook(idx, a1, a2, a3) -> v0 = seq */
	s[n++] = i_ori(R_A0, R_ZERO, (u16)idx);
	s[n++] = i_lui(R_T9, hookfn >> 16);
	s[n++] = i_ori(R_T9, R_T9, hookfn & 0xffff);
	s[n++] = i_jalr(R_T9);
	s[n++] = I_NOP;
	s[n++] = i_sw(R_V0, R_SP, 20);		/* seq */

	/* Arg extra su stack (o32): arg5@16(entry)=48(sp), arg6@20=52(sp).
	 * wl_diag_hook_argx(seq, arg5, arg6) -> record ARGX di continuazione. */
	if (hooks[idx].nargx) {
		s[n++] = i_lw(R_A0, R_SP, 20);			/* seq */
		s[n++] = i_lw(R_A1, R_SP, 48);			/* arg5 */
		if (hooks[idx].nargx >= 2)
			s[n++] = i_lw(R_A2, R_SP, 52);		/* arg6 */
		else
			s[n++] = i_ori(R_A2, R_ZERO, 0);
		s[n++] = i_lui(R_T9, argxfn >> 16);
		s[n++] = i_ori(R_T9, R_T9, argxfn & 0xffff);
		s[n++] = i_jalr(R_T9);
		s[n++] = I_NOP;
	}

	/* retcap: wl_diag_enter_ret(orig_ra, seq) -> v0 = ra da installare
	 * (trampolino se c'e' posto, altrimenti orig_ra = nessun dirottamento). */
	if (hooks[idx].retcap) {
		s[n++] = i_lw(R_A0, R_SP, 16);			/* orig_ra */
		s[n++] = i_lw(R_A1, R_SP, 20);			/* seq */
		s[n++] = i_lui(R_T9, enterfn >> 16);
		s[n++] = i_ori(R_T9, R_T9, enterfn & 0xffff);
		s[n++] = i_jalr(R_T9);
		s[n++] = I_NOP;
		s[n++] = i_sw(R_V0, R_SP, 24);			/* ra da installare */
	}

	s[n++] = i_lw(R_A0, R_SP, 0);
	s[n++] = i_lw(R_A1, R_SP, 4);
	s[n++] = i_lw(R_A2, R_SP, 8);
	s[n++] = i_lw(R_A3, R_SP, 12);
	s[n++] = i_lw(R_RA, R_SP, hooks[idx].retcap ? 24 : 16);
	s[n++] = i_addiu(R_SP, R_SP, 32);
	/* $a0 e' il primo argomento della funzione agganciata, cioe' `pi`.
	 * L'interruttore si rilegge a OGNI chiamata, non si cuoce all'arming:
	 *   lui/lw  t8 <- force_full_init
	 *   beq     t8, zero -> salta la sb
	 *   sb      zero, off(a0)
	 * Il delay slot del beq porta la sb stessa, quindi con t8 == 0 si salta a
	 * dopo di essa e non viene eseguita. */
	if (hooks[idx].zero_off) {
		unsigned long fp = (unsigned long)&force_full_init;

		/* La parte bassa del lw e' con segno, quindi la parte alta va
		 * compensata: la stessa aritmetica del patching dei siti. */
		s[n++] = i_lui(R_T8, (u16)((fp + 0x8000) >> 16));
		s[n++] = i_lw(R_T8, R_T8, (s16)(fp & 0xffff));
		/* Il delay slot si esegue SEMPRE, quindi ci va un nop e non la sb:
		 * altrimenti la scrittura avverrebbe anche a interruttore spento.
		 * Bersaglio del beq = (indirizzo del delay slot) + off*4, cioe' con
		 * off=2 si salta la parola dopo la sb. */
		s[n++] = i_beq(R_T8, R_ZERO, 2);
		s[n++] = I_NOP;
		s[n++] = i_sb(R_ZERO, R_A0, hooks[idx].zero_off);
	}
	for (k = 0; k < rep; k++)
		s[n++] = o[k];	/* riesegue le parole spiazzate (o[1] short-j ri-setta v0) */
	s[n++] = i_lui(R_T9, ret >> 16);
	s[n++] = i_ori(R_T9, R_T9, ret & 0xffff);
	s[n++] = i_jr(R_T9);
	s[n++] = I_NOP;
	/* max (classic+retcap+nargx+forzatura) == 45 <= STUB_WORDS */
}

/* Trampolino di ritorno condiviso: la funzione agganciata retcap fa jr ra con
 * ra == qui. Legge $v0 (valore restituito), lo consegna a wl_diag_exit_ret che
 * emette RETVAL e ritorna orig_ra, quindi salta a orig_ra con $v0 preservato. */
static void build_ret_trampoline(void)
{
	unsigned long exitfn = (unsigned long)&wl_diag_exit_ret;
	u32 *s = ret_tramp;
	int n = 0;

	s[n++] = i_addiu(R_SP, R_SP, -32);
	s[n++] = i_sw(R_V0, R_SP, 0);
	s[n++] = i_sw(R_V1, R_SP, 4);
	s[n++] = i_ori(R_A0, R_V0, 0);			/* a0 = retval (move) */
	s[n++] = i_lui(R_T9, exitfn >> 16);
	s[n++] = i_ori(R_T9, R_T9, exitfn & 0xffff);
	s[n++] = i_jalr(R_T9);
	s[n++] = I_NOP;
	s[n++] = i_ori(R_T8, R_V0, 0);			/* t8 = orig_ra */
	s[n++] = i_lw(R_V0, R_SP, 0);
	s[n++] = i_lw(R_V1, R_SP, 4);
	s[n++] = i_addiu(R_SP, R_SP, 32);
	s[n++] = i_jr(R_T8);
	s[n++] = I_NOP;
}

/* scrive l'ingresso, parola di testa PER ULTIMA (classic) o singola 'j' (short-j) */
static void patch_entry(int idx)
{
	u32 *o = (u32 *)hooks[idx].addr;
	unsigned long stub = (unsigned long)stub_pool[idx];

	if (hooks[idx].shortj) {
		/* patch a 1 parola atomica: o[0]=j stub. o[1] resta (delay slot,
		 * rieseguito anche dallo stub). Richiede stub in regione j 256MB
		 * (verificato in wd_init). */
		o[0] = i_j(stub);
		flush_i(hooks[idx].addr, hooks[idx].addr + 8);
		return;
	}
	o[3] = I_NOP;
	o[2] = i_jr(R_T9);
	o[1] = i_ori(R_T9, R_T9, stub & 0xffff);
	wmb();
	o[0] = i_lui(R_T9, stub >> 16);
	flush_i(hooks[idx].addr, hooks[idx].addr + 16);
}

static void restore_entry(int idx)
{
	u32 *o = (u32 *)hooks[idx].addr;
	u32 *sv = hooks[idx].saved;

	if (hooks[idx].shortj) {
		o[0] = sv[0];
		flush_i(hooks[idx].addr, hooks[idx].addr + 8);
		return;
	}
	o[1] = sv[1]; o[2] = sv[2]; o[3] = sv[3];
	wmb();
	o[0] = sv[0];
	flush_i(hooks[idx].addr, hooks[idx].addr + 16);
}

/* ---- char device ------------------------------------------------------ */
static ssize_t wd_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	struct wldiag_rec r;
	unsigned long flags;
	u32 d;
	int ret;

	if (len < sizeof(r))
		return -EINVAL;

	d = atomic_xchg(&drops, 0);
	if (d) {
		memset(&r, 0, sizeof(r));
		r.ts_ns = sched_clock();
		r.op = OP_DROP;
		r.aux = d;
		if (copy_to_user(ubuf, &r, sizeof(r)))
			return -EFAULT;
		return sizeof(r);
	}

	for (;;) {
		raw_spin_lock_irqsave(&fifo_lock, flags);
		ret = kfifo_out(&fifo, &r, 1);
		raw_spin_unlock_irqrestore(&fifo_lock, flags);
		if (ret)
			break;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(rq,
				!kfifo_is_empty(&fifo) || atomic_read(&drops)))
			return -ERESTARTSYS;
		if (atomic_read(&drops))
			return 0;
	}
	if (copy_to_user(ubuf, &r, sizeof(r)))
		return -EFAULT;
	return sizeof(r);
}

static unsigned int wd_poll(struct file *f, poll_table *wait)
{
	poll_wait(f, &rq, wait);
	if (!kfifo_is_empty(&fifo) || atomic_read(&drops))
		return POLLIN | POLLRDNORM;
	return 0;
}

static const struct file_operations wd_fops = {
	.owner = THIS_MODULE,
	.read = wd_read,
	.poll = wd_poll,
	.llseek = no_llseek,
};
/* Il buffer sta in /proc/wl_diag: appare da se' e non serve mknod. La strada
 * precedente era un misc device a minor dinamico, che voleva leggere il minor
 * da /proc/misc e crearlo a mano a ogni caricamento.
 * proc_create ha la stessa firma su 2.6.30 e 3.4 e prende file_operations,
 * quindi la stessa chiamata vale per entrambi. */
#define WD_PROC "wl_diag"

/* ---- init/exit -------------------------------------------------------- */
static int eligible[NHOOK];   /* indici agganciabili */
static int n_elig;

static int __init wd_init(void)
{
	int i, err;

	parse_skipphyrd();

	if (full_init_off) {
		for (i = 0; i < NHOOK; i++)
			if (hooks[i].op == OP_CAL_INIT)
				hooks[i].zero_off = (s16)full_init_off;
		pr_info("wl_diag: cal_init puo' azzerare pi->[%d]; interruttore in "
			"/sys/module/wl_diag/parameters/force_full_init (ora %d)\n",
			full_init_off, force_full_init);
	}

	n_elig = 0;
	for (i = 0; i < NHOOK; i++) {
		unsigned long a;
		u32 *o;
		int j, win, branch = -1;

		if (hooks[i].op == OP_DELAY && !delay) {
			pr_info("wl_diag: '%s' staccato (delay=0)\n",
				hooks[i].name);
			continue;
		}

		a = kallsyms_lookup_name(hooks[i].name);
		if (!a) {
			pr_warn("wl_diag: '%s' non trovato (wl caricato?)\n",
				hooks[i].name);
			continue;
		}
		hooks[i].addr = a;
		o = (u32 *)a;
		win = hooks[i].shortj ? 2 : 4;	/* parole toccate/riesguite */
		for (j = 0; j < 4; j++)
			hooks[i].saved[j] = o[j];
		for (j = 0; j < win; j++)
			if (branch < 0 && is_branch(o[j]))
				branch = j;
		if (branch >= 0 && find_sites(i, hooks[i].addr) > 0) {
			/* Non detourabile nel prologo, ma i siti di chiamata sono
			 * patchabili: la funzione resta intatta. Preferito al break,
			 * che richiede il die notifier e non c'e' su ogni kernel. */
			hooks[i].use_sites = true;
			eligible[n_elig++] = i;
			pr_info("wl_diag: piano hook '%s' @%px [siti: %d] (branch a istr %d)\n",
				hooks[i].name, o, n_sites[i], branch);
			continue;
		}
		if (branch >= 0) {
			/* Non detourabile. Il percorso a 'break' serve una parola sola
			 * e non ha delay slot, ma la parola 0 va rieseguita nello stub
			 * quindi non puo' essere PC-relative. */
			if (WD_HAVE_BP && !is_branch(o[0])) {
				hooks[i].use_bp = true;
				eligible[n_elig++] = i;
				pr_info("wl_diag: piano hook '%s' @%px [break] (branch a istr %d)\n",
					hooks[i].name, o, branch);
			} else if (WD_HAVE_BP) {
				pr_warn("wl_diag: salto '%s' (branch a istr %d e parola 0 non rieseguibile)\n",
					hooks[i].name, branch);
			} else {
				pr_warn("wl_diag: salto '%s' (branch a istr %d; percorso break non disponibile su questo kernel)\n",
					hooks[i].name, branch);
			}
			continue;
		}
		if (hooks[i].shortj &&
		    (((unsigned long)stub_pool[i] ^ a) >> 28)) {
			pr_warn("wl_diag: salto '%s' (stub fuori regione j 256MB)\n",
				hooks[i].name);
			continue;
		}
		eligible[n_elig++] = i;
		pr_info("wl_diag: piano hook '%s' @%px%s\n", hooks[i].name, o,
			hooks[i].shortj ? " [short-j]" : "");
	}

	if (!n_elig) {
		pr_err("wl_diag: nessuna funzione agganciabile\n");
		return -ENODEV;
	}

	if (fifo_recs < 4096) {
		pr_warn("wl_diag: fifo_recs=%d troppo piccolo, uso 4096\n", fifo_recs);
		fifo_recs = 4096;
	}
	fifo_recs = 1 << (fls(fifo_recs) - 1);	/* potenza di 2 per difetto */
	fifo_buf = vmalloc(fifo_recs * sizeof(struct wldiag_rec));
	if (!fifo_buf) {
		pr_err("wl_diag: vmalloc di %d KB per la coda fallita. "
		       "Riprovare con fifo_recs piu' basso.\n",
		       (int)(fifo_recs * sizeof(struct wldiag_rec) / 1024));
		return -ENOMEM;
	}
	err = kfifo_init(&fifo, fifo_buf, fifo_recs * sizeof(struct wldiag_rec));
	if (err) {
		pr_err("wl_diag: kfifo_init: %d\n", err);
		vfree(fifo_buf);
		fifo_buf = NULL;
		return err;
	}
	pr_info("wl_diag: coda %d record (%d KB)\n", fifo_recs,
		(int)(fifo_recs * sizeof(struct wldiag_rec) / 1024));

	if (!proc_create(WD_PROC, 0400, NULL, &wd_fops)) {
		pr_err("wl_diag: proc_create(/proc/%s) fallita\n", WD_PROC);
		vfree(fifo_buf);
		fifo_buf = NULL;
		return -ENOMEM;
	}

	if (!arm) {
		pr_info("wl_diag: DRY-RUN (%d hook pianificati). insmod con arm=1 per applicare.\n",
			n_elig);
		return 0;
	}

	/* risolvi il flush della i-cache. NB: questo kernel ha KALLSYMS ma non
	 * KALLSYMS_ALL, quindi kallsyms espone solo simboli di TESTO (funzioni):
	 * la variabile-puntatore 'flush_icache_range' (in BSS) e' invisibile.
	 * Risolviamo direttamente la funzione del cache-layer R4K, con ripieghi. */
	{
		static const char * const cand[] = {
			"r4k_flush_icache_range",
			"local_r4k_flush_icache_range",
			"local_flush_icache_range",  /* anch'esso var: probabile miss */
		};
		int k;

		for (k = 0; k < ARRAY_SIZE(cand); k++) {
			unsigned long a = kallsyms_lookup_name(cand[k]);

			if (a) {
				p_flush_icache = (flush_fn_t)a;
				pr_info("wl_diag: flush via '%s' @%px\n", cand[k], (void *)a);
				break;
			}
		}
	}
	if (!p_flush_icache) {
		pr_err("wl_diag: nessun flush i-cache risolvibile, resto in DRY-RUN\n");
		return 0;
	}

	for (i = 0; i < n_elig; i++) {
#if WD_HAVE_BP
		if (hooks[eligible[i]].use_bp) {
			build_bp_stub(eligible[i]);
			continue;
		}
#endif
		build_stub(eligible[i]);
	}
	flush_i((unsigned long)stub_pool,
		(unsigned long)stub_pool + sizeof(stub_pool));
	{
		int any_retcap = 0;

		for (i = 0; i < n_elig; i++)
			if (hooks[eligible[i]].retcap)
				any_retcap = 1;
		if (any_retcap) {
			build_ret_trampoline();
			flush_i((unsigned long)ret_tramp,
				(unsigned long)ret_tramp + sizeof(ret_tramp));
			ret_trampoline = (unsigned long)ret_tramp;
			pr_info("wl_diag: trampolino ritorno @%px\n",
				(void *)ret_trampoline);
		}
	}
#if WD_HAVE_BP
	{
		int any_bp = 0;

		for (i = 0; i < n_elig; i++)
			if (hooks[eligible[i]].use_bp)
				any_bp = 1;
		/* il notifier va registrato PRIMA di piazzare i break, o la
		 * prima trap finisce in do_trap_or_bp -> panic. */
		if (any_bp) {
			err = register_die_notifier(&wd_bp_nb);
			if (err) {
				pr_err("wl_diag: register_die_notifier: %d, resto in DRY-RUN\n",
				       err);
				return 0;
			}
			bp_registered = true;
		}
	}
#endif
	target_mod = __module_text_address(hooks[eligible[0]].addr);
	target_ref_get(target_mod);
	if (register_module_notifier(&wd_mod_nb))
		pr_warn("wl_diag: register_module_notifier fallita: se il modulo "
			"bersaglio si scarica mentre siamo armati, non lo sapremo\n");
	else
		mod_nb_registered = true;

	for (i = 0; i < n_elig; i++) {
		if (hooks[eligible[i]].use_sites) {
			patch_sites(eligible[i]);
			hooks[eligible[i]].armed = true;
			continue;
		}
#if WD_HAVE_BP
		if (hooks[eligible[i]].use_bp) {
			u32 *o = (u32 *)hooks[eligible[i]].addr;

			o[0] = BP_INSN;
			flush_i(hooks[eligible[i]].addr,
				hooks[eligible[i]].addr + 4);
			hooks[eligible[i]].armed = true;
			continue;
		}
#endif
		patch_entry(eligible[i]);
		hooks[eligible[i]].armed = true;
	}
	pr_info("wl_diag: ARMATO (%d hook) -> /proc/wl_diag\n", n_elig);
	return 0;
}

static void __exit wd_exit(void)
{
	int i;

	/* stop nuovi dirottamenti di ra prima di ripristinare i prologhi; gli
	 * stub in volo che hanno gia' dirottato tornano comunque via ret_tramp
	 * (statico, valido), e synchronize_sched aspetta che completino. */
	ret_trampoline = 0;
	if (mod_nb_registered) {
		unregister_module_notifier(&wd_mod_nb);
		mod_nb_registered = false;
	}

	for (i = 0; i < NHOOK; i++)
		if (hooks[i].armed) {
			if (hooks[i].use_sites) {
				restore_sites(i);
				hooks[i].armed = false;
				continue;
			}
#if WD_HAVE_BP
			if (hooks[i].use_bp) {
				u32 *o = (u32 *)hooks[i].addr;

				o[0] = hooks[i].saved[0];
				flush_i(hooks[i].addr, hooks[i].addr + 4);
				hooks[i].armed = false;
				continue;
			}
#endif
			restore_entry(i);
			hooks[i].armed = false;
		}
#if WD_HAVE_BP
	/* il notifier si sgancia DOPO aver ripristinato le parole: se restasse un
	 * break in giro senza handler, la trap finirebbe in panic. */
	if (bp_registered) {
		unregister_die_notifier(&wd_bp_nb);
		bp_registered = false;
	}
#endif
	/* lascia agli stub in volo il tempo di completare prima di sparire */
	synchronize_sched();
	if (mod_ref_held && target_mod) {
		module_put(target_mod);
		mod_ref_held = false;
	}
	remove_proc_entry(WD_PROC, NULL);
	vfree(fifo_buf);
	fifo_buf = NULL;
	pr_info("wl_diag: scaricato (persi: %d, filtrati: %d)\n",
		atomic_read(&drops), atomic_read(&filtered));
}

module_init(wd_init);
module_exit(wd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Inline-detour PHY/radio/PMU tracer for Broadcom wl (no kprobes)");
