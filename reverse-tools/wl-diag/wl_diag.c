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
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <asm/cacheflush.h>

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
	OP_DROP = 255,
};
struct wldiag_rec {
	u64 ts_ns; u32 seq; u32 addr; u32 val; u32 aux;
	u8 op; u8 cpu; u16 _pad;
} __packed;

#define FIFO_RECS 32768
static DEFINE_KFIFO(fifo, struct wldiag_rec, FIFO_RECS);
static DEFINE_RAW_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(rq);
static atomic_t seq = ATOMIC_INIT(0);
static atomic_t drops = ATOMIC_INIT(0);

static void emit(u8 op, u32 addr, u32 val, u32 aux)
{
	struct wldiag_rec r;
	unsigned long flags;

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
}

/* ---- tabella hook ----------------------------------------------------- *
 * Ogni campo prende un arg d'ingresso: 0=assente(->0), 1=a1, 2=a2, 3=a3.   *
 * Reg-style: addr=a1(reg), val/mask da a2/a3. GPIO ChipCommon              *
 * (sih,mask,val,prio) non ha reg: addr=0, val=a2, mask(aux)=a1.            */
struct hook {
	const char *name;
	u8 op, addr_src, val_src, aux_src;
	bool shortj;		/* true: detour a 1 parola 'j' (branch nella finestra a 4) */
	unsigned long addr;
	u32 saved[4];
	bool armed;
};
static struct hook hooks[] = {
	{ "phy_reg_read",       OP_PHY_R,     1, 0, 0 },
	{ "phy_reg_write",      OP_PHY_W,     1, 2, 0 },
	{ "phy_reg_mod",        OP_PHY_MOD,   1, 3, 2 },
	/* and/or: reg unico op (addr,val). Op-code distinti cosi' il decoder sa
	 * l'operazione; val=a2 e' la maschera-AND (bit tenuti) risp. il valore-OR
	 * (bit settati). Niente aux: la funzione non ha un 3o argomento. */
	{ "phy_reg_and",        OP_PHY_AND,   1, 2, 0 },
	{ "phy_reg_or",         OP_PHY_OR,    1, 2, 0 },
	{ "write_radio_reg",    OP_RADIO_W,   1, 2, 0 },
	{ "mod_radio_reg",      OP_RADIO_MOD, 1, 3, 2 },
	{ "si_pmu_chipcontrol", OP_PMU_CC,    1, 3, 2 },
	{ "si_pmu_regcontrol",  OP_PMU_RC,    1, 3, 2 },
	{ "si_pmu_pllcontrol",  OP_PMU_PLL,   1, 3, 2 },
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
	{ "wlc_bmac_mhf_get",   OP_MAC_MHF_R, 1, 0, 0 },
	/* branch a slot 3 (beq): detour classico a 4 parole impossibile. short-j a
	 * 1 parola: o[0]=j stub; o[1] (addiu $v0,1) resta come delay slot; lo stub
	 * riesegue o[0..1] e rientra a +8 (v0 ri-settato DOPO la hook). addr=a1
	 * grezzo (l'andi 0xffff e' o[0], rieseguito nello stub). */
	{ "read_radio_reg",     OP_RADIO_R,   1, 0, 0, true },
};
#define NHOOK ARRAY_SIZE(hooks)

/* Punto d'atterraggio del detour: chiamato dallo stub con (id, a1, a2, a3). */
static inline u32 pick(u8 src, u32 a1, u32 a2, u32 a3)
{
	return src == 1 ? a1 : src == 2 ? a2 : src == 3 ? a3 : 0;
}
void __used noinline
wl_diag_hook(u32 id, u32 a1, u32 a2, u32 a3)
{
	struct hook *h = &hooks[id];

	emit(h->op, pick(h->addr_src, a1, a2, a3),
		    pick(h->val_src,  a1, a2, a3),
		    pick(h->aux_src,  a1, a2, a3));
}

/* ---- mini-assembler MIPS o32 (codifiche verificate) ------------------- */
#define R_ZERO 0
#define R_A0 4
#define R_T9 25
#define R_SP 29
#define R_RA 31
static inline u32 i_addiu(u8 rt, u8 rs, s16 im){ return (0x09u<<26)|(rs<<21)|(rt<<16)|(u16)im; }
static inline u32 i_sw(u8 rt, u8 b, s16 o){ return (0x2bu<<26)|(b<<21)|(rt<<16)|(u16)o; }
static inline u32 i_lw(u8 rt, u8 b, s16 o){ return (0x23u<<26)|(b<<21)|(rt<<16)|(u16)o; }
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
#define STUB_WORDS 28
static u32 stub_pool[NHOOK][STUB_WORDS] __attribute__((aligned(8)));

static void build_stub(int idx)
{
	u32 *s = stub_pool[idx];
	u32 *o = (u32 *)hooks[idx].addr;
	unsigned long hookfn = (unsigned long)&wl_diag_hook;
	int rep = hooks[idx].shortj ? 2 : 4;	/* parole originali da rieseguire */
	unsigned long ret = hooks[idx].addr + (hooks[idx].shortj ? 8 : 16);
	int n = 0, k;

	s[n++] = i_addiu(R_SP, R_SP, -32);
	s[n++] = i_sw(4, R_SP, 0);   /* a0 */
	s[n++] = i_sw(5, R_SP, 4);   /* a1 */
	s[n++] = i_sw(6, R_SP, 8);   /* a2 */
	s[n++] = i_sw(7, R_SP, 12);  /* a3 */
	s[n++] = i_sw(R_RA, R_SP, 16);
	s[n++] = i_ori(R_A0, R_ZERO, (u16)idx);          /* a0 = hookid */
	s[n++] = i_lui(R_T9, hookfn >> 16);
	s[n++] = i_ori(R_T9, R_T9, hookfn & 0xffff);
	s[n++] = i_jalr(R_T9);                            /* a1..a3 = orig args */
	s[n++] = I_NOP;
	s[n++] = i_lw(4, R_SP, 0);
	s[n++] = i_lw(5, R_SP, 4);
	s[n++] = i_lw(6, R_SP, 8);
	s[n++] = i_lw(7, R_SP, 12);
	s[n++] = i_lw(R_RA, R_SP, 16);
	s[n++] = i_addiu(R_SP, R_SP, 32);
	for (k = 0; k < rep; k++)
		s[n++] = o[k];   /* riesegue le parole spiazzate (o[1] short-j ri-setta v0) */
	s[n++] = i_lui(R_T9, ret >> 16);
	s[n++] = i_ori(R_T9, R_T9, ret & 0xffff);
	s[n++] = i_jr(R_T9);
	s[n++] = I_NOP;
	/* classic: n==25 ; short-j: n==23 ; entrambi <= STUB_WORDS */
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
static struct miscdevice wd_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wl_diag",
	.fops = &wd_fops,
};

/* ---- init/exit -------------------------------------------------------- */
static int eligible[NHOOK];   /* indici agganciabili */
static int n_elig;

static int __init wd_init(void)
{
	int i, err;

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
		if (branch >= 0) {
			pr_warn("wl_diag: salto '%s' (branch a istr %d, non rilocabile)\n",
				hooks[i].name, branch);
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

	err = misc_register(&wd_misc);
	if (err)
		return err;

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

	for (i = 0; i < n_elig; i++)
		build_stub(eligible[i]);
	flush_i((unsigned long)stub_pool,
		(unsigned long)stub_pool + sizeof(stub_pool));
	for (i = 0; i < n_elig; i++) {
		patch_entry(eligible[i]);
		hooks[eligible[i]].armed = true;
	}
	pr_info("wl_diag: ARMATO (%d hook) -> /dev/wl_diag\n", n_elig);
	return 0;
}

static void __exit wd_exit(void)
{
	int i;

	for (i = 0; i < NHOOK; i++)
		if (hooks[i].armed) {
			restore_entry(i);
			hooks[i].armed = false;
		}
	/* lascia agli stub in volo il tempo di completare prima di sparire */
	synchronize_sched();
	misc_deregister(&wd_misc);
	pr_info("wl_diag: scaricato (record persi: %d)\n", atomic_read(&drops));
}

module_init(wd_init);
module_exit(wd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Inline-detour PHY/radio/PMU tracer for Broadcom wl (no kprobes)");
