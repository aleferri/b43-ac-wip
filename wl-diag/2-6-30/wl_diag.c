// SPDX-License-Identifier: GPL-2.0
/*
 * wl_diag, variant for kernel 2.6.30 (DSL-3580L, BCM6362 SoC, MIPS32 BE,
 * gcc 4.4.2 buildroot).
 *
 * The mechanism and the record format are those of the 3.4 variant: see the
 * head of ../3-4-11/wl_diag.c. The op codes are a subset of the 3.4 ones, with
 * the same values, so ../decode-wl-diag.py decodes the traces of every router
 * and they can be cross-correlated (between versions of wl the content of the
 * trace changes, not its format).
 *
 * Compared with 3.4 this lacks the break path, the diversion of the tail
 * call, the stop_machine around the entry patches, the one-hook-per-op rule,
 * the filter on symbols outside the target, the check on lui words shared
 * between epilogues and the collision check between hooks. The short-j leaves
 * o[1] in place as the delay slot of the `j` instead of nulling it.
 *
 * Only the pre-2.6.33 kernel glue is adapted here: a hand-made ring instead
 * of the typed kfifo, spinlock_t instead of raw_spinlock, and the three
 * #ifdefs below (pr_warn, kallsyms_lookup_name, sched_clock), each with its
 * reason next to it.
 *
 * If kallsyms_lookup_name is not exported to modules, the easy way is
 * 'klookup=<addr from /proc/kallsyms>': the module calls it by address and
 * resolves the rest by itself.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <linux/notifier.h>

/*
 * pr_warn is an alias of pr_warning added in 2.6.35; the DSL-3580L's 2.6.30
 * kernel only has pr_warning. #ifndef, so a backport that already defines it
 * wins.
 */
#ifndef pr_warn
#define pr_warn pr_warning
#endif

/*
 * kallsyms_lookup_name has always existed, but its EXPORT_SYMBOL to modules
 * came in 2.6.33: below that the link fails with "Unknown symbol
 * kallsyms_lookup_name", so that branch is disabled and 'klookup=' is used.
 * It can be forced by hand with -DWLDIAG_NO_KALLSYMS.
 */
#if !defined(WLDIAG_NO_KALLSYMS) && LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#define WLDIAG_NO_KALLSYMS
#endif

/*
 * sched_clock() is not exported to modules before 3.4; cpu_clock(cpu) is the
 * per-cpu wrapper with the same value in ns and is EXPORT_SYMBOL_GPL on both
 * 2.6.30 and 3.4, so it is used on both.
 */
static inline u64 wldiag_now_ns(void)
{
	return cpu_clock(raw_smp_processor_id());
}

static int arm;
module_param(arm, int, 0444);
MODULE_PARM_DESC(arm, "0=dry-run (solo log del piano), 1=applica le patch");

/* osl_delay is noisy (one entry per udelay) and the usec value captured from
 * a1 is not reliable on every path -- sometimes it is garbage (argument in a
 * different register, or an inlined path). It stays detached by default;
 * delay=1 re-attaches it when the timing is really needed. */
static int delay;
module_param(delay, int, 0444);
MODULE_PARM_DESC(delay, "0=non agganciare osl_delay (default), 1=aggancia");

/* Name of the target module, for the match on mod->name in the notifier. Not
 * cosmetic: hooks are resolved by symbol name, and without the module check
 * any COMING at all would restart the plan. */
static char *target = "wl";
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "nome del modulo da agganciare (default wl)");

/* Address of kallsyms_lookup_name (from /proc/kallsyms). Where the function
 * exists but is not exported to modules (2.6.30..2.6.32) it cannot be linked
 * by name but can still be called by address: given only this, the module
 * resolves every other symbol by itself.
 *
 * It is the only way, and there is no second one on purpose. Passing the
 * addresses by hand would fix the resolution at wl_diag's insmod, and after a
 * reload of the target they would be addresses of memory that is no longer
 * its own: incompatible with re-arming, and so with the cold capture, which is
 * why this tracer exists. CONFIG_KALLSYMS is needed anyway to resolve the
 * symbols of `wl`, so kallsyms_lookup_name is always in /proc/kallsyms and a
 * second mechanism adds nothing. */
static ulong klookup;
module_param(klookup, ulong, 0444);
MODULE_PARM_DESC(klookup,
	"indirizzo di kallsyms_lookup_name (da /proc/kallsyms); risolve gli altri simboli da solo");

typedef unsigned long (*kln_fn_t)(const char *name);

/* Symbol resolution through kallsyms_lookup_name: called by address if
 * 'klookup' was given, otherwise by name where the symbol can be linked
 * (kernels with the export, that is from 2.6.33). */
static unsigned long resolve_sym(const char *name)
{
	unsigned long a = 0;

	if (klookup)
		return ((kln_fn_t)klookup)(name);
#ifndef WLDIAG_NO_KALLSYMS
	a = kallsyms_lookup_name(name);
#endif
	return a;
}

/* flush_icache_range is not exported to modules, and on this kernel
 * (KALLSYMS without KALLSYMS_ALL) the pointer variable is not even visible to
 * kallsyms, because it lives in BSS. So the text function of the R4K cache
 * layer is resolved instead (see wd_init) and called through this pointer. */
typedef void (*flush_fn_t)(unsigned long, unsigned long);
static flush_fn_t p_flush_icache;
static void flush_i(unsigned long s, unsigned long e)
{
	if (p_flush_icache)
		p_flush_icache(s, e);
}

/* ---- record + queue -------------------------------------------------- */
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
	OP_MARK,					/* 39 (append) */
	OP_CHANSPEC_SHM,				/* 40 (append) */
	OP_MAC_OBJ_BULK_R, OP_MAC_OBJ_BULK_W,		/* 41,42 (append) */
	OP_AMT_W, OP_RCMTA_W, OP_ADDRMATCH,		/* 43,44,45 (append) */
	OP_PHY_WARR, OP_PHY_RDW, OP_PHY_WRW,		/* 46,47,48 (append) */
	OP_IHR_W, OP_OBJ_SET,				/* 49,50 (append) */
	/* 51-54 belong to 3.4 (PHY.FGC, IOCTL, IOVAR.*): they are not here, but
	 * the numbers stay the same, so there is one decoder. */
	OP_TX_PKT = 55, OP_TX_DATA,			/* 55,56 (append) */
	OP_TPL_DATA,					/* 57 (append) */
	OP_RX_PKT, OP_RX_DATA,				/* 58,59 (append) */
	/* 60-64 are wl-mmio-trap's (MMIO.*, DMA.*), merged into the same
	 * streams. */
	OP_TXS = 65, OP_TXS_DATA,			/* 65,66 (append) */
	OP_DROP = 255,
};
struct wldiag_rec {
	u64 ts_ns; u32 seq; u32 addr; u32 val; u32 aux;
	u8 op; u8 cpu; u16 _pad;
} __packed;

/*
 * The record queue, allocated at run time and sized by a parameter.
 *
 * It was a static array of 32768 records, 896 KB in the module's BSS. On a
 * DSL-3580L, 63 MB of RAM, the total free memory of DMA+Normal is around
 * 550 KB: the queue alone is one and a half times all the free memory, and
 * the insmod of `wl` fails in osl_ctfpool_init() with a page allocation
 * failure. That allocation is GFP_ATOMIC, so it cannot reclaim the ~15 MB of
 * page cache: the memory has to be free BEFORE.
 *
 * Hence: allocated with vmalloc at init and freed at exit, so a dry run costs
 * nothing, and the size is a parameter. The default is 8192 records, 224 KB.
 *
 * Raising it is not the lever for lost records: earlier captures show that
 * the bottleneck is the DRAIN, not the capacity -- the rate rises if the wait
 * is shortened, because the bring-up bursts become a larger share of the
 * time. Splitting a sweep into several runs costs zero bytes.
 */
#define FIFO_RECS_MIN 1024
#define FIFO_RECS_MAX 32768
static uint fifo_recs = 8192;
module_param(fifo_recs, uint, 0444);
MODULE_PARM_DESC(fifo_recs,
	"record nella coda, potenza di 2 (default 8192 = 224 KB)");
static struct wldiag_rec *ring;
static u32 ring_head, ring_tail;	/* count = (u32)(head - tail); empty when equal */
static DEFINE_SPINLOCK(fifo_lock);	/* non-RT: spin_* is raw_spin_* */
static DECLARE_WAIT_QUEUE_HEAD(rq);
static atomic_t seq = ATOMIC_INIT(0);
static atomic_t drops = ATOMIC_INIT(0);

/* fifo_recs brought into [MIN, MAX] and rounded down to a power of 2:
 * emit() and wd_read() index with fifo_recs - 1 as the mask. */
static int ring_alloc(void)
{
	if (fifo_recs < FIFO_RECS_MIN || fifo_recs > FIFO_RECS_MAX) {
		uint r = fifo_recs < FIFO_RECS_MIN ? FIFO_RECS_MIN : FIFO_RECS_MAX;

		pr_warn("wl_diag: fifo_recs=%u fuori da [%u, %u], uso %u\n",
			fifo_recs, FIFO_RECS_MIN, FIFO_RECS_MAX, r);
		fifo_recs = r;
	}
	fifo_recs = 1U << (fls(fifo_recs) - 1);

	ring = vmalloc(fifo_recs * sizeof(*ring));
	if (!ring) {
		pr_err("wl_diag: vmalloc di %u KB per la coda fallita, "
		       "riprova con un fifo_recs piu' basso\n",
		       (uint)(fifo_recs * sizeof(*ring) / 1024));
		return -ENOMEM;
	}
	pr_info("wl_diag: coda di %u record (%u KB)\n", fifo_recs,
		(uint)(fifo_recs * sizeof(*ring) / 1024));
	return 0;
}

/* PHY REGISTER reads not to record, to spare the fifo. It comes from the
 * radar detector polling: on DFS channels the driver reads 0x0253 and 0x0254
 * continuously -- 192000 and 194000 reads across the four phases -- and twice
 * that with RETVALs on, with no bearing whatsoever on the channel setup.
 * Filtering HERE, before the fifo, preserves the margin; in the decoder it
 * would not help, the bottleneck is the queue.
 *
 *   skipphyrd="0x253,0x254"
 *
 * THIS APPLIES TO OP_PHY_R ONLY, and that is not pedantry: address spaces are
 * separate per class. The same captures hold 32 OBJ.WR at 0x252 and 32 at
 * 0x254, which are object-memory offsets and have nothing to do with the
 * same-numbered PHY registers: a filter on the address alone would have
 * thrown them away in silence.
 *
 * And ONLY 0x253/0x254 are filtered. The head of the block -- 0x251 and
 * 0x252, read 1558 times in total, once per block -- is plausibly the pulse
 * state and data, which is the part that matters: it costs little and it is
 * kept.
 *
 * Filtered records do NOT count as lost: separate counter, so OP_DROP stays
 * an indicator of real loss.
 *
 * DFS needs dedicated captures with no filter. Linux already has the ETSI/FCC
 * classifier (dfs_pattern_detector, 377 lines), so all that is needed is the
 * format of those registers, not the classification.
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
		/* kstrtoul arrived in 2.6.38: simple_strtoul is on both
		 * kernels and validity is checked on the end pointer. */
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

	r.ts_ns = wldiag_now_ns();
	r.seq = (u32)atomic_inc_return(&seq);
	r.addr = addr; r.val = val; r.aux = aux;
	r.op = op; r.cpu = (u8)raw_smp_processor_id(); r._pad = 0;

	spin_lock_irqsave(&fifo_lock, flags);
	if ((u32)(ring_head - ring_tail) < fifo_recs) {
		ring[ring_head & (fifo_recs - 1)] = r;
		ring_head++;
	} else {
		atomic_inc(&drops);
	}
	spin_unlock_irqrestore(&fifo_lock, flags);
	wake_up_interruptible(&rq);
	return r.seq;
}

/*
 * A record followed by n bytes of kernel memory at src in data records,
 * twelve per record packed big-endian like MARK, queued under one lock hold
 * so that the sequence numbers of the group are consecutive and no other
 * cpu's record falls inside it. All or nothing: a group that does not fit
 * counts as lost whole. The caller checks src with readable() first; a chunk
 * that still faults is recorded as zeros.
 */
static u32 emit_group(u8 op, u32 addr, u32 val, u32 aux, u8 dop,
		      const u8 *src, u32 n)
{
	struct wldiag_rec r;
	unsigned long flags;
	u32 nrec = 1 + (n + 11) / 12, i, k, first = 0;

	spin_lock_irqsave(&fifo_lock, flags);
	if (fifo_recs - (u32)(ring_head - ring_tail) < nrec) {
		atomic_add(nrec, &drops);
		spin_unlock_irqrestore(&fifo_lock, flags);
		return 0;
	}
	for (i = 0; i < nrec; i++) {
		r.ts_ns = wldiag_now_ns();
		r.seq = (u32)atomic_inc_return(&seq);
		r.cpu = (u8)raw_smp_processor_id();
		r._pad = 0;
		if (!i) {
			r.op = op;
			r.addr = addr; r.val = val; r.aux = aux;
			first = r.seq;
		} else {
			u32 w[3] = { 0, 0, 0 }, o = (i - 1) * 12;
			u8 c[12];
			u32 m = n - o < 12 ? n - o : 12;

			if (probe_kernel_read(c, src + o, m))
				memset(c, 0, m);
			for (k = 0; k < m; k++)
				w[k / 4] |= (u32)c[k] << (24 - 8 * (k % 4));
			r.op = dop;
			r.addr = w[0]; r.val = w[1]; r.aux = w[2];
		}
		ring[ring_head & (fifo_recs - 1)] = r;
		ring_head++;
	}
	spin_unlock_irqrestore(&fifo_lock, flags);
	wake_up_interruptible(&rq);
	return first;
}

/* Whether n bytes at p can be read. They are one buffer of the driver, in
 * lowmem or vmalloc, so its two ends answer for the middle. */
static bool readable(const void *p, u32 n)
{
	u8 c;

	return n && !probe_kernel_read(&c, p, 1) &&
	       !probe_kernel_read(&c, (const u8 *)p + n - 1, 1);
}

/* ---- markers ---------------------------------------------------------- *
 * The same as the 3.4 tracer's, and they must stay so: the decoder undoes the
 * packing by hand and does not tell the two versions apart. Twelve characters
 * per record, explicitly big-endian, the excess cut.
 *
 * They are the boundary `split_trace.py --on mark` cuts on: without them a
 * sweep has no segments and hot and cold cannot be told apart. */
static u32 pack4(const char *p)
{
	return ((u32)(u8)p[0] << 24) | ((u32)(u8)p[1] << 16) |
	       ((u32)(u8)p[2] << 8) | (u32)(u8)p[3];
}

static void emit_mark(const char *b12)
{
	emit(OP_MARK, pack4(b12), pack4(b12 + 4), pack4(b12 + 8));
}

static void mark(const char *s)
{
	char b[12];
	int i;

	memset(b, 0, sizeof(b));
	for (i = 0; i < (int)sizeof(b) && s[i]; i++)
		b[i] = s[i];
	emit_mark(b);
}

/* ---- hook table ------------------------------------------------------- *
 * Each field takes an entry argument: 0=absent(->0), 1=a1, 2=a2, 3=a3.     *
 * Reg-style: addr=a1(reg), val/mask from a2/a3. ChipCommon GPIO            *
 * (sih,mask,val,prio) has no reg: addr=0, val=a2, mask(aux)=a1.            */
struct hook {
	const char *name;
	u8 op, addr_src, val_src, aux_src;
	bool shortj;		/* true: 1-word 'j' detour (branch inside the 4-word window) */
	bool retcap;		/* true: capture the return value through the ra trampoline */
	u8 nargx;		/* # of stack arguments to capture: arg5@16(sp), arg6@20(sp) */
	unsigned long addr;
	u32 saved[4];
	bool armed;
	/* State fields: they MUST stay at the tail, because the table
	 * initialises the first five fields positionally and a field inserted
	 * in the middle shifts every value after it. That is how a `true`
	 * meant for retcap ended up in the field before, retcap stayed false
	 * for every hook and NO RETVAL was ever emitted. The fields after
	 * @aux_src are set by name. */
	bool use_sites;		/* patch the lui/addiu pairs at the call sites */
	/* Name of a lower-level hook: if THAT one hooks, this one is
	 * skipped. It is for the thunks. `wlc_bmac_read/write_shm` are 16 and
	 * 20 byte thunks that tail-call the object-memory accessor, so hooking
	 * both would give TWO records for every shared-memory access: one from
	 * the thunk, with aux=0 by construction, and one from the accessor's
	 * entry, with the real selector. Every OBJ op would show up twice and
	 * the comparison with the port would be useless.
	 *
	 * The accessor underneath is also strictly more informative -- it
	 * carries the selector, and so the region -- and sees everything,
	 * including what goes through the bulk pair without touching the
	 * thunks. The thunks stay as the fallback for a firmware where the
	 * accessor does not resolve. */
	const char *ripiego_di;
	/* For OP_TX_PKT: a1 is a buffer and a2 its length, not an osl
	 * packet. Set by name. */
	bool txraw;
};
static struct hook hooks[] = {
	{ "phy_reg_read",       OP_PHY_R,     1, 0, 0, .retcap = true },
	{ "phy_reg_write",      OP_PHY_W,     1, 2, 0 },
	{ "phy_reg_mod",        OP_PHY_MOD,   1, 3, 2 },
	/* and/or: single-register op (addr,val). Distinct op codes so the
	 * decoder knows the operation; val=a2 is the AND mask (bits kept) or
	 * the OR value (bits set). No aux: the function has no 3rd argument. */
	{ "phy_reg_and",        OP_PHY_AND,   1, 2, 0 },
	{ "phy_reg_or",         OP_PHY_OR,    1, 2, 0 },
	{ "write_radio_reg",    OP_RADIO_W,   1, 2, 0 },
	{ "mod_radio_reg",      OP_RADIO_MOD, 1, 3, 2 },
	{ "si_pmu_chipcontrol", OP_PMU_CC,    1, 3, 2, .retcap = true },
	{ "si_pmu_regcontrol",  OP_PMU_RC,    1, 3, 2, .retcap = true },
	{ "si_pmu_pllcontrol",  OP_PMU_PLL,   1, 3, 2, .retcap = true },
	/* si_corereg(sih, coreidx, regoff, mask, val): generic access to a
	 * register of a backplane core. addr=regoff(a2), aux=coreidx(a1).
	 * val (a4, the 5th argument) is on the stack in o32 -> captured
	 * through nargx (a follow-on ARGX record). retcap: the return value
	 * (read/rmw) goes in the RETVAL. */
	{ "si_corereg",         OP_SI_COREREG,2, 0, 1, .retcap = true, .nargx = 1 },
	/* ChipCommon GPIO (sih, mask, val, prio): mask=a1, val=a2 */
	{ "si_gpiocontrol",     OP_CC_GPIOCTL,0, 2, 1 },
	{ "si_gpioout",         OP_CC_GPIOOUT,0, 2, 1 },
	{ "si_gpioouten",       OP_CC_GPIOOE, 0, 2, 1 },
	/* acphy table access (pi, id, len, off, width, data): id=a1, len=a2,
	 * off=a3. width/data are stack arguments and are not captured. Check
	 * the len/off order against your headers: the disasm pins id=a1 but
	 * not len-vs-off. */
	{ "wlc_phy_table_read_acphy",  OP_TBL_R, 1, 2, 3 },
	{ "wlc_phy_table_write_acphy", OP_TBL_W, 1, 2, 3 },
	{ "osl_delay",          OP_DELAY,     0, 1, 0 }, /* usec=a1 */
	/* Control towards the MAC (the d11 core). MACCONTROL RMW plus MAC host
	 * flags. Signatures taken from the brcmsmac branch (a mirror of the
	 * proprietary wl) -- to be re-checked against the disasm like the other
	 * hooks (cf. the len/off caveat on wlc_phy_table_*). If a prologue has
	 * a branch in the first 4 words, pianifica() falls back on the call
	 * sites or skips the hook with a pr_warn: no risk.
	 *   wlc_bmac_mctrl(hw, u32 mask, u32 val)   fixed reg: mask=a1, val=a2
	 *   wlc_bmac_mhf(hw, u8 idx, u16 mask, u16 val, int bands)
	 *                                           idx=a1, mask=a2, val=a3
	 *   wlc_bmac_mhf_get(hw, u8 idx, int bands) idx=a1 (val UNDEFINED) */
	{ "wlc_bmac_mctrl",     OP_MAC_MCTRL, 0, 2, 1 },
	/* `bands` is the 5th argument and in o32 sits at 16(sp): it is
	 * captured with nargx, and it is needed. In the captures the write of
	 * the HOSTF cell sometimes follows the call and sometimes does not --
	 * on cold01 #469 slot 3 and #620 slot 4 have no adjacent write, which
	 * reappears at the #689-#690 flush, while #623, #12242, #13525, #13530
	 * and #13535 have it straight away. The guess is that the cell is
	 * written only when `bands` matches the current band and that
	 * otherwise the value stays cached; without that argument the two
	 * cannot be told apart, and the accumulated values (0x80 -> 0x88 ->
	 * 0x8088 on slot 4) stay unexplained. */
	{ "wlc_bmac_mhf",       OP_MAC_MHF_W, 1, 3, 2, .nargx = 1 },
	{ "wlc_bmac_mhf_get",   OP_MAC_MHF_R, 1, 0, 0, .retcap = true },
	/* Template RAM: the bulk only; 6.30 has no ptr/data accessors.
	 * tpl_rec() records the content of the write. */
	{ "wlc_bmac_write_template_ram", OP_TPL_RAMW, 1, 2, 0 },
	/* OTP: the generic layer has the same names on 6.30 and 7.14 and a
	 * clean prologue, while the hndotp_ and ipxotp_ ones change. The
	 * content is the SROM image, static and already known from the dumps:
	 * what this is for is knowing WHEN it is read and WHICH words, that is,
	 * where the values are consumed.
	 *   otp_read_word(oh, wn, *data)              wn=a1
	 *   otp_read_region(sih, region, *data, *len) region=a1
	 *   otp_init(sih)                             the moment only */
	{ "otp_init",        OP_OTP_INIT, 0, 0, 0, .retcap = true },
	{ "otp_read_word",   OP_OTP_RDW,  1, 0, 2, .retcap = true },
	{ "otp_read_region", OP_OTP_RDR,  1, 0, 3, .retcap = true },
	/* Two accessors the acphy code calls. The call graph says WHERE they
	 * go:
	 *
	 *   wlc_bmac_bw_set     <- wlapi_bmac_bw_set <- wlc_phy_chanspec_set_acphy
	 *                                            <- wlc_phy_init
	 *     bandwidth at the MAC level. Needed for 40 and 80 MHz: with BW20
	 *     only the default goes unnoticed. It belongs in set_channel and
	 *     op_init.
	 *   si_get/set_sromctl  <- wlc_phy_attach_acphy
	 *     the SROM control register. It belongs at the point of op_init
	 *     that corresponds to attach_acphy.
	 *
	 * Signatures read off the prologue (arguments intact in a0-a3):
	 *   wlc_bmac_bw_set(hw, bw)        bw=a1, not an address -> val
	 *   si_get_sromctl(sih)            value in the RETVAL
	 *   si_set_sromctl(sih, val)       val=a1
	 *
	 * wlc_bmac_macphyclk_set is NOT hooked: its callers are init_htphy,
	 * init_nphy and wlc_bmac_init, so for the AC-PHY it is not on the path.
	 * It also has a bne in word 1, but that is beside the point. */
	/* The moment cal_init is invoked: once per bring-up cycle, which also
	 * makes it the anchor for segmenting a sweep.
	 *
	 * A COLD cycle comes from reloading the target module, which is what
	 * wl-capture-scripts/capture_cold_init.sh does, not from tampering
	 * with the PHY struct from the stub. The "already calibrated" byte (227
	 * on 6.30, 251 on 7.14.89) stays a note: when cold it is that byte at
	 * zero that makes cal_init complete. */
	{ "wlc_phy_cal_init", OP_CAL_INIT,   0, 0, 0 },
	{ "wlc_bmac_bw_set",  OP_MAC_BW,     0, 1, 0 },
	{ "si_get_sromctl",   OP_SROMCTL_R,  0, 0, 0, .retcap = true },
	{ "si_set_sromctl",   OP_SROMCTL_W,  0, 1, 0 },
	/* Channel change: chanspec in a1. The generic one is hooked, which
	 * fires for every PHY and allows a single run over several channels to
	 * be split afterwards. */
	{ "wlc_phy_chanspec_set", OP_CHANSPEC, 1, 0, 0 },
	/* MAC object memory (SHM, SCR, IHR): addr=offset, aux=selector.
	 * This also catches the noise sample of the crs_min_pwr cal, which
	 * comes through wlc_phy_noise_read_shmem -> wlapi_bmac_read_shm ->
	 * wlc_bmac_read_shm -> here, not from a PHY register.
	 * NAME PER VERSION: read_objmem on 6.30, read_objmem16 on 7.14. */
	{ "wlc_bmac_read_objmem",  OP_MAC_OBJ_R, 1, 0, 2, .retcap = true },
	{ "wlc_bmac_write_objmem", OP_MAC_OBJ_W, 1, 2, 3 },
	/* 16-bit thunks above read/write_objmem. Those two are LOCAL, which
	 * does not hide them from kallsyms: on 2.6.30 add_kallsyms() keeps the
	 * module's whole symtab, and CONFIG_KALLSYMS_ALL is about the kernel's
	 * only. They can however be missing from a blob's symtab altogether,
	 * and then these, GLOBAL, are the only way to the shared memory. They
	 * cover the SHM selector only, so aux stays 0.
	 *   wlc_bmac_read_shm(hw, offset)        offset=a1, value in the RETVAL
	 *   wlc_bmac_write_shm(hw, offset, val)  offset=a1, val=a2 */
	{ "wlc_bmac_read_shm",  OP_MAC_OBJ_R, 1, 0, 0, .retcap = true,
	  .ripiego_di = "wlc_bmac_read_objmem" },
	{ "wlc_bmac_write_shm", OP_MAC_OBJ_W, 1, 2, 0,
	  .ripiego_di = "wlc_bmac_write_objmem" },
	/* The BULK object-memory pair, GLOBAL on both versions. It carries the
	 * SELECTOR, and with it the region: it is the only way to see what is
	 * not shared memory. `buf` (a2) is a pointer and is not recorded; its
	 * content comes from the 16-bit ops underneath when those resolve.
	 *   wlc_bmac_copyfrom_objmem(hw, offset, buf, len, sel)
	 *   wlc_bmac_copyto_objmem(hw, offset, buf, len, sel)
	 * offset=a1, len=a3; sel is the 5th argument and in o32 sits at
	 * 16(sp), so it is captured with nargx as for si_corereg.
	 * TO BE CONFIRMED on the first capture: that the selector arrives in
	 * a5. */
	{ "wlc_bmac_copyfrom_objmem", OP_MAC_OBJ_BULK_R, 1, 0, 3, .nargx = 1 },
	{ "wlc_bmac_copyto_objmem",   OP_MAC_OBJ_BULK_W, 1, 0, 3, .nargx = 1 },
	/* Chanspec in shared memory. The symbol exists on 6.30 too. */
	{ "wlc_phy_chanspec_shm_set", OP_CHANSPEC_SHM, 1, 0, 0 },
	/* Address match: the three functions that program MAC and BSSID. They
	 * answer one precise question -- whether on the AC b43 is writing the
	 * MACFILTER port (0x0420/0x0422) while the hardware looks elsewhere --
	 * which the set of ops captured so far does not show, because that
	 * port is a direct write on the d11 core's window and goes through no
	 * named accessor.
	 *
	 * The names change between versions: on 6.30 the entry point is
	 * wlc_bmac_set_addrmatch, on 7.14 wlc_set_addrmatch. Both are listed:
	 * the one that is missing does not resolve and pianifica() says so.
	 *
	 * Index in a1 and pointer to the address in a2, read off the prologues
	 * of the 6.30 object and consistent with brcmsmac's
	 * brcms_b_set_addrmatch(). The address is behind the pointer and is
	 * not recorded. */
	/* set_addrmatch has a branch in word 2 of the prologue (the test on
	 * hw+72), so the 4-word detour does not fit: short-j. Words 0 and 1
	 * are `lw` and `sltiu`, not PC-relative and with no side effects, so
	 * re-running them in the stub is harmless.
	 * a1 = index, a2 = pointer to the address (`lbu 1($a2)` reads it). */
	{ "wlc_bmac_set_addrmatch", OP_ADDRMATCH, 1, 0, 0, .shortj = true },
	{ "wlc_set_addrmatch",      OP_ADDRMATCH, 1, 0, 0 },
	/* write_amt: a1 = index (`sll a1,1`), a3 = a signed 16-bit value the
	 * function does `bltz` on. Prologue clear. */
	{ "wlc_bmac_write_amt",     OP_AMT_W,     1, 0, 3 },
	{ "wlc_bmac_set_rcmta",     OP_RCMTA_W,   1, 0, 0 },
	/* Accessors found in the blobs of both versions and not covered by the
	 * hooks above:
	 *
	 *   phy_reg_write_array   bulk PHY write, the accessor that goes by the
	 *                         name phy_reg_write_list in other trees.
	 *   phy_reg_read/write_wide   32-bit PHY access. The 16-bit hooks do
	 *                         not intercept it.
	 *   wlc_bmac_write_ihr    the d11 core's Indirect Hardware Registers.
	 *                         objmem reaches them by selector, but a
	 *                         dedicated writer writes them without going
	 *                         through it.
	 *   wlc_bmac_set_shm      masked write to shared memory.
	 *
	 * Signatures read off the prologues of the 6.30 object, not assumed --
	 * and most were not what the name suggested:
	 *
	 *   phy_reg_write_array(pi, array, n)   has NO address: a1 is a pointer
	 *       to the array and a2 the count (a `blez a2` gives it away). Only
	 *       n is recorded. Inside it calls phy_reg_and & co., so the
	 *       individual writes are already visible from the 16-bit hooks and
	 *       this is a marker -- like TBL.WR for tables.
	 *   phy_reg_write_wide(pi, val)   has NO address: fixed register, value
	 *       in a1 masked to 16 bits. Like wlc_bmac_mctrl.
	 *   phy_reg_read_wide(pi)   no useful argument, value in the RETVAL.
	 *   wlc_bmac_write_ihr(hw, off, val)   off=a1, val=a2; there is no a3.
	 *   wlc_bmac_set_shm(hw, off, val, len)   off=a1, val=a2, len=a3: it is
	 *       a memset over shared memory, and in the loop it calls the
	 *       16-bit accessor.
	 *
	 * A candidate that does not get hooked does no harm: pianifica()
	 * notices from the prologue and skips it with a pr_warn. */
	{ "phy_reg_write_array", OP_PHY_WARR, 0, 2, 0 },
	{ "phy_reg_read_wide",   OP_PHY_RDW,  0, 0, 0, .retcap = true },
	{ "phy_reg_write_wide",  OP_PHY_WRW,  0, 1, 0 },
	{ "wlc_bmac_write_ihr",  OP_IHR_W,    1, 2, 0 },
	{ "wlc_bmac_set_shm",    OP_OBJ_SET,  1, 2, 3 },
	/* branch in slot 3 (beq): the classic 4-word detour is impossible.
	 * short-j: o[0]=j stub; o[1] (addiu $v0,1) stays as the delay slot; the
	 * stub re-runs o[0..1] and returns to +8 ($v0 set again AFTER the
	 * hook). addr=a1 raw (the andi 0xffff is o[0], re-run in the stub). */
	{ "read_radio_reg",     OP_RADIO_R,   1, 0, 0, .shortj = true, .retcap = true },
	/* The frames wl posts to a TX ring, d11 TX header in front: what the
	 * DMA engine is handed, the PHY TX control words of data frames
	 * included, which no register access carries. pkt_rec() reads them;
	 * nothing is recorded until txdump is set. addr_src is the argument
	 * that holds the osl packet (or the buffer, with txraw), val_src the
	 * length of a raw buffer. Signatures of the GPL sources:
	 *
	 *   dma64_txfast(di, p0, commit)            hnddma, p0=a1
	 *   dma64_txunframed(di, buf, len, commit)  hnddma, buf=a1, len=a2
	 *   wlc_txfifo(wlc, fifo, p, ...)           brcmsmac's brcms_c_txfifo,
	 *                                           p=a2
	 *
	 * hnddma's are LOCAL, reached through its function table, so call
	 * sites cannot take them; where the module keeps no local symbols, as
	 * the 7.14.43.21 wl_vd625.ko, they do not resolve at all. wlc_txfifo is
	 * GLOBAL and is where wlc hands a frame to its fifo, the header pushed:
	 * the fallback, dropped when dma64_txfast hooks.
	 *
	 * TO BE CONFIRMED on the first capture: that the packet is an sk_buff,
	 * as linux_osl.h makes it, and that p is a2 in this wl's wlc_txfifo.
	 * The decoder checks the header's frame_len against the length, so a
	 * wrong guess shows as an unrecognised layout and not as plausible
	 * fields. */
	{ "dma64_txfast",       OP_TX_PKT,    1, 0, 0 },
	{ "dma64_txunframed",   OP_TX_PKT,    1, 2, 0, .txraw = true },
	{ "wlc_txfifo",         OP_TX_PKT,    2, 0, 0,
	  .ripiego_di = "dma64_txfast" },
	/* The frames the bmac layer hands to wlc, d11 RX header in front:
	 * frame length, PHY and MAC RX status, then the PLCP (the HT-SIG or
	 * VHT-SIG-A b43_rx_rate_ac() reads) and the frame. pkt_rec() reads
	 * them; nothing is recorded until rxdump is set. Signature of the GPL
	 * sources:
	 *
	 *   wlc_recv(wlc, p)              brcmsmac's brcms_c_recv, p=a1,
	 *                                 p->data at the RX header
	 *
	 * GLOBAL, in .text.fastpath in wl_vd625.ko. TO BE CONFIRMED on the
	 * first capture, like the TX post: p in a1 and the header still in
	 * front of the frame at the entry. The decoder checks the header's
	 * frame length against the packet's. */
	{ "wlc_recv",           OP_RX_PKT,    1, 0, 0 },
	/* The TX status wlc gets for each frame: txs_rec() reads the start of
	 * the tx_status structure; nothing is recorded until txsdump is set.
	 * Signature from the 6.30 prologue (wlDSL-3580_EU.o_save): txs in a1,
	 * whose byte 3 the function masks with 7 for the fifo, the low byte of
	 * the frame ID:
	 *
	 *   wlc_dotxstatus(wlc, txs, ...)
	 *
	 * a2 is recorded as it comes. GLOBAL, so it
	 * resolves where the module keeps no local symbols. The structure's
	 * layout differs between versions and is recorded raw: the decoder
	 * finds the frame ID in it among those recorded at the TX post. */
	{ "wlc_dotxstatus",     OP_TXS,       1, 2, 0 },
};
#define NHOOK ARRAY_SIZE(hooks)

static inline u32 pick(u8 src, u32 a1, u32 a2, u32 a3)
{
	return src == 1 ? a1 : src == 2 ? a2 : src == 3 ? a3 : 0;
}
/*
 * One frame at the TX post or at the RX hand-over, with the start of its
 * bytes: a TX.PKT / RX.PKT record (addr = the length, val = the bytes that
 * follow, aux = PKT_F_*) and up to txdump / rxdump bytes of the frame in
 * TX.DATA / RX.DATA records, one group (emit_group). Off with the dump
 * length at 0; the budget counts the frames left and is written again for
 * more. All four can be changed while tracing. Only the linear part of an
 * sk_buff is read, where the driver keeps the d11 header; every read goes
 * through probe_kernel_read(), so a pointer that is not what the signature
 * says costs the record.
 */
#define PKTDUMP_MAX 256
#define PKT_F_RAW	1	/* aux: a raw buffer, not an osl packet */
#define PKT_F_TAGGED	2	/* aux: a tagged nbuff pointer, not read */

static uint txdump;
module_param(txdump, uint, 0644);
MODULE_PARM_DESC(txdump, "bytes of each frame posted to a TX ring to record, up to 256; 0=off (default). 168 covers the TX offload header, the d11 TX header and an 802.11 header");

static uint txbudget = 256;
module_param(txbudget, uint, 0644);
MODULE_PARM_DESC(txbudget, "TX frames left to record; counts down to 0 (default 256)");

static uint rxdump;
module_param(rxdump, uint, 0644);
MODULE_PARM_DESC(rxdump, "bytes of each received frame to record, up to 256; 0=off (default). 96 covers the d11 RX header, the PLCP and an 802.11 header");

static uint rxbudget = 256;
module_param(rxbudget, uint, 0644);
MODULE_PARM_DESC(rxbudget, "RX frames left to record; counts down to 0 (default 256)");

static u32 pkt_rec(const struct hook *h, u32 a1, u32 a2, u32 a3,
		   u8 op, u8 dop, uint want, uint *budget)
{
	u32 p = pick(h->addr_src, a1, a2, a3);
	const u8 *data;
	u32 len, lin, n;
	uint left;

	if (!want || !p)
		return 0;
	do {
		left = ACCESS_ONCE(*budget);
		if (!left)
			return 0;
	} while (cmpxchg(budget, left, left - 1) != left);

	if (h->txraw) {
		data = (const u8 *)(unsigned long)p;
		len = lin = pick(h->val_src, a1, a2, a3);
	} else {
		const struct sk_buff *skb = (const void *)(unsigned long)p;
		unsigned int l, dl;

		/* Broadcom's CPE kernels pass an FkBuff as an nbuff pointer
		 * tagged in its low bits; this wl imports fkb_xlate and
		 * fkb_free. An sk_buff is aligned, so a tagged pointer is
		 * recorded as such, with no bytes, and not read. */
		if (p & 3)
			return emit_group(op, 0, 0, PKT_F_TAGGED, dop, NULL, 0);

		if (probe_kernel_read(&data, &skb->data, sizeof(data)) ||
		    probe_kernel_read(&l, &skb->len, sizeof(l)) ||
		    probe_kernel_read(&dl, &skb->data_len, sizeof(dl)))
			return 0;
		len = l;
		lin = dl <= l ? l - dl : 0;
	}
	n = min_t(u32, want, lin);
	n = min_t(u32, n, PKTDUMP_MAX);
	if (!readable(data, n))
		n = 0;
	return emit_group(op, len, n, h->txraw ? PKT_F_RAW : 0, dop, data, n);
}

/*
 * One write to template RAM with its content: the TPL.RAMW record as before
 * (addr = offset, val = length in bytes) with aux = the bytes that follow in
 * TPL.DATA records, one group (emit_group). The beacon and the probe
 * response templates go through here, PLCP and header in front of the
 * frame, and so do the PHY's tone waveforms. Off with tpldump 0, when the
 * record carries no content and aux is 0; tplbudget counts the writes left.
 * Both can be changed while tracing.
 *
 *   wlc_bmac_write_template_ram(hw, offset, len, buf)  offset=a1, len=a2,
 *                                                      buf=a3
 */
#define TPLDUMP_MAX 1024

static uint tpldump;
module_param(tpldump, uint, 0644);
MODULE_PARM_DESC(tpldump, "bytes of each template RAM write to record, up to 1024; 0=off (default). A beacon template is up to 512");

static uint tplbudget = 256;
module_param(tplbudget, uint, 0644);
MODULE_PARM_DESC(tplbudget, "template RAM writes left to record; counts down to 0 (default 256)");

/*
 * One TX status with the start of its structure: a TXS record (addr = a2
 * at the entry, val = the bytes that follow) and up to txsdump bytes
 * of the tx_status in TXS.DATA records, one group (emit_group). Off with
 * txsdump 0; txsbudget counts the statuses left. Both can be changed while
 * tracing.
 */
#define TXSDUMP_MAX 128

static uint txsdump;
module_param(txsdump, uint, 0644);
MODULE_PARM_DESC(txsdump, "bytes of each TX status structure to record, up to 128; 0=off (default). Its size differs between versions: 64 to start with");

static uint txsbudget = 256;
module_param(txsbudget, uint, 0644);
MODULE_PARM_DESC(txsbudget, "TX statuses left to record; counts down to 0 (default 256)");

static u32 txs_rec(u32 txs, u32 a2)
{
	const u8 *src = (const u8 *)(unsigned long)txs;
	u32 n = min_t(u32, ACCESS_ONCE(txsdump), TXSDUMP_MAX);
	uint left;

	if (!n || !txs)
		return 0;
	do {
		left = ACCESS_ONCE(txsbudget);
		if (!left)
			return 0;
	} while (cmpxchg(&txsbudget, left, left - 1) != left);

	if (!readable(src, n))
		n = 0;
	return emit_group(OP_TXS, a2, n, 0, OP_TXS_DATA, src, n);
}

static u32 tpl_rec(u32 off, u32 len, u32 buf)
{
	const u8 *src = (const u8 *)(unsigned long)buf;
	u32 n = min_t(u32, ACCESS_ONCE(tpldump), len);
	uint left;

	n = min_t(u32, n, TPLDUMP_MAX);
	if (!n || !buf)
		return emit(OP_TPL_RAMW, off, len, 0);
	do {
		left = ACCESS_ONCE(tplbudget);
		if (!left)
			return emit(OP_TPL_RAMW, off, len, 0);
	} while (cmpxchg(&tplbudget, left, left - 1) != left);

	if (!readable(src, n))
		n = 0;
	return emit_group(OP_TPL_RAMW, off, len, n, OP_TPL_DATA, src, n);
}

/* Landing point of the detour: called from the stub with (id, a1, a2, a3). */
u32 __used noinline
wl_diag_hook(u32 id, u32 a1, u32 a2, u32 a3)
{
	struct hook *h = &hooks[id];

	/* CAL.INIT carries no payload: it says WHEN cal_init was invoked, and
	 * it is also the anchor for segmenting a sweep, one per cycle. */
	if (h->op == OP_CAL_INIT)
		return emit(h->op, 0, 0, 0);
	if (h->op == OP_TX_PKT)
		return pkt_rec(h, a1, a2, a3, OP_TX_PKT, OP_TX_DATA,
			       ACCESS_ONCE(txdump), &txbudget);
	if (h->op == OP_RX_PKT)
		return pkt_rec(h, a1, a2, a3, OP_RX_PKT, OP_RX_DATA,
			       ACCESS_ONCE(rxdump), &rxbudget);
	if (h->op == OP_TXS)
		return txs_rec(pick(h->addr_src, a1, a2, a3),
			       pick(h->val_src, a1, a2, a3));
	if (h->op == OP_TPL_RAMW)
		return tpl_rec(a1, a2, a3);

	return emit(h->op, pick(h->addr_src, a1, a2, a3),
			   pick(h->val_src,  a1, a2, a3),
			   pick(h->aux_src,  a1, a2, a3));
}

/* Follow-on record for stack arguments (o32): a second ARGX record tied to
 * the main one through parent_seq. addr=arg5, val=arg6. */
void __used noinline
wl_diag_hook_argx(u32 parent_seq, u32 x1, u32 x2)
{
	emit(OP_ARGX, x1, x2, parent_seq);
}

/* ---- return value capture (retcap): trampoline on 'ra' ---------------- *
 * The origin of 'ra' is saved PER INVOCATION in a pool indexed by 'current'  *
 * (the task): it survives preemption and migration (SMP+PREEMPT), unlike a   *
 * per-CPU slot. LIFO, to handle nesting (a hooked read that calls another).  *
 * Pool full -> NO diversion (no crash, only that value is lost). */
struct ret_inst {
	struct task_struct *task;
	unsigned long orig_ra;
	u32 seq;
	u32 order;
};
#define RET_POOL 64
static struct ret_inst ret_pool[RET_POOL];
static DEFINE_SPINLOCK(ret_lock);
static u32 ret_order;
static unsigned long ret_trampoline;	/* address of the shared return stub */

/* entry of a retcap: records (current, orig_ra, seq); returns the 'ra'
 * address to install (the trampoline if there is room, otherwise orig_ra). */
unsigned long __used noinline
wl_diag_enter_ret(unsigned long orig_ra, u32 seq)
{
	unsigned long f;
	int i;

	if (!ret_trampoline)
		return orig_ra;
	spin_lock_irqsave(&ret_lock, f);
	for (i = 0; i < RET_POOL; i++) {
		if (!ret_pool[i].task) {
			ret_pool[i].task = current;
			ret_pool[i].orig_ra = orig_ra;
			ret_pool[i].seq = seq;
			ret_pool[i].order = ++ret_order;
			spin_unlock_irqrestore(&ret_lock, f);
			return ret_trampoline;
		}
	}
	spin_unlock_irqrestore(&ret_lock, f);
	return orig_ra;
}

/* return of a retcap: takes current's instance LIFO, emits RETVAL(seq,
 * retval) and returns orig_ra. Called only if enter diverted -> by
 * construction the instance exists; defensive guard if best<0. */
unsigned long __used noinline
wl_diag_exit_ret(u32 retval)
{
	unsigned long f, ra = 0;
	int i, best = -1;
	u32 bestord = 0, seq = 0;

	spin_lock_irqsave(&ret_lock, f);
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
	spin_unlock_irqrestore(&ret_lock, f);
	if (best >= 0)
		emit(OP_RETVAL, seq, retval, 0);
	return ra;
}

/* ---- MIPS o32 mini-assembler (encodings checked) ---------------------- */
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
static inline u32 i_lui(u8 rt, u16 im){ return (0x0fu<<26)|(rt<<16)|im; }
static inline u32 i_ori(u8 rt, u8 rs, u16 im){ return (0x0du<<26)|(rs<<21)|(rt<<16)|im; }
static inline u32 i_jalr(u8 rs){ return (rs<<21)|(R_RA<<11)|0x09u; }
static inline u32 i_jr(u8 rs){ return (rs<<21)|0x08u; }
static inline u32 i_j(unsigned long tgt){ return (0x02u<<26)|(u32)((tgt>>2)&0x03ffffffu); }
#define I_NOP 0u

/* true if the opcode is a branch/jump (not relocatable as is into the stub) */
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

/* ---- executable stub pool (static: lives in the module, never freed) --- */
#define STUB_WORDS 48
static u32 stub_pool[NHOOK][STUB_WORDS] __attribute__((aligned(8)));
static u32 ret_tramp[16] __attribute__((aligned(8)));	/* shared return trampoline */

/* No break path on this kernel: do_bp() goes straight to do_trap_or_bp() --
 * a panic in kernel mode -- and set_except_vector is not exported. The tell
 * would be BRK_KPROBE_BP, which break.h defines only where the switch with
 * notify_die exists too. For prologues that cannot be detoured the call-site
 * patch is used, which works here and is preferable. */

/* Call-site patching. The module is -mabicalls: no jal in .text, calls are
 * lui/addiu + jalr (or jr $t9 for tail calls), so the pair is rewritten to
 * load the stub. The function stays intact: no constraint on the prologue,
 * and it works on 2.6.30 where there is no break.
 * One stub serves both jalr and jr: it preserves ra and jumps to the real
 * function.
 * Three conditions, checked at run time: the pair must give the exact
 * address; a jump on the SAME register must follow it; the addiu must not be
 * shared (the compiler reuses the low half between different sites: in the
 * D6220 blob the one at +0x1f5a24 serves two functions). */
#define MAX_SITES 8

struct site {
	u32 *hi, *lo;
	u32 saved_hi, saved_lo;
};
static struct site sites[NHOOK][MAX_SITES];
static int n_sites[NHOOK];

/* immediates of a lui/addiu pair loading `v`: the addiu sign-extends the
 * low half, so the high half has to compensate. */
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
			/* a jump on the same register within 8 instructions */
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

	/* drop the sites that share the addiu with another */
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

/* If the target module goes away while armed, our pointers stay on freed
 * memory and the restore at unload would write there. On 2.6.30 the PCI
 * rescan unloads wl, so it really happens. The notifier disarms at GOING,
 * while the text is still mapped. */
static struct module *target_mod;
static bool mod_nb_registered;

/* NO reference on the target, on purpose. Holding one would make `rmmod wl`
 * fail with -EBUSY, and on 2.6.30 it is the vendor's userspace that does the
 * rmmod at the rescan -- besides being the core step of a cold capture.
 * Safety comes from the notifier, which disarms at GOING while the text is
 * still mapped: in delete_module() the notification arrives after
 * mod->exit() and before free_module(). */

static void build_stub(int idx)
{
	u32 *s = stub_pool[idx];
	u32 *o = (u32 *)hooks[idx].addr;
	unsigned long hookfn = (unsigned long)&wl_diag_hook;
	unsigned long argxfn = (unsigned long)&wl_diag_hook_argx;
	unsigned long enterfn = (unsigned long)&wl_diag_enter_ret;
	/* on the sites the function is intact: nothing to re-run, enter at 0 */
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

	/* Stack arguments (o32): arg5@16(entry)=48(sp), arg6@20=52(sp).
	 * wl_diag_hook_argx(seq, arg5, arg6) -> follow-on ARGX record. */
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

	/* retcap: wl_diag_enter_ret(orig_ra, seq) -> v0 = ra to install
	 * (the trampoline if there is room, else orig_ra = no diversion). */
	if (hooks[idx].retcap) {
		s[n++] = i_lw(R_A0, R_SP, 16);			/* orig_ra */
		s[n++] = i_lw(R_A1, R_SP, 20);			/* seq */
		s[n++] = i_lui(R_T9, enterfn >> 16);
		s[n++] = i_ori(R_T9, R_T9, enterfn & 0xffff);
		s[n++] = i_jalr(R_T9);
		s[n++] = I_NOP;
		s[n++] = i_sw(R_V0, R_SP, 24);			/* ra to install */
	}

	s[n++] = i_lw(R_A0, R_SP, 0);
	s[n++] = i_lw(R_A1, R_SP, 4);
	s[n++] = i_lw(R_A2, R_SP, 8);
	s[n++] = i_lw(R_A3, R_SP, 12);
	s[n++] = i_lw(R_RA, R_SP, hooks[idx].retcap ? 24 : 16);
	s[n++] = i_addiu(R_SP, R_SP, 32);
	for (k = 0; k < rep; k++)
		s[n++] = o[k];	/* re-run the displaced words (short-j: o[1] sets v0 again) */
	s[n++] = i_lui(R_T9, ret >> 16);
	s[n++] = i_ori(R_T9, R_T9, ret & 0xffff);
	s[n++] = i_jr(R_T9);
	s[n++] = I_NOP;
	/* max (classic+retcap+nargx) == 40 <= STUB_WORDS */
}

/* Shared return trampoline: a retcap-hooked function does jr ra with
 * ra == here. It reads $v0 (the return value), hands it to wl_diag_exit_ret,
 * which emits RETVAL and returns orig_ra, then jumps to orig_ra with $v0
 * preserved. */
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

/* writes the entry, head word LAST (classic) or a single 'j' (short-j) */
static void patch_entry(int idx)
{
	u32 *o = (u32 *)hooks[idx].addr;
	unsigned long stub = (unsigned long)stub_pool[idx];

	if (hooks[idx].shortj) {
		/* atomic 1-word patch: o[0]=j stub. o[1] stays (delay slot,
		 * re-run by the stub too). Needs the stub in the j's 256MB
		 * region (checked in pianifica()). */
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

/* ---- /proc/wl_diag --------------------------------------------------- */
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
		r.ts_ns = wldiag_now_ns();
		r.op = OP_DROP;
		r.aux = d;
		if (copy_to_user(ubuf, &r, sizeof(r)))
			return -EFAULT;
		return sizeof(r);
	}

	for (;;) {
		spin_lock_irqsave(&fifo_lock, flags);
		if (ring_head != ring_tail) {
			r = ring[ring_tail & (fifo_recs - 1)];
			ring_tail++;
			ret = 1;
		} else {
			ret = 0;
		}
		spin_unlock_irqrestore(&fifo_lock, flags);
		if (ret)
			break;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(rq,
				(ring_head != ring_tail) || atomic_read(&drops)))
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
	if ((ring_head != ring_tail) || atomic_read(&drops))
		return POLLIN | POLLRDNORM;
	return 0;
}

/* Cycle label from userspace: `echo "ch36 bw20" > /proc/wl_diag`. The record
 * goes into the queue like every other, so it is ordered with the ops and
 * not with the clock of whoever writes it. */
static ssize_t wd_write(struct file *f, const char __user *ubuf, size_t len,
			loff_t *off)
{
	char b[12];
	size_t n = len < sizeof(b) ? len : sizeof(b);
	int i;

	memset(b, 0, sizeof(b));
	if (n && copy_from_user(b, ubuf, n))
		return -EFAULT;
	for (i = 0; i < (int)sizeof(b); i++)
		if (b[i] == '\n' || b[i] == '\r')
			b[i] = 0;
	emit_mark(b);
	return len;
}

static const struct file_operations wd_fops = {
	.owner = THIS_MODULE,
	.read = wd_read,
	.write = wd_write,
	.poll = wd_poll,
	.llseek = no_llseek,
};
/* The buffer is in /proc/wl_diag: it appears by itself and needs no mknod,
 * while a misc device with a dynamic minor would need the minor read from
 * /proc/misc and created by hand at every load.
 * proc_create has the same signature on 2.6.30 and 3.4 and takes
 * file_operations, so the same call works on both. */
#define WD_PROC "wl_diag"

/* ---- plan, arming, disarming ----------------------------------------- */
static int eligible[NHOOK];   /* hookable indices */
static int n_elig;

/* Restores the patched prologues and sites. Idempotent: called both at
 * wl_diag's unload and at the target's GOING. */
static void disarma(void)
{
	int i;
	bool qualcuno = false;

	/* Stop new diversions of ra before restoring the prologues; stubs in
	 * flight that have already diverted return through ret_tramp anyway
	 * (static, valid), and synchronize_sched waits for them to finish. */
	ret_trampoline = 0;

	for (i = 0; i < NHOOK; i++)
		if (hooks[i].armed) {
			qualcuno = true;
			if (hooks[i].use_sites) {
				restore_sites(i);
				hooks[i].armed = false;
				continue;
			}
			restore_entry(i);
			hooks[i].armed = false;
		}
	if (qualcuno)
		synchronize_sched();
}

/* Clears the plan. When the target is reloaded the addresses change, and
 * re-patching the old ones would be silent and fatal: the plan is rebuilt
 * from scratch at every COMING. The trace buffer is NOT cleared, so the
 * segments of a sweep stay in the same run. */
static void azzera_piano(void)
{
	int i, j;

	for (i = 0; i < NHOOK; i++) {
		hooks[i].addr = 0;
		hooks[i].armed = false;
		hooks[i].use_sites = false;
		for (j = 0; j < 4; j++)
			hooks[i].saved[j] = 0;
		n_sites[i] = 0;
	}
	n_elig = 0;
}

static int pianifica(void);
static int arma(void);

/*
 * The notifier is the pivot of dynamic arming, not a backup defence: COMING
 * arms, GOING disarms, no exceptions. That is what a cold capture needs,
 * where every cycle is an rmmod plus an insmod of the target.
 *
 * The order of the notifications in 2.6.30 allows it, and it is the same as
 * in 3.4: in init_module() load_module() completes -- relocations applied,
 * module in the list -- then COMING arrives, then mod->init, so the hooks are
 * armed before the driver starts; in delete_module() mod->exit() runs, then
 * GOING arrives, then free_module(), so the text is still mapped at disarm.
 * 2.6.30 does not even have set_section_ro_nx, which in 3.4 runs right AFTER
 * COMING: there the writable window is very narrow, here module text is
 * always writable.
 *
 * No reference on the target: `rmmod wl` has to be able to succeed, and it is
 * the core step of a cold capture.
 */
static int wd_mod_notify(struct notifier_block *nb, unsigned long ev, void *data)
{
	struct module *m = data;

	if (!m || !target || strcmp(m->name, target))
		return NOTIFY_DONE;

	switch (ev) {
	case MODULE_STATE_COMING:
		/* A COMING without the GOING before it should not happen, but
		 * re-patching old addresses would be silent and fatal. */
		disarma();
		azzera_piano();
		target_mod = m;
		mark("mod COMING");
		if (pianifica() > 0)
			arma();
		break;
	case MODULE_STATE_GOING:
		mark("mod GOING");
		disarma();
		target_mod = NULL;
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block wd_mod_nb = { .notifier_call = wd_mod_notify };

/* Builds the plan: resolves the symbols and picks the detour strategy for
 * each. Returns the number of hookable hooks. Rebuilt at every COMING of the
 * target, because the addresses change at a reload. */
static int pianifica(void)
{
	int i;

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

		a = resolve_sym(hooks[i].name);
		if (!a) {
			pr_warn("wl_diag: '%s' non trovato (wl caricato?)\n",
				hooks[i].name);
			continue;
		}
		hooks[i].addr = a;
		o = (u32 *)a;
		win = hooks[i].shortj ? 2 : 4;	/* words touched/re-run */
		for (j = 0; j < 4; j++)
			hooks[i].saved[j] = o[j];
		for (j = 0; j < win; j++)
			if (branch < 0 && is_branch(o[j]))
				branch = j;
		if (branch >= 0 && find_sites(i, hooks[i].addr) > 0) {
			/* Cannot be detoured in the prologue, but the call
			 * sites can be patched: the function stays intact.
			 * Preferred to the break, which needs the die notifier
			 * and is not on every kernel. */
			hooks[i].use_sites = true;
			eligible[n_elig++] = i;
			pr_info("wl_diag: piano hook '%s' @%px [siti: %d] (branch a istr %d)\n",
				hooks[i].name, o, n_sites[i], branch);
			continue;
		}
		if (branch >= 0) {
			pr_warn("wl_diag: salto '%s' (branch a istr %d, e nessun sito di chiamata patchabile)\n",
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
		pr_info("wl_diag: piano hook '%s' @%p%s\n", hooks[i].name, o,
			hooks[i].shortj ? " [short-j]" : "");
	}

	/* Drop the fallbacks whose accessor underneath hooked: see
	 * `ripiego_di` in struct hook for why. */
	{
		int k, j, w = 0;

		for (k = 0; k < n_elig; k++) {
			struct hook *h = &hooks[eligible[k]];
			bool sotto = false;

			for (j = 0; j < n_elig && h->ripiego_di; j++)
				if (!strcmp(hooks[eligible[j]].name,
					    h->ripiego_di)) {
					sotto = true;
					break;
				}
			if (sotto) {
				pr_info("wl_diag: salto '%s': e' il ripiego di "
					"'%s', che si e' agganciato -- "
					"altrimenti ogni op uscirebbe doppia\n",
					h->name, h->ripiego_di);
				h->addr = 0;
				continue;
			}
			eligible[w++] = eligible[k];
		}
		n_elig = w;
	}

	if (!n_elig)
		pr_err("wl_diag: nessuna funzione agganciabile\n");
	return n_elig;
}

/* Applies the plan. Assumes pianifica() has run. */
static int arma(void)
{
	int i;

	if (!arm) {
		pr_info("wl_diag: DRY-RUN (%d hook pianificati). insmod con arm=1 per applicare.\n",
			n_elig);
		return 0;
	}

	/* resolve the i-cache flush. NB: this kernel has KALLSYMS but not
	 * KALLSYMS_ALL, so kallsyms exposes TEXT symbols (functions) only: the
	 * pointer variable 'flush_icache_range' (in BSS) is invisible. The R4K
	 * cache layer function is resolved directly, with fallbacks.
	 *
	 * Only once: it is a kernel symbol, it does not move between one
	 * reload of the target and the next. */
	if (!p_flush_icache) {
		static const char * const cand[] = {
			"r4k_flush_icache_range",
			"local_r4k_flush_icache_range",
			"local_flush_icache_range",  /* a variable too: probably a miss */
		};
		int k;

		for (k = 0; k < ARRAY_SIZE(cand); k++) {
			unsigned long a = resolve_sym(cand[k]);

			if (a) {
				p_flush_icache = (flush_fn_t)a;
				pr_info("wl_diag: flush via '%s' @%p\n", cand[k], (void *)a);
				break;
			}
		}
	}
	if (!p_flush_icache) {
		pr_err("wl_diag: nessun flush i-cache risolvibile, resto in DRY-RUN\n");
		return 0;
	}

	for (i = 0; i < n_elig; i++) {
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
			pr_info("wl_diag: trampolino ritorno @%p\n",
				(void *)ret_trampoline);
		}
	}
	if (!target_mod)
		target_mod = __module_text_address(hooks[eligible[0]].addr);

	for (i = 0; i < n_elig; i++) {
		if (hooks[eligible[i]].use_sites) {
			patch_sites(eligible[i]);
			hooks[eligible[i]].armed = true;
			continue;
		}
		patch_entry(eligible[i]);
		hooks[eligible[i]].armed = true;
	}
	pr_info("wl_diag: ARMATO (%d hook) -> /proc/wl_diag\n", n_elig);
	return 0;
}

/* ---- init/exit ------------------------------------------------------- */
static int __init wd_init(void)
{
	parse_skipphyrd();

	if (ring_alloc())
		return -ENOMEM;

	if (!proc_create(WD_PROC, 0600, NULL, &wd_fops)) {
		pr_err("wl_diag: proc_create(/proc/%s) fallita\n", WD_PROC);
		vfree(ring);
		return -ENOMEM;
	}

	/* Registered BEFORE planning: if the target is not loaded yet, there
	 * is no plan and the COMING will make it. Not an error, and it is the
	 * normal case of a cold capture, where wl_diag is loaded first. */
	if (register_module_notifier(&wd_mod_nb))
		pr_warn("wl_diag: register_module_notifier fallita: senza di essa "
			"non si arma sui ricaricamenti del bersaglio, e la "
			"cattura a freddo non funziona\n");
	else
		mod_nb_registered = true;

	if (pianifica() > 0)
		arma();
	else
		pr_info("wl_diag: nessun piano ora; in attesa del COMING di '%s'\n",
			target);
	return 0;
}

static void __exit wd_exit(void)
{
	if (mod_nb_registered) {
		unregister_module_notifier(&wd_mod_nb);
		mod_nb_registered = false;
	}
	disarma();
	remove_proc_entry(WD_PROC, NULL);
	vfree(ring);
	pr_info("wl_diag: scaricato (persi: %d, filtrati: %d)\n",
		atomic_read(&drops), atomic_read(&filtered));
}

module_init(wd_init);
module_exit(wd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Inline-detour PHY/radio/PMU tracer for Broadcom wl (no kprobes)");
