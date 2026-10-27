/*
 * wl_diag, variant for kernel 4.4 on ARM (Broadcom Northstar: BCM4709,
 * Cortex-A9 SMP, little-endian), with kprobes.
 *
 * The record format, the op codes, the queue, the record formatters and
 * /proc/wl_diag are those of ../3-4-11/wl_diag.c, copied, and so is the hook
 * table, whose signatures are commented there: see its head and
 * ../README.md. What differs is how a hook is attached. That kernel has no
 * CONFIG_KPROBES and patches the accessors' prologues; this one requires
 * CONFIG_KPROBES (CONFIG_KRETPROBES follows from it on ARM) and puts a
 * kprobe at the entry of each accessor, or a kretprobe where the value read
 * is wanted (retcap).
 *
 * The ARM calling convention passes the first four arguments in r0-r3, as
 * o32 does in a0-a3, and the fifth and sixth at [sp] and [sp + 4] at the
 * entry, so the table's a1, a2, a3 are r1, r2, r3 and nargx reads the stack
 * there.
 *
 * Symbols are resolved as "<target>:<name>", in the target module only. A
 * name the module does not have is "absent" in the plan; one kprobes
 * refuses is "refused", with the error. arm=0 logs the plan and attaches
 * nothing.
 *
 * Arming happens at the target's MODULE_STATE_COMING, after its symbols are
 * in kallsyms and before its init runs: kprobes accepts a COMING module's
 * text, init text included (check_kprobe_address_safe()), so the probe and
 * the attach are traced. Disarming happens at GOING, after mod->exit(), so
 * the detach is traced too. A target already loaded at insmod is armed
 * straight away.
 *
 * The queue holds records in host order; wd_read() hands them out
 * big-endian, the stream format of ../decode-wl-diag.py, so the stream is
 * the one the MIPS variants write. Timestamps are local_clock():
 * sched_clock() is not exported to modules on ARM.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/kfifo.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <linux/notifier.h>

#if !defined(CONFIG_KPROBES) || !defined(CONFIG_KRETPROBES)
#error "wl_diag on ARM needs CONFIG_KPROBES and CONFIG_KRETPROBES"
#endif

static int arm;
module_param(arm, int, 0444);
MODULE_PARM_DESC(arm, "0=dry run (log the plan only), 1=attach the probes");

static char *target = "wl";
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "name of the module to hook (default wl)");

static int delay;
module_param(delay, int, 0444);
MODULE_PARM_DESC(delay, "0=do not hook osl_delay (default), 1=hook it");

/* Concurrent calls of one retcap accessor whose return can be waited for;
 * past it the value of that call is lost (counted in the kretprobe's
 * nmissed, logged at disarm). */
static int maxactive = 64;
module_param(maxactive, int, 0444);
MODULE_PARM_DESC(maxactive, "kretprobe instances per retcap hook (default 64)");

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
	OP_PHY_FGC,					/* 51 (append) */
	OP_IOCTL, OP_IOVAR_NAME, OP_IOVAR_SET,		/* 52,53,54 (append) */
	OP_TX_PKT, OP_TX_DATA,				/* 55,56 (append) */
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
 * The queue is allocated at init, so its size is a parameter and a failed
 * allocation shows up straight away instead of inflating the module's .bss.
 * It sits on a vmalloc'd buffer, not on kfifo_alloc: __kfifo_alloc uses
 * kmalloc, which wants physically CONTIGUOUS pages, and on a fragmented router
 * an allocation of a few MB fails. vmalloc has no such constraint. kfifo_init
 * takes the buffer ready-made and puts the queue head on top of it.
 *
 * Why it has to be large: the peak rate seen on the DSL captures is 5200
 * records/s (the 80 MHz phase, with RETVALs on), and with 32768 records the
 * margin was ~6 s. That was not enough: that phase lost 1873 records in 232
 * drips of 2-3. The default of 131072 brings the margin to ~25 s; 262144 to
 * ~50 s, which is a whole channel cycle.
 *
 * MIND what this fixes: a bigger buffer absorbs BURSTS, not an average rate
 * above the drain. The 232 drips of 2-3 records in the 80 MHz phase say the
 * queue was full several times, that is, the reader was on average slower than
 * the writer: in that case no size is enough and the read side is what to look
 * at (TCP, `cat`), or filter more with skipphyrd.
 *
 * Cost: 28 bytes per record, so 131072 records are 3.5 MB of unpageable kernel
 * memory, paid only while the module is loaded. If the allocation fails, lower
 * the parameter: 65536 is 1.75 MB and ~12 s of margin.
 *
 * fifo_recs is rounded down to a power of 2: kfifo_init divides the size in
 * bytes by esize and does rounddown_pow_of_two.
 */

#define FIFO_RECS_DEF 131072
static int fifo_recs = FIFO_RECS_DEF;
module_param(fifo_recs, int, 0444);
static DECLARE_KFIFO_PTR(fifo, struct wldiag_rec);
static void *fifo_buf;
static DEFINE_RAW_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(rq);
static atomic_t seq = ATOMIC_INIT(0);
static atomic_t drops = ATOMIC_INIT(0);

/* PHY REGISTER reads not to record, to spare the fifo. It comes from the radar
 * detector polling: on DFS channels the driver reads 0x0253 and 0x0254
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
 * same-numbered PHY registers: a filter on the address alone would have thrown
 * them away in silence.
 *
 * And ONLY 0x253/0x254 are filtered. The head of the block -- 0x251 and 0x252,
 * read 1558 times in total, once per block -- is plausibly the pulse state and
 * data, which is the part that matters: it costs little and it is kept.
 *
 * Filtered records do NOT count as lost: separate counter, so OP_DROP stays an
 * indicator of real loss.
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
			pr_warn("wl_diag: skipphyrd: '%s' is not a number\n", tok);
			continue;
		}
		skip_list[skip_n++] = (u32)v;
	}
	if (skip_n)
		pr_info("wl_diag: %d PHY reads filtered by address\n", skip_n);
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

	r.ts_ns = local_clock();
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

	raw_spin_lock_irqsave(&fifo_lock, flags);
	if (kfifo_avail(&fifo) < nrec) {
		atomic_add(nrec, &drops);
		raw_spin_unlock_irqrestore(&fifo_lock, flags);
		return 0;
	}
	for (i = 0; i < nrec; i++) {
		r.ts_ns = local_clock();
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
		kfifo_in(&fifo, &r, 1);
	}
	raw_spin_unlock_irqrestore(&fifo_lock, flags);
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
 * A MARK record carries 12 characters packed into the three u32 fields, so the
 * cycle label sits INSIDE the trace and cutting it up afterwards needs no
 * heuristics on time gaps. The packing is explicitly big-endian and does not
 * depend on the endianness of the machine: the decoder undoes it by hand.
 * Twelve characters are enough for "ch140 bw80"; the excess is cut. */
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
 * Each source field takes an entry argument: 0=absent(->0), 1=a1 (r1),
 * 2=a2 (r2), 3=a3 (r3). The entries are ../3-4-11's, where each signature is
 * commented, without its prologue flags (shortj, tail_aux_src).
 */
struct hook {
	const char *name;
	u8 op, addr_src, val_src, aux_src;
	/* The accessor this hook is the fallback for: dropped if that one is
	 * attached, or every op would come out twice. */
	const char *ripiego_di;
	bool retcap;		/* a RETVAL record with what the call returns */
	u8 nargx;		/* stack arguments in an ARGX record: [sp], [sp+4] */
	bool txraw;		/* OP_TX_PKT: a1 is a buffer, a2 its length */
	/* state */
	unsigned long addr;
	bool armed;
	struct kprobe kp;
	struct kretprobe krp;
};
static struct hook hooks[] = {
	{ "phy_reg_read", OP_PHY_R, 1, 0, 0, .retcap = true },
	{ "phy_reg_write", OP_PHY_W, 1, 2, 0 },
	{ "phy_reg_mod", OP_PHY_MOD, 1, 3, 2 },
	{ "phy_reg_and", OP_PHY_AND, 1, 2, 0 },
	{ "phy_reg_or", OP_PHY_OR, 1, 2, 0 },
	{ "write_radio_reg", OP_RADIO_W, 1, 2, 0 },
	{ "mod_radio_reg", OP_RADIO_MOD, 1, 3, 2 },
	{ "si_pmu_chipcontrol", OP_PMU_CC, 1, 3, 2, .retcap = true },
	{ "si_pmu_regcontrol", OP_PMU_RC, 1, 3, 2, .retcap = true },
	{ "si_pmu_pllcontrol", OP_PMU_PLL, 1, 3, 2, .retcap = true },
	{ "si_corereg", OP_SI_COREREG,2, 0, 1, .retcap = true, .nargx = 1 },
	{ "si_gpiocontrol", OP_CC_GPIOCTL,0, 2, 1 },
	{ "si_gpioout", OP_CC_GPIOOUT,0, 2, 1 },
	{ "si_gpioouten", OP_CC_GPIOOE, 0, 2, 1 },
	{ "wlc_phy_table_read_acphy", OP_TBL_R, 1, 2, 3 },
	{ "wlc_phy_table_write_acphy", OP_TBL_W, 1, 2, 3 },
	{ "osl_delay", OP_DELAY, 0, 1, 0 },
	{ "wlc_bmac_mctrl", OP_MAC_MCTRL, 0, 2, 1 },
	{ "wlc_bmac_mhf", OP_MAC_MHF_W, 1, 3, 2, .nargx = 1 },
	{ "wlc_bmac_mhf_get", OP_MAC_MHF_R, 1, 0, 0, .retcap = true },
	{ "wlc_bmac_templateptr_wreg", OP_TPL_PTRW, 1, 0, 0 },
	{ "wlc_bmac_templatedata_wreg", OP_TPL_DATW, 1, 0, 0 },
	{ "wlc_bmac_templateptr_rreg", OP_TPL_PTRR, 0, 0, 0, .retcap = true },
	{ "wlc_bmac_templatedata_rreg", OP_TPL_DATR, 0, 0, 0, .retcap = true },
	{ "wlc_bmac_write_template_ram", OP_TPL_RAMW, 1, 2, 0 },
	{ "otp_init", OP_OTP_INIT, 0, 0, 0, .retcap = true },
	{ "otp_read_word", OP_OTP_RDW, 1, 0, 2, .retcap = true },
	{ "otp_read_region", OP_OTP_RDR, 1, 0, 3, .retcap = true },
	{ "wlc_phy_cal_init", OP_CAL_INIT, 0, 0, 0 },
	{ "wlc_bmac_bw_set", OP_MAC_BW, 0, 1, 0 },
	{ "si_get_sromctl", OP_SROMCTL_R, 0, 0, 0, .retcap = true },
	{ "si_set_sromctl", OP_SROMCTL_W, 0, 1, 0 },
	{ "wlc_phy_chanspec_shm_set", OP_CHANSPEC_SHM, 1, 0, 0 },
	{ "wlc_bmac_read_objmem16", OP_MAC_OBJ_R, 1, 0, 2, .retcap = true },
	{ "wlc_bmac_write_objmem16", OP_MAC_OBJ_W, 1, 2, 3 },
	{ "wlc_bmac_copyfrom_objmem", OP_MAC_OBJ_BULK_R, 1, 0, 3, .nargx = 1 },
	{ "wlc_bmac_copyto_objmem", OP_MAC_OBJ_BULK_W, 1, 0, 3, .nargx = 1 },
	{ "wlc_bmac_set_addrmatch", OP_ADDRMATCH, 1, 0, 0 },
	{ "wlc_set_addrmatch", OP_ADDRMATCH, 1, 0, 0 },
	{ "wlc_bmac_set_rxe_addrmatch", OP_ADDRMATCH, 1, 0, 0 },
	{ "wlc_bmac_write_amt", OP_AMT_W, 1, 0, 3 },
	{ "wlc_bmac_set_rcmta", OP_RCMTA_W, 1, 0, 0 },
	{ "phy_reg_write_array", OP_PHY_WARR, 0, 2, 0 },
	{ "phy_reg_read_wide", OP_PHY_RDW, 0, 0, 0, .retcap = true },
	{ "phy_reg_write_wide", OP_PHY_WRW, 0, 1, 0 },
	{ "wlc_bmac_write_ihr", OP_IHR_W, 1, 2, 0 },
	{ "wlc_bmac_set_shm", OP_OBJ_SET, 1, 2, 3 },
	{ "wlc_bmac_phyclk_fgc", OP_PHY_FGC, 0, 1, 0 },
	{ "wlc_ioctl", OP_IOCTL, 1, 2, 3 },
	{ "wlc_bmac_read_shm", OP_MAC_OBJ_R, 1, 0, 0, .retcap = true, .ripiego_di = "wlc_bmac_read_objmem16" },
	{ "wlc_bmac_write_shm", OP_MAC_OBJ_W, 1, 2, 0, .ripiego_di = "wlc_bmac_write_objmem16" },
	{ "read_radio_reg", OP_RADIO_R, 1, 0, 0, .retcap = true },
	{ "dma64_txfast", OP_TX_PKT, 1, 0, 0 },
	{ "dma64_txunframed", OP_TX_PKT, 1, 2, 0, .txraw = true },
	{ "wlc_txfifo", OP_TX_PKT, 2, 0, 0, .ripiego_di = "dma64_txfast" },
	{ "wlc_recv", OP_RX_PKT, 1, 0, 0 },
	{ "wlc_dotxstatus", OP_TXS, 1, 2, 0 },
};
#define NHOOK ARRAY_SIZE(hooks)

static inline u32 pick(u8 src, u32 a1, u32 a2, u32 a3)
{
	return src == 1 ? a1 : src == 2 ? a2 : src == 3 ? a3 : 0;
}

/*
 * One wlc_ioctl() call as records. The two variable commands carry a string:
 *
 *   WLC_GET_VAR (262)  "name\0" + room for the answer. Not recorded: the value
 *                      only exists after the call, and the name alone says
 *                      that somebody asked, not what the driver runs with.
 *   WLC_SET_VAR (263)  "name\0" + value. The name goes out in IOVAR_NAME
 *                      records, twelve bytes each packed big-endian like MARK,
 *                      then one IOVAR_SET with the first u32 of the value in
 *                      the driver's byte order (addr) and the value's length
 *                      (val). Names are cut at 36 bytes; past that the value
 *                      is not read.
 *
 * Every other command is one IOCTL record: cmd, the first u32 of the payload
 * (0 below four bytes) and the length. The payload is read with
 * probe_kernel_read(), so a bad pointer costs a record, not an oops.
 */
#define WD_WLC_GET_VAR	262
#define WD_WLC_SET_VAR	263
#define WD_IOVAR_NAME_MAX	36

static u32 ioctl_rec(u32 cmd, u32 buf, u32 len)
{
	u8 b[WD_IOVAR_NAME_MAX + 1 + 4];
	u32 n = len < sizeof(b) ? len : sizeof(b);
	u32 nl, i, v = 0;

	if (cmd == WD_WLC_GET_VAR)
		return 0;
	if (!buf || probe_kernel_read(b, (void *)(unsigned long)buf, n))
		n = 0;

	if (cmd != WD_WLC_SET_VAR) {
		if (n >= 4)
			memcpy(&v, b, 4);
		return emit(OP_IOCTL, cmd, v, len);
	}

	for (nl = 0; nl < n && nl < WD_IOVAR_NAME_MAX && b[nl]; nl++)
		;
	for (i = 0; i < nl; i += 12) {
		u32 w[3] = { 0, 0, 0 };
		u32 k;

		for (k = 0; k < 12 && i + k < nl; k++)
			w[k / 4] |= (u32)b[i + k] << (24 - 8 * (k % 4));
		emit(OP_IOVAR_NAME, w[0], w[1], w[2]);
	}
	if (nl < n && !b[nl] && nl + 1 + 4 <= n)
		memcpy(&v, b + nl + 1, 4);
	return emit(OP_IOVAR_SET, v, len > nl + 1 ? len - nl - 1 : 0, 0);
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

/* ---- one call of an accessor ------------------------------------------ */

/* The call's records; returns the main record's seq, 0 if none went out. */
static u32 wd_hook(const struct hook *h, struct pt_regs *regs)
{
	u32 a1 = regs->ARM_r1, a2 = regs->ARM_r2, a3 = regs->ARM_r3;
	u32 s;

	switch (h->op) {
	case OP_CAL_INIT:
		s = emit(h->op, 0, 0, 0);
		break;
	case OP_IOCTL:
		s = ioctl_rec(a1, a2, a3);
		break;
	case OP_TX_PKT:
		s = pkt_rec(h, a1, a2, a3, OP_TX_PKT, OP_TX_DATA,
			    ACCESS_ONCE(txdump), &txbudget);
		break;
	case OP_RX_PKT:
		s = pkt_rec(h, a1, a2, a3, OP_RX_PKT, OP_RX_DATA,
			    ACCESS_ONCE(rxdump), &rxbudget);
		break;
	case OP_TXS:
		s = txs_rec(pick(h->addr_src, a1, a2, a3),
			    pick(h->val_src, a1, a2, a3));
		break;
	case OP_TPL_RAMW:
		s = tpl_rec(a1, a2, a3);
		break;
	default:
		s = emit(h->op, pick(h->addr_src, a1, a2, a3),
			 pick(h->val_src, a1, a2, a3),
			 pick(h->aux_src, a1, a2, a3));
		break;
	}

	if (h->nargx && s) {
		u32 x[2] = { 0, 0 };

		if (probe_kernel_read(x, (void *)regs->ARM_sp,
				      h->nargx * sizeof(u32)))
			x[0] = x[1] = 0;
		emit(OP_ARGX, x[0], x[1], s);
	}
	return s;
}

static int wd_pre(struct kprobe *p, struct pt_regs *regs)
{
	wd_hook(container_of(p, struct hook, kp), regs);
	return 0;
}

static int wd_ret_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct hook *h = container_of(ri->rp, struct hook, krp);

	*(u32 *)ri->data = wd_hook(h, regs);
	return 0;
}

static int wd_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	u32 s = *(u32 *)ri->data;

	if (s)
		emit(OP_RETVAL, s, regs->ARM_r0, 0);
	return 0;
}

/* ---- plan, arming, disarming ------------------------------------------ */

static DEFINE_MUTEX(arm_lock);

static struct hook *hook_by_name(const char *name)
{
	int i;

	for (i = 0; i < NHOOK; i++)
		if (!strcmp(hooks[i].name, name))
			return &hooks[i];
	return NULL;
}

static unsigned long resolve(const char *name)
{
	char key[MODULE_NAME_LEN + 1 + KSYM_NAME_LEN];

	snprintf(key, sizeof(key), "%s:%s", target, name);
	return kallsyms_lookup_name(key);
}

static int attach(struct hook *h)
{
	if (h->retcap) {
		memset(&h->krp, 0, sizeof(h->krp));
		h->krp.kp.addr = (kprobe_opcode_t *)h->addr;
		h->krp.entry_handler = wd_ret_entry;
		h->krp.handler = wd_ret;
		h->krp.data_size = sizeof(u32);
		h->krp.maxactive = maxactive;
		return register_kretprobe(&h->krp);
	}
	memset(&h->kp, 0, sizeof(h->kp));
	h->kp.addr = (kprobe_opcode_t *)h->addr;
	h->kp.pre_handler = wd_pre;
	return register_kprobe(&h->kp);
}

/*
 * Resolve every hook in the target and, with arm=1, attach it. The fallbacks
 * go in a second pass, so that a fallback is dropped only for an accessor
 * that attached (or, in a dry run, resolved). Returns the hooks resolved: 0
 * means the target is not loaded. Called with arm_lock held.
 */
static int arma(void)
{
	int pass, i, resolved = 0, armed = 0;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < NHOOK; i++) {
			struct hook *h = &hooks[i];
			const char *kind = h->retcap ? "kretprobe" : "kprobe";
			struct hook *o;
			int err;

			if (!h->ripiego_di != !pass)
				continue;
			h->armed = false;
			h->addr = 0;
			if (h->op == OP_DELAY && !delay)
				continue;
			h->addr = resolve(h->name);
			if (!h->addr) {
				pr_info("wl_diag: %-28s absent\n", h->name);
				continue;
			}
			resolved++;
			o = h->ripiego_di ? hook_by_name(h->ripiego_di) : NULL;
			if (o && (arm ? o->armed : o->addr != 0)) {
				pr_info("wl_diag: %-28s dropped, %s is hooked\n",
					h->name, o->name);
				continue;
			}
			if (!arm) {
				pr_info("wl_diag: %-28s @%p %s (dry run)\n",
					h->name, (void *)h->addr, kind);
				continue;
			}
			err = attach(h);
			if (err) {
				pr_warn("wl_diag: %-28s @%p refused: %d\n",
					h->name, (void *)h->addr, err);
				continue;
			}
			h->armed = true;
			armed++;
			pr_info("wl_diag: %-28s @%p %s\n", h->name,
				(void *)h->addr, kind);
		}
	}
	if (resolved)
		pr_info("wl_diag: %s: %d of %d hooks resolved, %d attached%s\n",
			target, resolved, (int)NHOOK, armed,
			arm ? "" : " (arm=0: dry run)");
	return resolved;
}

/* Called with arm_lock held. */
static void disarma(void)
{
	struct kretprobe *krps[NHOOK];
	struct kprobe *kps[NHOOK];
	int i, nk = 0, nr = 0;

	for (i = 0; i < NHOOK; i++) {
		struct hook *h = &hooks[i];

		if (!h->armed)
			continue;
		if (h->retcap)
			krps[nr++] = &h->krp;
		else
			kps[nk++] = &h->kp;
	}
	if (nk)
		unregister_kprobes(kps, nk);
	if (nr)
		unregister_kretprobes(krps, nr);
	for (i = 0; i < NHOOK; i++) {
		struct hook *h = &hooks[i];

		if (h->armed && h->retcap && h->krp.nmissed)
			pr_warn("wl_diag: %s: %d return values lost (maxactive=%d)\n",
				h->name, h->krp.nmissed, maxactive);
		h->armed = false;
	}
}

static int wd_mod_notify(struct notifier_block *nb, unsigned long ev, void *data)
{
	struct module *m = data;

	if (!m || !target || strcmp(m->name, target))
		return NOTIFY_DONE;

	mutex_lock(&arm_lock);
	switch (ev) {
	case MODULE_STATE_COMING:
		disarma();
		mark("mod COMING");
		arma();
		break;
	case MODULE_STATE_GOING:
		mark("mod GOING");
		disarma();
		break;
	default:
		break;
	}
	mutex_unlock(&arm_lock);
	return NOTIFY_DONE;
}

static struct notifier_block wd_mod_nb = { .notifier_call = wd_mod_notify };

/* ---- /proc/wl_diag --------------------------------------------------- */
/* The stream is big-endian, as the MIPS variants write it natively: every
 * field is a number, the bytes of MARK and data records included. */
static void to_wire(struct wldiag_rec *r)
{
	r->ts_ns = (__force u64)cpu_to_be64(r->ts_ns);
	r->seq = (__force u32)cpu_to_be32(r->seq);
	r->addr = (__force u32)cpu_to_be32(r->addr);
	r->val = (__force u32)cpu_to_be32(r->val);
	r->aux = (__force u32)cpu_to_be32(r->aux);
	r->_pad = (__force u16)cpu_to_be16(r->_pad);
}

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
		r.ts_ns = local_clock();
		r.op = OP_DROP;
		r.aux = d;
		to_wire(&r);
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
	to_wire(&r);
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

/* Writing to the buffer injects a MARK carrying the label written:
 *
 *     echo "ch36 bw20" > /proc/wl_diag
 *
 * It is needed because with dynamic arming the reader stays open for the whole
 * run and cycle boundaries are no longer separate files. The record enters the
 * queue like every other, so it is ordered with the ops around it and not with
 * the clock of whoever writes it. */
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
/* The buffer lives in /proc/wl_diag: it appears by itself and needs no mknod,
 * where a misc device would need its dynamic minor read from /proc/misc on
 * every load. proc_create has the same signature on 2.6.30 and 3.4 and takes
 * file_operations, so the same call works for both. */
#define WD_PROC "wl_diag"


/* ---- init/exit -------------------------------------------------------- */
static int __init wd_init(void)
{
	int err;

	parse_skipphyrd();

	if (fifo_recs < 4096) {
		pr_warn("wl_diag: fifo_recs=%d too small, using 4096\n", fifo_recs);
		fifo_recs = 4096;
	}
	fifo_recs = 1 << (fls(fifo_recs) - 1);	/* round down to a power of 2 */
	fifo_buf = vmalloc(fifo_recs * sizeof(struct wldiag_rec));
	if (!fifo_buf) {
		pr_err("wl_diag: vmalloc of %d KB for the queue failed. "
		       "Retry with a lower fifo_recs.\n",
		       (int)(fifo_recs * sizeof(struct wldiag_rec) / 1024));
		return -ENOMEM;
	}
	err = kfifo_init(&fifo, fifo_buf, fifo_recs * sizeof(struct wldiag_rec));
	if (err) {
		pr_err("wl_diag: kfifo_init: %d\n", err);
		goto out_fifo;
	}
	pr_info("wl_diag: queue %d records (%d KB)\n", fifo_recs,
		(int)(fifo_recs * sizeof(struct wldiag_rec) / 1024));

	if (!proc_create(WD_PROC, 0600, NULL, &wd_fops)) {
		pr_err("wl_diag: proc_create(/proc/%s) failed\n", WD_PROC);
		err = -ENOMEM;
		goto out_fifo;
	}

	err = register_module_notifier(&wd_mod_nb);
	if (err) {
		pr_err("wl_diag: register_module_notifier: %d\n", err);
		goto out_proc;
	}

	mutex_lock(&arm_lock);
	if (!arma())
		pr_info("wl_diag: waiting for '%s' to load\n", target);
	mutex_unlock(&arm_lock);
	return 0;

out_proc:
	remove_proc_entry(WD_PROC, NULL);
out_fifo:
	vfree(fifo_buf);
	fifo_buf = NULL;
	return err;
}

static void __exit wd_exit(void)
{
	unregister_module_notifier(&wd_mod_nb);
	mutex_lock(&arm_lock);
	disarma();
	mutex_unlock(&arm_lock);
	remove_proc_entry(WD_PROC, NULL);
	vfree(fifo_buf);
	fifo_buf = NULL;
	pr_info("wl_diag: unloaded (lost: %d, filtered: %d)\n",
		atomic_read(&drops), atomic_read(&filtered));
}

module_init(wd_init);
module_exit(wd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kprobes PHY/radio/PMU tracer for Broadcom wl, ARM");
