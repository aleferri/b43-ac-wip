// SPDX-License-Identifier: GPL-2.0
/*
 * wl_diag (B) - PHY/radio/PMU tracer for the Broadcom "wl" driver, WITHOUT
 * kprobes.
 *
 * CONFIG_KPROBES is off on this kernel, so the accessors are hooked with a
 * function-entry detour: resolve the address through kallsyms, overwrite the
 * first 4 instructions with a jump (lui/ori/jr $t9) to an executable stub that
 * records (op, addr, val, mask), then re-run the 4 original instructions,
 * relocated, and return to func+16.
 *
 * ONLY entry arguments are captured (a1=addr, a2/a3=val/mask). The value
 * RETURNED by reads is not traced (a simplification): phy_reg_read and
 * read_radio_reg therefore log only the address read, and the decoder emits
 * them as val=UNDEFINED, never 0x0000.
 *
 * read_radio_reg has a branch in its 4th instruction (beq), so the classic
 * 4-word detour is impossible. It is hooked with the "short-j" variant (the
 * shortj field): the entry word becomes 'j stub' and the 2nd word becomes a
 * nop, so that the delay slot of that jump does not run the original word with
 * the registers as they were before the entry word. The stub re-runs o[0..1],
 * relocated, and returns to func+8. This needs the stub in the same 256MB
 * region, checked in the plan. The only condition on a short-j hook is
 * therefore the same as on a 4-word one, that no word of the window is a
 * branch, over a window of 2 words instead of 4. osl_delay (usec=a1) and
 * wlc_phy_table_{read,write}_acphy (id/len/off = a1/a2/a3) use the classic
 * 4-word detour.
 *
 * When the window holds an unconditional absolute `j`, that jump is diverted
 * instead: it is on the entry path by construction, since nothing before it
 * branched, so overwriting that ONE word catches every call, and the stub exits
 * by re-executing the saved jump. That is the route for the tail-call thunks,
 * whose own prologue is the jump and which therefore fit neither detour.
 *
 * Safety: arm=0 by default (dry run, the plan is only logged). With arm=1 the
 * patches are applied from inside stop_machine, writing the entry word LAST:
 * no intermediate state of a multi-word patch is a valid prologue, so no other
 * cpu may execute one. The stub pool is freed only when THIS module unloads,
 * so a stub still in flight when the target unloads still runs valid code.
 *
 * Runtime assumption (MIPS32R1, no NX/RODATA for module text): module memory
 * is RWX plus an explicit flush_icache_range. To be confirmed on the device.
 *
 * Arming is DYNAMIC. The hooks are not applied when this module is inserted
 * but on the target's MODULE_STATE_COMING notification, which on 3.4
 * (kernel/module.c, SYSCALL_DEFINE3(init_module)) arrives AFTER load_module --
 * the module is relocated and already in `modules`, so kallsyms sees it -- and
 * BEFORE do_one_initcall(mod->init). The driver probe, and the attach that
 * follows it, therefore fall under the hooks with no remove/rescan of the PCI
 * device. It also arrives before set_section_ro_nx, so the initial patch does
 * not depend on module text being writable; the restore does.
 *
 * Disarming sits on the MODULE_STATE_GOING notification, which on 3.4 arrives
 * AFTER mod->exit() and before free_module(): the detach is traced, and the
 * text is still mapped when the prologues are restored. Restoring plus
 * synchronize_sched() there is what keeps a fresh call from entering a stub
 * while the target's memory is about to be freed.
 *
 * What this means in practice: `rmmod wl; modprobe wl` in a loop without
 * touching this module, with the /proc/wl_diag reader open for the whole run.
 * Cycle boundaries go into the trace with `echo <label> > /proc/wl_diag` (a
 * MARK record).
 *
 * Target: kernel 3.4.x, MIPS32 big-endian, o32, SMP=2, PREEMPT.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/kfifo.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>		/* high_memory */
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <asm/cacheflush.h>
#include <asm/page.h>		/* PAGE_OFFSET, per il controllo su module_core */
/* Needed for BRK_KPROBE_BP, which tells whether the kernel has the
 * notify_die(DIE_BREAK) branch in do_bp: without this include the #ifdef
 * further down would always be false and the break path would compile away
 * in silence even where it is available. */
#include <asm/break.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/stop_machine.h>

static int arm;
module_param(arm, int, 0444);
MODULE_PARM_DESC(arm, "0=dry run (log the plan only), 1=apply the patches");

/* Name of the target module, for the match on mod->name in the notifier. Not
 * cosmetic: hooks are resolved by symbol name, and without the module check any
 * COMING at all would restart the plan. */
static char *target = "wl";
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "name of the module to hook (default wl)");

/* osl_delay is noisy -- one entry per udelay -- and the usec value captured
 * from a1 is not reliable on every path: sometimes it is garbage (argument in a
 * different register, or an inlined path). It stays detached by default;
 * delay=1 re-attaches it when the timing is really needed. */
static int delay;
module_param(delay, int, 0444);
MODULE_PARM_DESC(delay, "0=do not hook osl_delay (default), 1=hook it");

/* The firmware loads the wireless modules through a reserved allocator -- the
 * one behind "Load wl module core" -- that advances a cursor and never rewinds
 * it, so after an rmmod the target's block is not available to the next
 * insmod. bump_ptr is the KSEG0 address of that cursor, found with
 * reverse-tools/memfind and confirmed on the code of module_alloc: kallsyms
 * cannot give it, the variable is data and this kernel has no KALLSYMS_ALL.
 *
 * At the target's GOING the cursor is rewound to module_core, provided the
 * block is the last one allocated: the cursor must equal the block's
 * page-aligned end, otherwise nothing is touched. With restore_alloc=0 the
 * check runs and is logged, nothing is written. The next COMING compares the
 * fresh module_core with the value written, so the reuse is verified here and
 * not by reading the dmesg. */
static unsigned long bump_ptr;
module_param(bump_ptr, ulong, 0444);
MODULE_PARM_DESC(bump_ptr, "KSEG0 address of the reserved allocator's cursor (0=off)");

static int restore_alloc;
module_param(restore_alloc, int, 0444);
MODULE_PARM_DESC(restore_alloc, "0=check and log only (default), 1=rewind the cursor at the target's GOING");

/* flush_icache_range is not exported to modules, and on this kernel (KALLSYMS
 * without KALLSYMS_ALL) the pointer variable is not even visible to kallsyms,
 * because it lives in BSS. So we resolve the text function of the R4K cache
 * layer instead (see wd_init) and call it through this pointer. */
typedef void (*flush_fn_t)(unsigned long, unsigned long);
static flush_fn_t p_flush_icache;
static void flush_i(unsigned long s, unsigned long e)
{
	if (p_flush_icache)
		p_flush_icache(s, e);
}

/* ---- record + queue + char device (same as the kprobe version) -------- */
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
	OP_DROP = 255,
};
struct wldiag_rec {
	u64 ts_ns; u32 seq; u32 addr; u32 val; u32 aux;
	u8 op; u8 cpu; u16 _pad;
} __packed;

/*
 * The queue is allocated at runtime rather than statically: the size becomes a
 * parameter, and an allocation that fails shows up straight away instead of
 * inflating the module image in .bss.
 *
 * Why it has to be large: the peak rate seen on the DSL captures is 5200
 * records/s (the 80 MHz phase, with RETVALs on), and with 32768 records the
 * margin was ~6 seconds. That was not enough: that phase lost 1873 records in
 * 232 drips of 2-3, meaning the queue was on the edge throughout. With the
 * default of 131072 the margin rises to ~25 s.
 *
 * Cost: 28 bytes per record, so 131072 records are 3.5 MB of unpageable kernel
 * memory. On a router with 64 MB that is a slice, but it is only paid while the
 * module is loaded. If the allocation fails, lower the parameter: 65536 is
 * 1.75 MB and ~12 s of margin.
 */
/*
 * The queue sits on a vmalloc'd buffer, not on kfifo_alloc: __kfifo_alloc uses
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
 * memory, paid only while the module is loaded.
 *
 * fifo_recs is rounded down to a power of 2: kfifo_init divides the size in
 * bytes by esize and does rounddown_pow_of_two.
 */
/* A COLD init cycle is obtained by reloading the target module -- which is what
 * reverse-tools/cold_capture.sh does, with the hooks already armed from
 * MODULE_STATE_COMING -- not by tampering with the PHY struct from the stub.
 * The "already calibrated" byte (251 on 7.14.89, 227 on 6.30) therefore stays
 * a note only: when cold it is pi->[251] == 0 that makes cal_init complete, and
 * in the trace that shows as the CAL.INIT record.
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
 * Each field takes an entry argument: 0=absent(->0), 1=a1, 2=a2, 3=a3.     *
 * Reg-style: addr=a1(reg), val/mask from a2/a3. ChipCommon GPIO            *
 * (sih,mask,val,prio) has no reg: addr=0, val=a2, mask(aux)=a1.            */
struct hook {
	const char *name;
	u8 op, addr_src, val_src, aux_src;
	/* The thunk this hook is the fallback for, or NULL. If the accessor named
	 * here gets hooked, this hook is dropped: otherwise every op would come
	 * out twice, once from the thunk and once from what sits underneath.
	 *
	 * It goes after @aux_src and not next to @name because the table entries
	 * use positional initialisers for the first five fields: a field slipped
	 * in before would shift them all. This one is only ever set by name.
	 */
	const char *ripiego_di;
	bool shortj;		/* true: 1-word 'j' detour (branch inside the 4-word window) */
	bool retcap;		/* true: capture the return value through the ra trampoline */
	u8 nargx;		/* # arg extra su stack da catturare: arg5@16(sp), arg6@20(sp) */
	/* Where aux comes from when the hook is armed as a tail call: there the
	 * arguments seen are those of the function being jumped to, not those of
	 * the thunk's entry, and the delay slot has already set the ones it sets.
	 * 0 = same as @aux_src. Set by name, like the three above. */
	u8 tail_aux_src;
	unsigned long addr;
	u32 saved[4];
	bool armed;
	/* State fields added later: they MUST stay at the tail, because the table
	 * uses positional initialisers and putting them in the middle shifts them
	 * all. It has happened: a `true` meant for retcap ended up in use_bp,
	 * retcap stayed false for every hook, and NO RETVAL was ever emitted. */
	bool use_bp;		/* hook through 'break' + die notifier (not detourable) */
	bool use_sites;		/* patch the lui/addiu pairs at the call sites */
	u32 *bp_stub;		/* resume stub for the break path */
	bool use_tailj;		/* divert the unconditional tail call at word @tailw */
	u8 tailw;
	bool use_shortj;	/* @shortj AND the stub is reachable with a `j` */
};
static struct hook hooks[] = {
	{ "phy_reg_read",       OP_PHY_R,     1, 0, 0, .retcap = true },
	{ "phy_reg_write",      OP_PHY_W,     1, 2, 0 },
	{ "phy_reg_mod",        OP_PHY_MOD,   1, 3, 2 },
	/* and/or: single-register op (addr,val). Distinct op-codes so the decoder
	 * knows the operation; val=a2 is the AND mask (bits kept) or the OR value
	 * (bits set). No aux: the function has no 3rd argument. */
	{ "phy_reg_and",        OP_PHY_AND,   1, 2, 0 },
	{ "phy_reg_or",         OP_PHY_OR,    1, 2, 0 },
	{ "write_radio_reg",    OP_RADIO_W,   1, 2, 0 },
	{ "mod_radio_reg",      OP_RADIO_MOD, 1, 3, 2 },
	{ "si_pmu_chipcontrol", OP_PMU_CC,    1, 3, 2, .retcap = true },
	{ "si_pmu_regcontrol",  OP_PMU_RC,    1, 3, 2, .retcap = true },
	{ "si_pmu_pllcontrol",  OP_PMU_PLL,   1, 3, 2, .retcap = true },
	/* si_corereg(sih, coreidx, regoff, mask, val): generic access to a register
	 * of a backplane core. addr=regoff(a2), aux=coreidx(a1). val (a4, the 5th
	 * argument) is on the stack in o32 -> captured through nargx (a follow-on
	 * ARGX record). retcap: the return value (read/rmw) goes in the RETVAL. */
	{ "si_corereg",         OP_SI_COREREG,2, 0, 1, .retcap = true, .nargx = 1 },
	/* ChipCommon GPIO (sih, mask, val, prio): mask=a1, val=a2 */
	{ "si_gpiocontrol",     OP_CC_GPIOCTL,0, 2, 1 },
	{ "si_gpioout",         OP_CC_GPIOOUT,0, 2, 1 },
	{ "si_gpioouten",       OP_CC_GPIOOE, 0, 2, 1 },
	/* acphy table access (pi, id, len, off, width, data): id=a1, len=a2,
	 * off=a3. width/data are stack arguments and are not captured. Check the
	 * len/off order against your headers: the disasm pins id=a1 but not
	 * len-vs-off. */
	{ "wlc_phy_table_read_acphy",  OP_TBL_R, 1, 2, 3 },
	{ "wlc_phy_table_write_acphy", OP_TBL_W, 1, 2, 3 },
	{ "osl_delay",          OP_DELAY,     0, 1, 0 }, /* usec=a1 */
	/* Control towards the MAC (the d11 core). MACCONTROL RMW plus MAC host
	 * flags. Signatures taken from the brcmsmac branch (a mirror of the
	 * proprietary wl) -- to be re-checked against the disasm like the other
	 * hooks (cf. the len/off caveat on wlc_phy_table_*). If a prologue has a
	 * branch in the first 4 words, wd_init skips it with a pr_warn: no risk.
	 *   wlc_bmac_mctrl(hw, u32 mask, u32 val)   fixed reg: mask=a1, val=a2
	 *   wlc_bmac_mhf(hw, u8 idx, u16 mask, u16 val, int bands)
	 *                                           idx=a1, mask=a2, val=a3
	 *   wlc_bmac_mhf_get(hw, u8 idx, int bands) idx=a1 (val UNDEFINED) */
	{ "wlc_bmac_mctrl",     OP_MAC_MCTRL, 0, 2, 1 },
	/* `bands` is the 5th argument and in o32 sits at 16(sp): it is captured
	 * with nargx, and it is needed. In the captures the write of the HOSTF
	 * cell sometimes follows the call and sometimes does not -- on cold01
	 * #469 slot 3 and #620 slot 4 have no adjacent write, which reappears at
	 * the #689-#690 flush, while #623, #12242, #13525, #13530 and #13535 have
	 * it straight away. The guess is that the cell is written only when
	 * `bands` matches the current band and that otherwise the value stays
	 * cached; without that argument the two cannot be told apart, and the
	 * accumulated values (0x80 -> 0x88 -> 0x8088 on slot 4) stay
	 * unexplained. */
	{ "wlc_bmac_mhf",       OP_MAC_MHF_W, 1, 3, 2, .nargx = 1 },
	{ "wlc_bmac_mhf_get",   OP_MAC_MHF_R, 1, 0, 0, .retcap = true },
	/* MAC object memory (SHM, SCR, IHR): addr=offset, aux=selector.
	 * This also catches the noise sample of the crs_min_pwr cal, which comes
	 * through wlc_phy_noise_read_shmem -> wlapi_bmac_read_shm ->
	 * wlc_bmac_read_shm -> here, not from a PHY register.
	 * NAME PER VERSION: read_objmem on 6.30, read_objmem16 on 7.14.
	 * read_shm is not hooked: it is a wrapper with a jr in word 2. */
	/* Channel change: chanspec in a1. The generic one is hooked, which fires
	 * for every PHY and allows a single run over several channels, to be
	 * split afterwards. */
	/* Template RAM: where the PHY loads the tone waveforms, the input of
	 * RXIQ, PAPD and do_dummy_tx. No op class covered it.
	 * On 6.30 the ptr/data accessors do not exist: there, only the bulk.
	 *
	 * short-j on all four: on 7.14.89 the branch sits in word 2 of every one
	 * of them -- `beq` on the corerev test for ptr_wreg, `jr $ra` for
	 * data_wreg, `bne` on the byte at +2 for the two rreg -- so the 4-word
	 * window does not hold while the 2-word one does. The re-entry stays on
	 * $t9: the first two words write $v0 and $v1 only. */
	{ "wlc_bmac_templateptr_wreg",  OP_TPL_PTRW, 1, 0, 0, .shortj = true },
	{ "wlc_bmac_templatedata_wreg", OP_TPL_DATW, 1, 0, 0, .shortj = true },
	{ "wlc_bmac_templateptr_rreg",  OP_TPL_PTRR, 0, 0, 0,
	  .shortj = true, .retcap = true },
	{ "wlc_bmac_templatedata_rreg", OP_TPL_DATR, 0, 0, 0,
	  .shortj = true, .retcap = true },
	{ "wlc_bmac_write_template_ram", OP_TPL_RAMW, 1, 2, 3 },
	/* OTP: the generic layer has the same names on 6.30 and 7.14 and a clean
	 * prologue, while the hndotp_ and ipxotp_ ones change. The content is the
	 * SROM image, static and already known from the dumps: what this is for is
	 * knowing WHEN it is read and WHICH words, that is, where the values are
	 * consumed.
	 *   otp_read_word(oh, wn, *data)              wn=a1
	 *   otp_read_region(sih, region, *data, *len) region=a1
	 *   otp_init(sih)                             the moment only */
	{ "otp_init",        OP_OTP_INIT, 0, 0, 0, .retcap = true },
	{ "otp_read_word",   OP_OTP_RDW,  1, 0, 2, .retcap = true },
	{ "otp_read_region", OP_OTP_RDR,  1, 0, 3, .retcap = true },
	/* Two accessors the acphy code calls and the port does NOT do at all --
	 * zero references to bw_set or sromctl in src/. The call graph also says
	 * WHERE they go:
	 *
	 *   wlc_bmac_bw_set     <- wlapi_bmac_bw_set <- wlc_phy_chanspec_set_acphy
	 *                                            <- wlc_phy_init
	 *     bandwidth at the MAC level. Needed for 40 and 80 MHz: with BW20 only
	 *     the default goes unnoticed. It belongs in set_channel and op_init.
	 *   si_get/set_sromctl  <- wlc_phy_attach_acphy
	 *     the SROM control register. It belongs at the point of op_init that
	 *     corresponds to attach_acphy.
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
	 * makes it the soundest anchor for segmenting a sweep. Prologue:
	 * addiu sp / sw s0 / sw ra / lbu 251(a0), branch in word 4, so the 4-word
	 * detour holds. */
	{ "wlc_phy_cal_init", OP_CAL_INIT,   0, 0, 0 },
	{ "wlc_bmac_bw_set",  OP_MAC_BW,     0, 1, 0 },
	{ "si_get_sromctl",   OP_SROMCTL_R,  0, 0, 0, .retcap = true },
	{ "si_set_sromctl",   OP_SROMCTL_W,  0, 1, 0 },
	/* The chanspec written to shared memory is the sweep's cycle BOUNDARY,
	 * and on the AC-PHY it is the only one: the generic wlc_phy_chanspec_set
	 * is off the per-PHY path (no wlc_phy_chanspec_set_acphy symbol in these
	 * blobs) and emits no record there. It is deliberately absent from this
	 * table for a second reason: its 4-word detour window covers the `lui`
	 * half of the call site that loads THIS function, a site the short-j
	 * fallback below rewrites, so hooking both would put two patches on one
	 * word -- the collision piano_senza_collisioni() now refuses.
	 * Prologue: lw / lw / sltiu / beq, so the branch is in word 3 and the
	 * 4-word detour does not hold; the first two words are lw and are not
	 * PC-relative, so the 2-word short-j does. */
	{ "wlc_phy_chanspec_shm_set", OP_CHANSPEC_SHM, 1, 0, 0, .shortj = true },
	{ "wlc_bmac_read_objmem16",  OP_MAC_OBJ_R, 1, 0, 2, .retcap = true },
	{ "wlc_bmac_write_objmem16", OP_MAC_OBJ_W, 1, 2, 3 },
	/* The BULK object-memory pair. It is needed for two independent reasons.
	 *
	 * The first: it covers the regions the 16-bit pair does not show. The two
	 * `*_objmem16` accessors are LOCAL symbols, that is static, which does not
	 * hide them from kallsyms -- is_core_symbol() filters on section flags and
	 * not on binding -- but they are missing from the symbol table altogether
	 * in some blobs. Shared-memory ops are visible anyway through the
	 * read/write_shm thunks; what goes through the bulk without touching them
	 * is not.
	 *
	 * The second: the bulk carries the SELECTOR, and with it the region. In
	 * the current captures every OBJ op is shared memory, and nothing is seen
	 * of any other region -- among them the address match table, which is the
	 * open question on MAC and BSSID in docs/retrace-todo.md.
	 *
	 *   wlc_bmac_copyfrom_objmem(hw, offset, buf, len, sel)
	 *   wlc_bmac_copyto_objmem(hw, offset, buf, len, sel)
	 *
	 * offset=a1, len=a3, sel is the 5th argument and in o32 sits on the stack
	 * at 16(sp): captured with nargx, as is done for si_corereg. `buf` (a2) is
	 * a pointer and is not recorded: its content comes from the 16-bit ops
	 * underneath when the other hook is active, and otherwise stays out.
	 * TO BE CONFIRMED on the first capture: that the selector arrives in a5.
	 * If the record carries an absurd value the signature differs from this
	 * one. */
	{ "wlc_bmac_copyfrom_objmem", OP_MAC_OBJ_BULK_R, 1, 0, 3, .nargx = 1 },
	{ "wlc_bmac_copyto_objmem",   OP_MAC_OBJ_BULK_W, 1, 0, 3, .nargx = 1 },
	/* Address match: see the note in the 2.6.30 tracer, which has the same
	 * ones. The names change between versions and both are listed; whichever
	 * is absent does not resolve. Signatures from brcms_b_set_addrmatch()
	 * in brcmsmac, index in a1. TO BE CONFIRMED on the first capture. */
	/* set_addrmatch has a branch in word 2 of the prologue (the test on
	 * hw+72), so the 4-word detour does not fit: short-j. Words 0 and 1 are
	 * `lw` and `sltiu`, not PC-relative.
	 * a1 = index, a2 = pointer to the address (`lbu 1($a2)` reads it).
	 *
	 * ORDER, and it is not a matter of taste. On 7.14.89 the bmac-level name
	 * is set_rxe_addrmatch, but it sits on the branch wlc_set_addrmatch takes
	 * only for corerev < 40:
	 *
	 *     lw    $v0, 16($v0)          ; corerev
	 *     sltiu $v0, $v0, 0x28
	 *     bne   $v0, $zero, <legacy>  ; and inside <legacy>, and only there,
	 *                                 ; the calls to set_rcmta and to
	 *                                 ; set_rxe_addrmatch
	 *
	 * The AC core is corerev 42, so that branch is never taken and the hook
	 * arms clean and stays mute for the whole run -- measured: ADDRM.SET at 0
	 * over 45 cycles on the TG789vac v2, against 5280 on the D6220 where the
	 * op went to the wlc level. It is the same reason RCMTA.WR cannot appear
	 * on this core, already written down in router-data/CLASS-COVERAGE.md.
	 * So the wlc-level entries come first and set_rxe_addrmatch stays as the
	 * last resort, for a build where neither of the two above resolves. */
	{ "wlc_bmac_set_addrmatch", OP_ADDRMATCH, 1, 0, 0, .shortj = true },
	{ "wlc_set_addrmatch",      OP_ADDRMATCH, 1, 0, 0 },
	{ "wlc_bmac_set_rxe_addrmatch", OP_ADDRMATCH, 1, 0, 0, .shortj = true },
	/* write_amt: a1 = index (`sll a1,1`), a3 = a signed 16-bit value the
	 * function does `bltz` on. Prologue clear. */
	{ "wlc_bmac_write_amt",     OP_AMT_W,     1, 0, 3 },
	{ "wlc_bmac_set_rcmta",     OP_RCMTA_W,   1, 0, 0 },
	/* Accessors found in the blobs of both versions and not covered by the
	 * hooks above. They cover what nothing shows today:
	 *
	 *   phy_reg_write_array   bulk PHY write. This is the accessor the TODO in
	 *                         docs/retrace-todo.md was hunting for under the
	 *                         name phy_reg_write_list. If it calls phy_reg_write
	 *                         inside, the individual writes are already visible
	 *                         and this hook adds a marker, as TBL.WR does for
	 *                         tables; if it does not, this is the only way to
	 *                         see them. Useful either way.
	 *   phy_reg_read/write_wide   32-bit PHY access. The 16-bit hooks do not
	 *                         intercept it.
	 *   wlc_bmac_write_ihr    the d11 core's Indirect Hardware Registers.
	 *                         objmem reaches them by selector, but a dedicated
	 *                         writer writes them without going through it.
	 *   wlc_bmac_set_shm      masked write to shared memory.
	 *
	 * Signatures read off the prologues of the 6.30 object, not assumed -- and
	 * four out of six were not what the name suggested:
	 *
	 *   phy_reg_write_array(pi, array, n)   has NO address: a1 is a pointer to
	 *       the array and a2 the count (a `blez a2` gives it away). Only n is
	 *       recorded. Inside it calls phy_reg_and & co., so the individual
	 *       writes are already visible from the 16-bit hooks and this is a
	 *       marker -- like TBL.WR for tables.
	 *   phy_reg_write_wide(pi, val)   has NO address: fixed register, value in
	 *       a1 masked to 16 bits. Like wlc_bmac_mctrl.
	 *   phy_reg_read_wide(pi)   no useful argument, value in the RETVAL.
	 *   wlc_bmac_write_ihr(hw, off, val)   off=a1, val=a2; there is no a3.
	 *   wlc_bmac_set_shm(hw, off, val, len)   off=a1, val=a2, len=a3: it is a
	 *       memset over shared memory, and in the loop it calls the 16-bit
	 *       accessor.
	 *
	 * A candidate that does not get hooked does no harm: pianifica() notices
	 * from the prologue and skips it with a pr_warn. */
	{ "phy_reg_write_array", OP_PHY_WARR, 0, 2, 0 },
	{ "phy_reg_read_wide",   OP_PHY_RDW,  0, 0, 0, .retcap = true },
	/* short-j: on the 7.14 blob the prologue is lw / lw / lbu / bne, so the
	 * branch falls in word 3 and the 4-word detour does not hold; the first
	 * two words are lw, not PC-relative and with no side effects, so re-running
	 * them in the stub is harmless. The signatures below were read on the 6.30
	 * object, where the prologue was evidently different: this is the textbook
	 * case of the note above about names and offsets changing between
	 * versions, except that here it is not the name that changes but the
	 * prologue, and the outcome was the same -- hook skipped, class missing
	 * from the capture, no error.
	 *
	 * What to expect: on 7.14 this changes nothing about what is seen, because
	 * in that blob *nobody* calls phy_reg_write_wide -- zero lui/addiu pairs
	 * point here. The hook gets planned and never emits. It matters for builds
	 * where the function is live, and so that the plan is clean.
	 * Checked with reverse-tools/audit_hooks.py; to be re-checked on 6.30 when
	 * that object is within reach. */
	{ "phy_reg_write_wide",  OP_PHY_WRW,  0, 1, 0, .shortj = true },
	{ "wlc_bmac_write_ihr",  OP_IHR_W,    1, 2, 0 },
	{ "wlc_bmac_set_shm",    OP_OBJ_SET,  1, 2, 3 },
	/* The force gated clock. b43 has it as b43_phy_force_clock() and the port
	 * uses it in b43_phy_ac_reset_cca(), but the captures hold no class for it:
	 * the harness emits it as a comment and the comparison ignores it, so it is
	 * invisible on both sides. On the d6220 7.14 blob the AC callers are three
	 * and not one: wlc_phy_resetcca_acphy once and wlc_phy_cal_txiqlo_acphy
	 * twice. If the vendor forces the clock around the TX IQ/LO cal as well and
	 * the port does not, nobody sees it today.
	 *
	 * wlc_bmac_phyclk_fgc is hooked and not the thunk wlapi_bmac_phyclk_fgc:
	 * all thirteen callers go through the thunk, which tail-calls it, and the
	 * thunk has lui $t9 in word 0 -- the case that forced the choice of the
	 * re-entry register -- plus thirteen sites against MAX_SITES 8.
	 *
	 * short-j: the prologue is lw / addiu / lhu / beq, branch in word 3. The
	 * first two words write $v0 and $v1, so the re-entry stays on $t9.
	 * Signature wlc_bmac_phyclk_fgc(hw, force): force in a1, an `andi a1,0xff`
	 * says so. Checked with reverse-tools/audit_hooks.py on the module pulled
	 * out of the firmware, which is the same code as the prelink object. */
	{ "wlc_bmac_phyclk_fgc", OP_PHY_FGC,  0, 1, 0, .shortj = true },
	/* The configuration userspace gives the driver: `wl <cmd>` and every other
	 * ioctl. wl_ioctl() copies the user buffer into one of its own (osl_malloc
	 * of max(len, 0x2000), then __copy_user) and calls
	 *
	 *   wlc_ioctl(wlc, cmd, buf, len, wlcif)   cmd=a1, buf=a2, len=a3
	 *
	 * so the payload is kernel memory by the time it gets here. A command that
	 * only sets a driver variable -- `wl phycal_tempdelta 40`, `wl chanspec`
	 * before the up -- touches no register, and without this hook it is not in
	 * the trace at all. ioctl_rec() decodes the payload; see it for the records.
	 *
	 * wlc_ioctl is a thunk: lui/addiu $t9, jr $t9, nop (read with mipsdis.py on
	 * wlD6220.o, 7.14.89). The first two words are not PC-relative, so the
	 * short-j re-runs them and returns to the jr; word 0 writes $t9, so the
	 * re-entry goes through $t8. */
	{ "wlc_ioctl",          OP_IOCTL,    1, 2, 3, .shortj = true },
	/* Variants for builds where the two above do NOT exist. On 7.14.43 there is
	 * no 16-bit accessor at all: only the bulk copyfrom/copyto_objmem pair, and
	 * the 16-bit ones go through here. They cover the SHM selector only, not
	 * SCR nor IHR, so aux stays 0.
	 *
	 * They are short thunks with a tail call -- read_shm 16 B, write_shm 20 B,
	 * both with `jr $t9` inside the 4-word window -- so the classic detour does
	 * not hold. The first TWO words are lui/addiu and lui/andi, not
	 * PC-relative, so the short-j does: the stub re-runs them and returns to
	 * +8, where the rest of the tail call is. The call-site route is not taken:
	 * read_shm has ~34 of them and MAX_SITES is 8.
	 *
	 * They are also the case that made the choice of re-entry register in the
	 * stub necessary: word 0 is `lui $t9`, and with $t9 used for the re-entry
	 * too, control returns to +8 with the wrong $t9, the addiu adds the lo16 to
	 * it, and the `jr $t9` lands in the middle of another function.
	 *
	 *   wlc_bmac_read_shm(hw, offset)         offset=a1, value in the RETVAL
	 *   wlc_bmac_write_shm(hw, offset, val)   offset=a1, val=a2
	 *
	 * val arrives ALREADY truncated to 16 bits: the andi is word 1, which the
	 * short-j nops and the stub re-runs before returning to +8.
	 *
	 * On 7.14.89.14 the two thunks are 8 B `j body` + `lui $a2,1` and 12 B
	 * `andi` + `j body` + `lui $a3,1`, and the 16-bit accessors they jump to
	 * carry no symbol at all: nothing resolves by name, so `ripiego_di` has
	 * nothing to prefer and these two are all there is. There the tail call
	 * is what gets diverted, and `tail_aux_src` is why: at the jump the delay
	 * slot has already put the selector in a2 (read) and a3 (write), which is
	 * the signature of the accessor and not of the thunk, so aux carries the
	 * real selector instead of 0 and the records come out in the same shape
	 * as those from read/write_objmem16 on the other boards. */
	{ "wlc_bmac_read_shm",  OP_MAC_OBJ_R, 1, 0, 0,
	  .shortj = true, .retcap = true, .tail_aux_src = 2,
	  .ripiego_di = "wlc_bmac_read_objmem16" },
	{ "wlc_bmac_write_shm", OP_MAC_OBJ_W, 1, 2, 0, .shortj = true,
	  .tail_aux_src = 3,
	  .ripiego_di = "wlc_bmac_write_objmem16" },
	/* branch in slot 3 (beq): the classic 4-word detour is impossible, the
	 * short-j holds. addr=a1 raw: the andi 0xffff is o[0], re-run in the
	 * stub, as is o[1] (addiu $v0,1), so $v0 is re-set AFTER the hook. */
	{ "read_radio_reg",     OP_RADIO_R,   1, 0, 0, .shortj = true, .retcap = true },
};
#define NHOOK ARRAY_SIZE(hooks)

/* Landing point of the detour: called from the stub with (id, a1, a2, a3). */
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

u32 __used noinline
wl_diag_hook(u32 id, u32 a1, u32 a2, u32 a3)
{
	struct hook *h = &hooks[id];
	u8 aux_src = (h->use_tailj && h->tail_aux_src) ? h->tail_aux_src :
							 h->aux_src;

	/* CAL.INIT carries the state of the switch in the record, so the trace says
	 * by itself which cycles were forced. Without this the experiment cannot be
	 * read afterwards: capture_plan alternates 1 and 0, so the value read from
	 * sysfs at the end of the run is always the last one written. */
	/* CAL.INIT carries no payload: it says WHEN cal_init was invoked, and it is
	 * also the anchor for segmenting a sweep, one per cycle. */
	if (h->op == OP_CAL_INIT)
		return emit(h->op, 0, 0, 0);
	if (h->op == OP_IOCTL)
		return ioctl_rec(a1, a2, a3);

	return emit(h->op, pick(h->addr_src, a1, a2, a3),
			   pick(h->val_src,  a1, a2, a3),
			   pick(aux_src,     a1, a2, a3));
}

/* Follow-on record for stack arguments (o32): a second ARGX record tied to the
 * main one through parent_seq. addr=arg5, val=arg6. */
void __used noinline
wl_diag_hook_argx(u32 parent_seq, u32 x1, u32 x2)
{
	emit(OP_ARGX, x1, x2, parent_seq);
}

/* ---- return value capture (retcap): trampoline on 'ra' ----------------- *
 * The origin of 'ra' is saved PER INVOCATION in a pool indexed by 'current'  *
 * (the task): it survives preemption and migration (SMP+PREEMPT), unlike a   *
 * per-CPU slot. LIFO, to handle nesting (a hooked read that calls another).  *
 * Pool full -> NO diversion (no crash, only that value is lost).             */
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
static unsigned long ret_trampoline;	/* address of the shared return stub */

/* entry of a retcap: records (current, orig_ra, seq); returns the 'ra' address
 * to install (the trampoline if there is room, otherwise orig_ra). */
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

/* return of a retcap: take the instance of current off the LIFO, emit
 * RETVAL(seq, retval) and return orig_ra. Called only if enter had diverted, so
 * by construction the instance exists; defensive guard if best<0. */
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
/* beq rs,rt,off: off in INSTRUCTIONS, relative to the word after the delay
 * slot */
static inline u32 i_lui(u8 rt, u16 im){ return (0x0fu<<26)|(rt<<16)|im; }
static inline u32 i_ori(u8 rt, u8 rs, u16 im){ return (0x0du<<26)|(rs<<21)|(rt<<16)|im; }
static inline u32 i_jalr(u8 rs){ return (rs<<21)|(R_RA<<11)|0x09u; }
static inline u32 i_jr(u8 rs){ return (rs<<21)|0x08u; }
static inline u32 i_j(unsigned long tgt){ return (0x02u<<26)|(u32)((tgt>>2)&0x03ffffffu); }
#define I_NOP 0u

/* true if the opcode is a branch or jump (not relocatable verbatim into the
 * stub) */
/*
 * Destination register of an instruction, or 0xff if it writes none.
 * This is needed for ONE precise reason: after re-running the displaced words,
 * the stub loads the re-entry address into a register and jumps through it. If
 * one of the re-run words wrote THAT register, the value the original code
 * expects on re-entry is destroyed.
 *
 * Not theoretical: the prologue of a tail-calling thunk is lui/addiu on $t9,
 * and using $t9 for the re-entry too returns to function+8 with $t9 = re-entry
 * address instead of the target. The following `addiu $t9, $t9, lo` produces a
 * random address and the `jr $t9` jumps into the middle of another function:
 * an oops for an unaligned access, with $t9 == epc in the dump.
 */
static u8 dest_reg(u32 insn)
{
	u32 op = insn >> 26;

	if (op == 0) {				/* SPECIAL: dest = rd */
		u32 f = insn & 0x3f;

		if (f == 0x08 || f == 0x09)	/* jr/jalr: nessun rd utile */
			return 0xff;
		return (u8)((insn >> 11) & 31);
	}
	if (op == 0x0f || op == 0x09 || op == 0x0c || op == 0x0d ||
	    op == 0x0a || op == 0x0b || op == 0x08 ||	/* lui/addiu/andi/ori/slti* */
	    (op >= 0x20 && op <= 0x27))			/* load: dest = rt */
		return (u8)((insn >> 16) & 31);
	return 0xff;				/* store, branch, altro */
}

static bool scrive_reg(const u32 *w, int n, u8 r)
{
	int i;

	for (i = 0; i < n; i++)
		if (dest_reg(w[i]) == r)
			return true;
	return false;
}

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

/* An unconditional absolute jump: `j target`, with no link. The 26-bit field
 * is relative to the 256MB region of the PC, so the same word re-executed from
 * a stub in that region lands on the same target. */
static bool is_j_abs(u32 insn)
{
	return (insn >> 26) == 0x02;
}

/* ---- executable stub pool ---------------------------------------------
 *
 * kmalloc and not a static array, and the reason is the `j`. A one-word patch
 * can only be a `j`, whose 26-bit field keeps the top 4 bits of the PC, so the
 * short-j and the tail-call diversion work only if the stub sits in the same
 * 256MB region as the code being diverted. A static array lives in the
 * module's .data, which the module loader puts in the ordinary module area --
 * around 0xc3e5b000 on this family -- while the vendor loader puts `wl` in
 * KSEG0, around 0x80b9a000: different regions, and both one-word routes fall
 * back to the 4-word detour or the break. kmalloc returns KSEG0 too, so the
 * pool lands next to the target and the one-word routes become usable.
 *
 * Nothing depends on this working: pianifica() checks the region hook by hook
 * and falls back on its own, so an allocation in the wrong region costs
 * coverage, not correctness.
 *
 * Freed only in wd_exit, never when the TARGET unloads: a stub still in flight
 * at the target's GOING has to keep running valid code. */
#define STUB_WORDS 48
#define STUB_POOL_BYTES (NHOOK * STUB_WORDS * sizeof(u32))
static u32 (*stub_pool)[STUB_WORDS];
static u32 ret_tramp[16] __attribute__((aligned(8)));	/* trampolino di ritorno condiviso */

/* The 'break' path for prologues that cannot be detoured (a branch in the
 * window). One word, no delay slot. do_bp() calls notify_die(DIE_BREAK) for
 * BRK_KPROBE_BP outside CONFIG_KPROBES, so a die notifier is enough; with
 * NOTIFY_STOP die_if_kernel is never reached. Checked on Linux 3.4.
 * On 2.6.30 it does not exist (do_bp -> do_trap_or_bp, set_except_vector not
 * exported): BRK_KPROBE_BP is the discriminator and the path compiles away.
 * Word 0 is re-run in a stub, so it cannot be PC-relative: that is checked
 * before arming. */
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

		/* o32: a1..a3 are $a1..$a3 = regs[5..7] */
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

/* stub: [0] original word, [1] j func+4, [2] nop */
static void build_bp_stub(int idx)
{
	u32 *s = stub_pool[idx];
	unsigned long ret = hooks[idx].addr + 4;
	u8 rj = scrive_reg(hooks[idx].saved, 1, R_T9) ? R_T8 : R_T9;

	/* The return is a 32-bit jump and NOT a `j`: the 26-bit field keeps the
	 * top 4 bits of the PC, and the stub does not live in the target's 256MB
	 * region whenever the two modules are allocated in different areas. On
	 * this family the vendor loader puts `wl` in KSEG0 (0x80b9a000) while a
	 * normal module lands around 0xc3e5b000, so a `j 0x80d7ced4` from the
	 * stub goes to 0xc0d7ced4, which is somebody else's text. */
	s[0] = hooks[idx].saved[0];
	s[1] = i_lui(rj, ret >> 16);
	s[2] = i_ori(rj, rj, ret & 0xffff);
	s[3] = i_jr(rj);
	s[4] = I_NOP;
	hooks[idx].bp_stub = s;
}
#else
#define WD_HAVE_BP 0
#endif

/* Call-site patching. The module is -mabicalls: zero jal in .text, calls are
 * lui/addiu + jalr (or jr $t9 for tail calls), so the pair is rewritten to load
 * the stub instead. The function stays intact: no constraint on the prologue,
 * and it works on 2.6.30 where there is no break.
 * A single stub serves both jalr and jr: it preserves ra and jumps to the real
 * function.
 * Three conditions, checked at runtime: the pair must yield the exact address;
 * a jump on the SAME register must follow it; the addiu must not be shared (the
 * compiler reuses the low part across different sites: in the D6220 blob the
 * one at +0x1f5a24 serves two functions). */
#define MAX_SITES 8

struct site {
	u32 *hi, *lo;
	u32 saved_hi, saved_lo;
};
static struct site sites[NHOOK][MAX_SITES];
static int n_sites[NHOOK];

/* immediates of a lui/addiu pair to load `v`: the addiu sign-extends the low
 * part, so the high part has to compensate. */
static inline u16 hi16_of(unsigned long v) { return (u16)((v + 0x8000UL) >> 16); }
static inline u16 lo16_of(unsigned long v) { return (u16)(v & 0xffff); }

/* Identity of the target, WITHOUT taking a reference: needed for two things,
 * giving find_sites the base of the text to scan, and discarding symbols that
 * kallsyms resolves outside this module. The second is not theoretical:
 * accessor names are not namespaced, and hooking the namesake in another module
 * would patch foreign code with no error. */
static struct module *target_mod;

/* A purely arithmetic filter, no list walking: it applies once the first symbol
 * has established which module is the target. */
/* The target's text range, and whether it can be trusted.
 *
 * module_core and core_text_size are the ONLY fields of struct module this
 * code reads past the name, and they sit behind enough of the structure that a
 * vendor patch to module.h moves them. Read at the wrong offset, module_core
 * is not a pointer and find_sites scans from it: an oops with no hint of why.
 *
 * The path that reads them is the COMING notifier, which hands over the
 * struct. Arming from wd_init against an already loaded target does not get
 * here at all: the vendor loader puts `wl` in KSEG0, outside
 * module_addr_min/max, so __module_text_address gives NULL and target_mod
 * stays NULL. The two paths are therefore NOT equivalent, and the first
 * insmod of the target is where these offsets get used for the first time.
 *
 * So they are checked once per plan, against the first symbol that resolves:
 * that address has to be inside the range the structure claims. If it is not,
 * the range is unusable and it costs the call-site scan and the out-of-module
 * filter, not the run. */
#define TESTO_DA_DECIDERE	0
#define TESTO_BUONO		1
#define TESTO_INUTILIZZABILE	2
static u8 testo_stato;
static unsigned long testo_base, testo_size;

static void valuta_testo(unsigned long a)
{
	unsigned long b = (unsigned long)target_mod->module_core;
	unsigned long n = target_mod->core_text_size;

	if (b >= PAGE_OFFSET && !(b & 3) && n && n <= (32UL << 20) &&
	    a >= b && a < b + n) {
		testo_base = b;
		testo_size = n;
		testo_stato = TESTO_BUONO;
		pr_info("wl_diag: target text %px + %lu B\n", (void *)b, n);
		return;
	}
	testo_stato = TESTO_INUTILIZZABILE;
	pr_warn("wl_diag: struct module says module_core=%px core_text_size=%lu, and %px is not inside it: the offsets do not match this kernel. No call-site scan, no out-of-module filter.\n",
		(void *)b, n, (void *)a);
}

static bool dentro_bersaglio(unsigned long a)
{
	if (!target_mod)
		return true;
	if (testo_stato == TESTO_DA_DECIDERE)
		valuta_testo(a);
	if (testo_stato != TESTO_BUONO)
		return true;	/* cannot tell: better than dropping every hook */
	return a >= testo_base && a < testo_base + testo_size;
}

/* ---- reserved allocator: rewind at GOING, verify at COMING -------------- */
static unsigned long alloc_rewound;	/* module_core written at the last GOING */

static inline unsigned long bump_read(void)
{
	return *(volatile unsigned long *)bump_ptr;
}

/* core_size sits next to core_text_size in struct module, so the offset check
 * that valuta_testo did for this cycle covers it: without TESTO_BUONO the block
 * bounds cannot be trusted and the cursor is left alone. */
static void alloc_rewind(struct module *m)
{
	unsigned long base, fine, fine_raw, cur;
	int i;

	if (!bump_ptr)
		return;
	/* Not yet decided when the plan ran without target_mod: decide it now on
	 * the first resolved hook, the same way pianifica does. */
	if (testo_stato == TESTO_DA_DECIDERE)
		for (i = 0; i < NHOOK; i++)
			if (hooks[i].addr) {
				dentro_bersaglio(hooks[i].addr);
				break;
			}
	if (testo_stato != TESTO_BUONO) {
		pr_warn("wl_diag: rewind skipped: struct module offsets not verified in this cycle\n");
		return;
	}
	base = (unsigned long)m->module_core;
	fine_raw = base + m->core_size;
	fine = PAGE_ALIGN(fine_raw);
	if (base < PAGE_OFFSET || (base & ~PAGE_MASK) || fine <= base ||
	    fine - base > (32UL << 20)) {
		pr_warn("wl_diag: rewind skipped: implausible block %px + %u B\n",
			(void *)base, m->core_size);
		return;
	}
	/* The block is on top whether the allocator aligns the cursor after
	 * the allocation (cursor == page-aligned end) or before the next one
	 * (cursor == raw end): the three boot addresses do not tell which. */
	cur = bump_read();
	if (cur != fine && cur != fine_raw) {
		pr_warn("wl_diag: rewind skipped: cursor %px, block %px..%px is not on top\n",
			(void *)cur, (void *)base, (void *)fine);
		return;
	}
	if (!restore_alloc) {
		pr_info("wl_diag: cursor %px is the block end; restore_alloc=1 would rewind it to %px\n",
			(void *)cur, (void *)base);
		return;
	}
	*(volatile unsigned long *)bump_ptr = base;
	alloc_rewound = base;
	pr_info("wl_diag: cursor rewound %px -> %px\n", (void *)cur, (void *)base);
}

static void alloc_verify(struct module *m)
{
	unsigned long base;

	if (!alloc_rewound)
		return;
	base = (unsigned long)m->module_core;
	if (testo_stato != TESTO_BUONO)
		pr_warn("wl_diag: reuse unverified: struct module offsets not verified in this cycle\n");
	else if (base == alloc_rewound)
		pr_info("wl_diag: block reused: module_core %px\n", (void *)base);
	else
		pr_warn("wl_diag: block NOT reused: module_core %px, cursor was rewound to %px\n",
			(void *)base, (void *)alloc_rewound);
	alloc_rewound = 0;
}

/* How many addiu/jump epilogues consume the high half of the `lui $rt` at
 * index i to reach fnaddr. The compiler emits one lui and several tail-call
 * epilogues off it across a branch diamond -- wlc_pretbtt_set does exactly
 * this for wlc_bmac_write_shm, sharing one `lui $t9` between two `addiu $t9 /
 * jr $t9` paths. patch_sites can repoint a lui only once, so a shared one must
 * be left alone: repointing it gives every sibling epilogue hi(stub):lo(fn), a
 * wild jump into the stub pool. Along any control path the high half comes
 * from the nearest preceding lui, so the reach ends at the next full write of
 * $rt (another lui, a load, a move); an `addiu $rt,$rt` is a consumer, not a
 * redefinition, and does not end it -- the sibling epilogues are mutually
 * exclusive at run time even though both are present in memory. */
static int lui_consumatori(const u32 *base, int words, int i, int rt,
			   unsigned long fnaddr)
{
	int p, cnt = 0;

	for (p = i + 1; p < words; p++) {
		u32 w = base[p];
		bool consumer = (w >> 26) == 0x09 &&
				((w >> 21) & 31) == rt && ((w >> 16) & 31) == rt;
		int j;

		if (!consumer && dest_reg(w) == rt)
			break;			/* $rt's high half redefined */
		if (!consumer)
			continue;
		if (((unsigned long)(base[i] & 0xffff) << 16) +
		    (long)(s16)(w & 0xffff) != fnaddr)
			continue;
		for (j = 1; j <= 8 && p + j < words; j++) {
			u32 wj = base[p + j];

			if ((wj >> 26) != 0 || ((wj >> 21) & 31) != rt)
				continue;
			if ((wj & 0x3f) == 0x08 || (wj & 0x3f) == 0x09) {
				cnt++;
				break;
			}
		}
	}
	return cnt;
}

static int find_sites(int idx, unsigned long fnaddr)
{
	u32 *base;
	unsigned long words;
	int n = 0, i, k;

	if (testo_stato != TESTO_BUONO) {
		pr_warn("wl_diag: no usable text range for '%s', no call-site scan\n",
			hooks[idx].name);
		return 0;
	}
	base = (u32 *)testo_base;
	words = testo_size / 4;

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
			if (a != fnaddr)
				break;
			/* jump on the same register within 8 instructions */
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
				pr_info("wl_diag: '%s' site @%px has no jump, ignored\n",
					hooks[idx].name, &base[i]);
				break;
			}
			if (lui_consumatori(base, (int)words, i, rt, fnaddr) > 1) {
				pr_warn("wl_diag: '%s' site @%px dropped (lui shared by several epilogues)\n",
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

	/* drop the sites that share their addiu with another */
	for (i = 0; i < n; i++) {
		int shared = 0;

		for (k = 0; k < n; k++)
			if (k != i && sites[idx][k].lo == sites[idx][i].lo)
				shared = 1;
		if (shared) {
			pr_warn("wl_diag: '%s' site @%px dropped (addiu shared @%px)\n",
				hooks[idx].name, sites[idx][i].hi, sites[idx][i].lo);
			sites[idx][i] = sites[idx][--n];
			i--;
		}
	}
	if (n == MAX_SITES)
		pr_warn("wl_diag: '%s' hit the cap of %d sites: calls beyond "
			"the cap are NOT intercepted\n", hooks[idx].name, MAX_SITES);
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

/* The notifier is the pivot of dynamic arming, not a fallback defence: COMING
 * arms (before mod->init, hence before the driver probe), GOING disarms (after
 * mod->exit, while the text is still mapped). No reference on the target:
 * `rmmod wl` has to be able to succeed, and it is the central step of a cold
 * capture. */
static bool mod_nb_registered;

static int arma(void);
static void disarma(void);

static int wd_mod_notify(struct notifier_block *nb, unsigned long ev, void *data)
{
	struct module *m = data;

	if (!m || !target || strcmp(m->name, target))
		return NOTIFY_DONE;

	switch (ev) {
	case MODULE_STATE_COMING:
		/* A COMING without the preceding GOING should not happen, but
		 * re-patching over stale addresses would be silent and fatal. */
		disarma();
		target_mod = m;
		mark("mod COMING");
		arma();
		alloc_verify(m);
		break;
	case MODULE_STATE_GOING:
		mark("mod GOING");
		disarma();
		/* Armed from wd_init against an already loaded target, target_mod
		 * is still NULL here: __module_text_address does not see a module
		 * the vendor loader put in KSEG0. The notifier hands over the
		 * struct, and alloc_rewind needs it. */
		if (!target_mod)
			target_mod = m;
		alloc_rewind(m);
		target_mod = NULL;
		break;
	default:
		break;
	}
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
	/* on the call-site route the function is intact: nothing to re-run, the
	 * re-entry is from 0 */
	int rep = (hooks[idx].use_sites || hooks[idx].use_tailj) ? 0 :
		  (hooks[idx].use_shortj ? 2 : 4);
	unsigned long ret = hooks[idx].use_sites ? hooks[idx].addr :
		hooks[idx].addr + (hooks[idx].use_shortj ? 8 : 16);
	int n = 0, k;
	u8 rj;

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

	/* retcap: wl_diag_enter_ret(orig_ra, seq) -> v0 = the ra to install
	 * (the trampoline if there is room, otherwise orig_ra = no diversion). */
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
	/* Armed on a tail call, the stub exits by RE-EXECUTING that jump: the
	 * saved word itself, so the target is the one the driver had, with no
	 * address recomputed here. Nothing else is re-run -- the words before it
	 * and its delay slot have already gone by -- and no register is needed,
	 * because the jump is absolute. The return does not come back through
	 * here: $ra is the caller's, or the trampoline's when retcap is on. */
	if (hooks[idx].use_tailj) {
		s[n++] = hooks[idx].saved[hooks[idx].tailw];
		s[n++] = I_NOP;
		return;
	}
	for (k = 0; k < rep; k++)
		s[n++] = o[k];	/* re-run the displaced words (o[1] of the short-j re-sets v0) */
	/* Register for the re-entry jump: $t9 if no re-run word writes it,
	 * otherwise $t8. pianifica() has already dropped the hook if it writes
	 * both, so here one of the two is always fine. */
	rj = scrive_reg(hooks[idx].saved, rep, R_T9) ? R_T8 : R_T9;
	s[n++] = i_lui(rj, ret >> 16);
	s[n++] = i_ori(rj, rj, ret & 0xffff);
	s[n++] = i_jr(rj);
	s[n++] = I_NOP;
	/* max (classic+retcap+nargx) == 40 <= STUB_WORDS */
}

/* Shared return trampoline: the hooked retcap function does jr ra with ra ==
 * here. It reads $v0 (the returned value), hands it to wl_diag_exit_ret which
 * emits the RETVAL and returns orig_ra, then jumps to orig_ra with $v0
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

/* Write the entry, head word LAST. No i-cache flush here: the caller runs it
 * outside the stopped context, see scrivi_ingressi(). */
static void patch_entry(int idx)
{
	u32 *o = (u32 *)hooks[idx].addr;
	unsigned long stub = (unsigned long)stub_pool[idx];

	if (hooks[idx].use_tailj) {
		/* One word, and the only one: the words before the jump and its
		 * delay slot keep running as they are, which is what puts the
		 * arguments in place. Single store, so no intermediate state. */
		o[hooks[idx].tailw] = i_j(stub);
		return;
	}
	if (hooks[idx].use_shortj) {
		/* o[1] is nopped: left in place it would run as the delay slot of
		 * the `j`, with the registers as they were BEFORE o[0], and then a
		 * second time inside the stub. Nopped, o[0] and o[1] are both
		 * re-run by the stub as in the 4-word case, so the only condition
		 * on the window is the one pianifica() already checks, that
		 * neither word is a branch. Needs the stub in the same 256MB j
		 * region, checked in pianifica(). */
		o[1] = I_NOP;
		wmb();
		o[0] = i_j(stub);
		return;
	}
	o[3] = I_NOP;
	o[2] = i_jr(R_T9);
	o[1] = i_ori(R_T9, R_T9, stub & 0xffff);
	wmb();
	o[0] = i_lui(R_T9, stub >> 16);
}

static void restore_entry(int idx)
{
	u32 *o = (u32 *)hooks[idx].addr;
	u32 *sv = hooks[idx].saved;

	if (hooks[idx].use_tailj) {
		o[hooks[idx].tailw] = sv[hooks[idx].tailw];
		return;
	}
	if (hooks[idx].use_shortj) {
		o[1] = sv[1];
		wmb();
		o[0] = sv[0];
		return;
	}
	o[1] = sv[1]; o[2] = sv[2]; o[3] = sv[3];
	wmb();
	o[0] = sv[0];
}

/* The words patch_entry/restore_entry touch, for the flush. */
static void flush_ingresso(int idx)
{
	unsigned long a = hooks[idx].addr;

	if (hooks[idx].use_tailj) {
		a += 4 * hooks[idx].tailw;
		flush_i(a, a + 4);
		return;
	}
	flush_i(a, a + (hooks[idx].use_shortj ? 8 : 16));
}

/* Diverted at the entry, as opposed to at its call sites or with a break. */
static bool ingresso_patchato(const struct hook *h)
{
	if (h->use_sites)
		return false;
#if WD_HAVE_BP
	if (h->use_bp)
		return false;
#endif
	return true;
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
/* The buffer lives in /proc/wl_diag: it appears by itself and needs no mknod.
 * The previous route was a misc device with a dynamic minor, which meant
 * reading the minor from /proc/misc and creating it by hand on every load.
 * proc_create has the same signature on 2.6.30 and 3.4 and takes
 * file_operations, so the same call works for both. */
#define WD_PROC "wl_diag"

/* ---- piano, armamento, disarmo ---------------------------------------- */
static int eligible[NHOOK];   /* indici agganciabili */
static int n_elig;
static bool armato;

/* An op has ONE hook: the first in the table that resolves and turns out to be
 * hookable takes it, the others are dropped. That is what allows several
 * variants of the same thing to be listed for different builds -- accessor
 * names change between 6.30, 7.14.43 and 7.14.89 -- without producing two
 * records for the same access where both exist. The order in the table is the
 * order of preference. */
static u8 op_preso[256];

static void elegge(int i)
{
	eligible[n_elig++] = i;
	op_preso[hooks[i].op] = 1;
}

/*
 * Entry words go in with the other cpu parked.
 *
 * Neither the 2-word nor the 4-word patch has a valid intermediate state: with
 * the head word still original and the tail already rewritten, a caller runs a
 * prologue that is half one thing and half the other. The stopped context is
 * what keeps the other cpu from executing one, and it is paid only here and in
 * ripristina_ingressi(), never per captured op. It matters because arming does
 * not only happen at the target's COMING, where nothing of it has run yet:
 * wd_init() arms straight away when the target is already loaded, and there
 * the accessors are under traffic.
 *
 * The i-cache flush stays OUT of the callback on purpose. The flush resolved in
 * wd_init() is r4k_flush_icache_range, which on a non-MT SMP kernel goes
 * through smp_call_function(..., wait=1), and from inside stop_machine that
 * call cannot complete: the other cpu is spinning in the stopper with
 * interrupts off. Between the stores and the flush the other cpu reads the old
 * words, which is the exposure the 4-word detour has always had.
 *
 * No #ifdef needed: without CONFIG_STOP_MACHINE the header defines
 * stop_machine as local_irq_save + fn.
 */
static int scrivi_ingressi(void *unused)
{
	int i;

	for (i = 0; i < n_elig; i++)
		if (ingresso_patchato(&hooks[eligible[i]]))
			patch_entry(eligible[i]);
	return 0;
}

static int ripristina_ingressi(void *unused)
{
	int i;

	for (i = 0; i < NHOOK; i++)
		if (hooks[i].armed && ingresso_patchato(&hooks[i]))
			restore_entry(i);
	return 0;
}

/*
 * State that depends on the target being LOADED: addresses, saved words, the
 * outcome of eligibility, the call sites found. It has to be cleared before
 * every new plan, because after a re-insmod of the target the addresses are
 * different, and re-patching the old ones gives no error, it gives damage.
 *
 * shortj, retcap and nargx are NOT touched: they are the definition of the
 * hook, not state. It is the same boundary that already produced a bug when
 * state fields ended up among the table's positional initialisers.
 */
static void azzera_stato(void)
{
	unsigned long f;
	int i, j;

	testo_stato = TESTO_DA_DECIDERE;
	testo_base = 0;
	testo_size = 0;

	for (i = 0; i < NHOOK; i++) {
		hooks[i].addr = 0;
		hooks[i].armed = false;
		hooks[i].use_bp = false;
		hooks[i].use_sites = false;
		hooks[i].use_tailj = false;
		hooks[i].tailw = 0;
		hooks[i].use_shortj = false;
		hooks[i].bp_stub = NULL;
		for (j = 0; j < 4; j++)
			hooks[i].saved[j] = 0;
		n_sites[i] = 0;
	}

	/* The return pool. An entry left occupied by a thread that will never
	 * come back -- because the target unloaded while it was inside an
	 * accessor -- makes later retcaps return orig_ra in silence: no RETVAL
	 * and no error, a symptom already seen once. */
	raw_spin_lock_irqsave(&ret_lock, f);
	for (i = 0; i < RET_POOL; i++)
		ret_pool[i].task = NULL;
	ret_order = 0;
	raw_spin_unlock_irqrestore(&ret_lock, f);

	memset(op_preso, 0, sizeof(op_preso));
	n_elig = 0;
}

/* Resolves the symbols and decides, for each hook, how it gets attached. Returns
 * the number of eligible hooks. At the target's COMING the symbols are already
 * visible: load_module does list_add_rcu(&mod->list, &modules) and add_kallsyms
 * before the notification, and module_kallsyms_lookup_name on 3.4 walks the list
 * without filtering on module state. */
static int pianifica(void)
{
	int i;

	for (i = 0; i < NHOOK; i++) {
		unsigned long a;
		u32 *o;
		int j, win, branch = -1;

		if (op_preso[hooks[i].op]) {
			pr_info("wl_diag: '%s' not needed, the op is already covered\n",
				hooks[i].name);
			continue;
		}
		if (hooks[i].op == OP_DELAY && !delay) {
			pr_info("wl_diag: '%s' detached (delay=0)\n",
				hooks[i].name);
			continue;
		}

		a = kallsyms_lookup_name(hooks[i].name);
		if (!a) {
			pr_warn("wl_diag: '%s' not found (is '%s' loaded?)\n",
				hooks[i].name, target);
			continue;
		}
		if (!target_mod)
			target_mod = __module_text_address(a);
		if (!dentro_bersaglio(a)) {
			pr_warn("wl_diag: '%s' resolved outside '%s', skipping\n",
				hooks[i].name, target);
			continue;
		}
		hooks[i].addr = a;
		o = (u32 *)a;
		/* The short-j and the tail-call diversion both need the stub in
		 * the target's 256MB `j` region. When it is not there the hook is
		 * not lost: it falls back on the 4-word window and, from there,
		 * on the sites or the break, which jump with a full 32-bit
		 * address. */
		hooks[i].use_shortj = hooks[i].shortj &&
			!(((unsigned long)stub_pool[i] ^ a) >> 28);
		win = hooks[i].use_shortj ? 2 : 4;	/* words touched / re-run */
		for (j = 0; j < 4; j++)
			hooks[i].saved[j] = o[j];
		for (j = 0; j < win; j++)
			if (branch < 0 && is_branch(o[j]))
				branch = j;
		if (branch >= 0 && is_j_abs(o[branch]) &&
		    !((((unsigned long)stub_pool[i]) ^ (a + 4 * branch)) >> 28)) {
			/* The entry path always reaches an unconditional jump
			 * found in the window -- nothing before it branched --
			 * so diverting that one word catches every call, with
			 * the arguments the jump target is about to receive.
			 * Preferred over the sites and over the break: one word,
			 * no register, no trap per call. */
			hooks[i].use_tailj = true;
			hooks[i].tailw = (u8)branch;
			elegge(i);
			pr_info("wl_diag: hook plan '%s' @%px [tail-call at insn %d]\n",
				hooks[i].name, o, branch);
			continue;
		}
		if (branch >= 0 && find_sites(i, hooks[i].addr) > 0) {
			/* Not detourable in the prologue, but the call sites are
			 * patchable: the function stays intact. Preferred over the
			 * break, which needs the die notifier and is not on every
			 * kernel. */
			hooks[i].use_sites = true;
			elegge(i);
			pr_info("wl_diag: hook plan '%s' @%px [sites: %d] (branch at insn %d)\n",
				hooks[i].name, o, n_sites[i], branch);
			continue;
		}
		if (branch >= 0) {
			/* Not detourable. The 'break' path needs a single word and has no
			 * delay slot, but word 0 has to be re-run in the stub, so it
			 * cannot be PC-relative. */
			if (WD_HAVE_BP && !is_branch(o[0])) {
				hooks[i].use_bp = true;
				elegge(i);
				pr_info("wl_diag: hook plan '%s' @%px [break] (branch at insn %d)\n",
					hooks[i].name, o, branch);
			} else if (WD_HAVE_BP) {
				pr_warn("wl_diag: skipping '%s' (branch at insn %d and word 0 not re-runnable)\n",
					hooks[i].name, branch);
			} else {
				pr_warn("wl_diag: skipping '%s' (branch at insn %d; break path not available on this kernel)\n",
					hooks[i].name, branch);
			}
			continue;
		}
		/* The stub needs ONE register for the re-entry jump, and it cannot be
		 * one of those the re-run words write. With both t8 and t9 taken
		 * nothing safe is left: better no hook than a re-entry that jumps
		 * to a badly computed address. */
		if (scrive_reg(hooks[i].saved, win, R_T9) &&
		    scrive_reg(hooks[i].saved, win, R_T8)) {
			pr_warn("wl_diag: skipping '%s' (the displaced words write both t8 and t9)\n",
				hooks[i].name);
			continue;
		}
		elegge(i);
		pr_info("wl_diag: hook plan '%s' @%px%s\n", hooks[i].name, o,
			hooks[i].use_shortj ? " [short-j]" :
			hooks[i].shortj ? " [4 parole: stub fuori regione j]" : "");
	}

	/* Drop the fallbacks whose underlying accessor got hooked: see
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
				pr_info("wl_diag: skipping '%s': it is a thunk on "
					"'%s', which did get hooked -- "
					"otherwise every op would come out twice\n",
					h->name, h->ripiego_di);
				h->addr = 0;
				continue;
			}
			eligible[w++] = eligible[k];
		}
		n_elig = w;
	}

	return n_elig;
}

/* The words this plan will overwrite in the target text for one hook: the
 * detour window (4, or 2 for the short-j), the diverted tail-call word, the
 * break word, or -- on the call-site route -- the hi and lo of every patched
 * pair. Mirrors what patch_entry()/patch_sites() actually touch. The buffer
 * holds at most 2*MAX_SITES words (the sites route is the widest). */
static int parole_patchate(int idx, unsigned long *out)
{
	const struct hook *h = &hooks[idx];
	int n = 0, k;

	if (h->use_sites) {
		for (k = 0; k < n_sites[idx]; k++) {
			out[n++] = (unsigned long)sites[idx][k].hi;
			out[n++] = (unsigned long)sites[idx][k].lo;
		}
		return n;
	}
	if (h->use_tailj) {
		out[n++] = h->addr + 4 * h->tailw;
		return n;
	}
#if WD_HAVE_BP
	if (h->use_bp) {
		out[n++] = h->addr;
		return n;
	}
#endif
	out[n++] = h->addr;
	out[n++] = h->addr + 4;
	if (!h->use_shortj) {
		out[n++] = h->addr + 8;
		out[n++] = h->addr + 12;
	}
	return n;
}

/* Two hooks that write the same word corrupt each other: the case that put a
 * shm_set call-site rewrite inside the chanspec_set detour window, and two
 * sites on one pair in general. The stores are silent and order-dependent, so
 * the whole plan is refused here, before any of it reaches the target -- the
 * exit is clean because nothing has been patched yet. */
static bool piano_senza_collisioni(void)
{
	unsigned long wa[2 * MAX_SITES], wb[2 * MAX_SITES];
	int a, b, na, nb, i, j;

	for (a = 0; a < n_elig; a++) {
		na = parole_patchate(eligible[a], wa);
		for (b = a + 1; b < n_elig; b++) {
			nb = parole_patchate(eligible[b], wb);
			for (i = 0; i < na; i++)
				for (j = 0; j < nb; j++)
					if (wa[i] == wb[j]) {
						pr_err("wl_diag: '%s' and '%s' both patch %px: refusing to arm\n",
						       hooks[eligible[a]].name,
						       hooks[eligible[b]].name,
						       (void *)wa[i]);
						return false;
					}
		}
	}
	return true;
}

static int arma(void)
{
	int i;

	azzera_stato();

	if (!pianifica()) {
		pr_err("wl_diag: no hookable function in '%s'\n", target);
		return -ENODEV;
	}
	if (!arm) {
		pr_info("wl_diag: DRY-RUN (%d hooks planned). arm=1 to apply.\n",
			n_elig);
		return 0;
	}
	if (!p_flush_icache) {
		pr_err("wl_diag: no i-cache flush resolvable, staying in DRY-RUN\n");
		return 0;
	}
	if (!piano_senza_collisioni())
		return 0;

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
		(unsigned long)stub_pool + STUB_POOL_BYTES);
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
			pr_info("wl_diag: return trampoline @%px\n",
				(void *)ret_trampoline);
		}
	}
#if WD_HAVE_BP
	{
		int any_bp = 0, err;

		for (i = 0; i < n_elig; i++)
			if (hooks[eligible[i]].use_bp)
				any_bp = 1;
		/* the notifier has to be registered BEFORE placing the breaks, or
		 * the first trap ends up in do_trap_or_bp -> panic. */
		if (any_bp && !bp_registered) {
			err = register_die_notifier(&wd_bp_nb);
			if (err) {
				pr_err("wl_diag: register_die_notifier: %d, staying in DRY-RUN\n",
				       err);
				return 0;
			}
			bp_registered = true;
		}
	}
#endif

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
	}
	/* The break is a single word and needs no stopped context; the entry
	 * detours do, and they all go in inside one stop_machine. */
	stop_machine(scrivi_ingressi, NULL, NULL);
	for (i = 0; i < n_elig; i++)
		if (ingresso_patchato(&hooks[eligible[i]])) {
			flush_ingresso(eligible[i]);
			hooks[eligible[i]].armed = true;
		}
	armato = true;
	pr_info("wl_diag: ARMED (%d hooks) -> /proc/wl_diag\n", n_elig);
	return 0;
}

/*
 * Restores the prologues and waits for the in-flight stubs to leave.
 *
 * From GOING this is the only useful moment: on 3.4 delete_module calls
 * mod->exit() and ONLY AFTER that the notification, so by here the target's
 * detach is finished but free_module() has not started and the text is still
 * mapped. Restoring the words keeps a fresh call from entering a stub, and
 * synchronize_sched waits for those already inside; the context is sleepable
 * (blocking notifier, module_mutex not held on the rmmod path).
 */
static void disarma(void)
{
	int i;

	if (!armato)
		return;

	/* stop new ra diversions before restoring the prologues; in-flight stubs
	 * that have already diverted still return through ret_tramp (static and
	 * valid), and synchronize_sched waits for them to complete. */
	ret_trampoline = 0;

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
		}
	stop_machine(ripristina_ingressi, NULL, NULL);
	for (i = 0; i < NHOOK; i++)
		if (hooks[i].armed && ingresso_patchato(&hooks[i])) {
			flush_ingresso(i);
			hooks[i].armed = false;
		}
#if WD_HAVE_BP
	/* the notifier is unregistered AFTER the words have been restored: if a
	 * break were left around with no handler, the trap would end in a panic. */
	if (bp_registered) {
		unregister_die_notifier(&wd_bp_nb);
		bp_registered = false;
	}
#endif
	/* give the in-flight stubs time to complete */
	synchronize_sched();
	armato = false;
	pr_info("wl_diag: DISARMED (lost so far: %d, filtered: %d)\n",
		atomic_read(&drops), atomic_read(&filtered));
}

/* ---- init/exit -------------------------------------------------------- */
static int __init wd_init(void)
{
	int err;

	if (bump_ptr) {
		/* virt_addr_valid() is out: on MIPS it pulls in min_low_pfn,
		 * which the kernel does not export. KSEG0 below high_memory is
		 * the RAM the cursor can live in. */
		if ((bump_ptr & 3) || bump_ptr < PAGE_OFFSET ||
		    bump_ptr >= (unsigned long)high_memory) {
			pr_err("wl_diag: bump_ptr %px is not a word-aligned KSEG0 address below high_memory\n",
			       (void *)bump_ptr);
			return -EINVAL;
		}
		pr_info("wl_diag: allocator cursor @%px = %px (restore_alloc=%d)\n",
			(void *)bump_ptr, (void *)bump_read(), restore_alloc);
	} else if (restore_alloc) {
		pr_warn("wl_diag: restore_alloc=1 without bump_ptr does nothing\n");
	}

	parse_skipphyrd();

	stub_pool = kmalloc(STUB_POOL_BYTES, GFP_KERNEL);
	if (!stub_pool) {
		pr_err("wl_diag: kmalloc of %u B for the stub pool failed\n",
		       (unsigned int)STUB_POOL_BYTES);
		return -ENOMEM;
	}
	pr_info("wl_diag: stub pool @%px (%u B): the one-word routes need it in "
		"the target's 256MB j region\n",
		stub_pool, (unsigned int)STUB_POOL_BYTES);

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
		kfree(stub_pool);
		stub_pool = NULL;
		return -ENOMEM;
	}
	err = kfifo_init(&fifo, fifo_buf, fifo_recs * sizeof(struct wldiag_rec));
	if (err) {
		pr_err("wl_diag: kfifo_init: %d\n", err);
		vfree(fifo_buf);
		fifo_buf = NULL;
		kfree(stub_pool);
		stub_pool = NULL;
		return err;
	}
	pr_info("wl_diag: queue %d records (%d KB)\n", fifo_recs,
		(int)(fifo_recs * sizeof(struct wldiag_rec) / 1024));

	/* 0600 and not 0400: the write injects a MARK record, which is how cycle
	 * boundaries are placed while the reader stays open. */
	if (!proc_create(WD_PROC, 0600, NULL, &wd_fops)) {
		pr_err("wl_diag: proc_create(/proc/%s) failed\n", WD_PROC);
		vfree(fifo_buf);
		fifo_buf = NULL;
		kfree(stub_pool);
		stub_pool = NULL;
		return -ENOMEM;
	}

	/* resolve the i-cache flush. NB: this kernel has KALLSYMS but not
	 * KALLSYMS_ALL, so kallsyms only exposes TEXT symbols (functions): the
	 * pointer variable 'flush_icache_range' (in BSS) is invisible. We resolve
	 * the R4K cache-layer function directly, with fallbacks. It sits here and
	 * not in arma(): it is kernel text, and it does not change between one
	 * load of the target and the next. */
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

	/* Fatal, and not a warning: without the notifier an rmmod of the target is
	 * not reported to us, the patched prologues stay on memory that gets
	 * freed, and the restore at unload writes there. */
	err = register_module_notifier(&wd_mod_nb);
	if (err) {
		pr_err("wl_diag: register_module_notifier: %d\n", err);
		remove_proc_entry(WD_PROC, NULL);
		vfree(fifo_buf);
		fifo_buf = NULL;
		kfree(stub_pool);
		stub_pool = NULL;
		return err;
	}
	mod_nb_registered = true;

	/* If the target is already loaded we arm straight away, so the "hook a wl
	 * that is already running" use stays as it was. If it is not there, that
	 * is not an error: we wait for its COMING. */
	if (arma())
		pr_info("wl_diag: waiting for '%s' to load\n", target);

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
	vfree(fifo_buf);
	fifo_buf = NULL;
	/* after disarma(), which restores the words and then waits with
	 * synchronize_sched for the stubs already in flight */
	kfree(stub_pool);
	stub_pool = NULL;
	pr_info("wl_diag: unloaded (lost: %d, filtered: %d)\n",
		atomic_read(&drops), atomic_read(&filtered));
}

module_init(wd_init);
module_exit(wd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Inline-detour PHY/radio/PMU tracer for Broadcom wl (no kprobes)");
