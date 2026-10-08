// SPDX-License-Identifier: GPL-2.0
/*
 * wl_mmio_trap -- tracer for the raw MMIO accesses wl makes through its own
 * register pointer, the R_REG/W_REG inlines that wl_diag.c's function-entry
 * hooks cannot see because there is no call to hook.
 *
 * How it works, end to end:
 *
 *   - wl's own ioremap of the device's BAR is located by itself: the PCI
 *     layer gives the BAR's physical address, and a walk of the kernel page
 *     tables over the vmalloc range gives the virtual address whose pte
 *     carries it. Nothing has to be passed in (see win_find.c).
 *   - Every page of that mapping is made permanently invalid, so every one
 *     of wl's accesses traps. It is never made valid again while armed.
 *   - This module keeps its OWN alias of the same physical pages and
 *     performs the access through that, from inside the fault, after
 *     decoding wl's instruction. wl's instruction never executes, so
 *     nothing is ever split in two and no window of validity is opened for
 *     another cpu to slip through.
 *   - The fault is caught at fixup_exception(), with a one-word
 *     `break BRK_KPROBE_BP`: do_bp() reaches the die chain with or without
 *     CONFIG_KPROBES (bp_hook.h), which the DIE_PAGE_FAULT path in
 *     do_page_fault() does not (it is inside #ifdef CONFIG_KPROBES, and
 *     this build has KPROBES off). fixup_exception() is chosen over
 *     do_page_fault() because it is only reached from no_context, i.e.
 *     essentially never in normal operation, so instrumenting it does not
 *     tax every userspace fault in the system.
 *
 * Cost: every register access becomes a full exception plus a decode. wl's
 * SPINWAIT polling loops slow down by orders of magnitude and bring-up
 * timeouts can fire. Arm around the phase of interest rather than for the
 * whole life of the module:
 *
 *     echo off > /proc/wl_mmio_trap      # stop trapping, window valid
 *     echo on  > /proc/wl_mmio_trap      # trap again
 *     echo status > /proc/wl_mmio_trap   # counters to dmesg
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/log2.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/err.h>
#include <asm/atomic.h>
#include <asm/ptrace.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/addrspace.h>

#include "compat.h"
#include "kern_syms.h"
#include "bp_hook.h"
#include "mmio_pte.h"
#include "win_find.h"
#include "win_redirect.h"
#include "mips_mmio_emulate.h"
#include "wl_ring.h"
#include "dma_dd.h"

#define PROC_NAME "wl_mmio_trap"

static int autoarm = 1;
module_param(autoarm, int, 0444);
MODULE_PARM_DESC(autoarm, "1=engage as soon as the window is found (default), 0=wait for 'on' on /proc");

static char *target = "wl";
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "module whose load starts the search for the window (default wl)");

static uint pci_vendor = 0x14e4;
module_param(pci_vendor, uint, 0444);
MODULE_PARM_DESC(pci_vendor, "PCI vendor of the device owning the window (default 0x14e4)");

static uint pci_device = PCI_ANY_ID;
module_param(pci_device, uint, 0444);
MODULE_PARM_DESC(pci_device, "PCI device id, or PCI_ANY_ID (default)");

static int bar = 0;
module_param(bar, int, 0444);
MODULE_PARM_DESC(bar, "BAR index carrying the register window (default 0)");

static ulong win_base;
module_param(win_base, ulong, 0444);
MODULE_PARM_DESC(win_base, "override: kernel VA of the window, skipping discovery");

static ulong win_len;
module_param(win_len, ulong, 0444);
MODULE_PARM_DESC(win_len, "override: length of win_base, in bytes");

static ulong win_phys;
module_param(win_phys, ulong, 0444);
MODULE_PARM_DESC(win_phys, "override: physical base of the window, skipping the PCI lookup but not the search for its mapping (read it out of /proc/bus/pci/devices)");

static ulong win_phys_len = 8192;
module_param(win_phys_len, ulong, 0444);
MODULE_PARM_DESC(win_phys_len, "length of win_phys, in bytes (default 8192)");

static int poll_ms = 200;
module_param(poll_ms, int, 0444);
MODULE_PARM_DESC(poll_ms, "interval between discovery attempts while waiting for the ioremap");

static int poll_secs = 60;
module_param(poll_secs, int, 0444);
MODULE_PARM_DESC(poll_secs, "how long to keep looking for the window before giving up");

/* A bring-up burst runs at 150-230k accesses per second on a BCM63168 and
 * lasts seconds, so the queue's job is to absorb it whole: the reader
 * cannot drain at that rate on a cpu that is already spending most of its
 * cycles taking exceptions. 8192 records filled in 47 ms on that board and
 * everything until the burst subsided was lost. */
#define FIFO_RECS_DEF 65536
static int redirect;
module_param(redirect, int, 0444);
MODULE_PARM_DESC(redirect, "1=answer the driver's __ioremap() of the window with a page-table-backed uncached mapping of this module's own, so the window can be trapped on a board where __ioremap() would return CKSEG1. Hands the driver a pointer it did not ask for; see win_redirect.h");

static int ioremap_watch;
module_param(ioremap_watch, int, 0444);
MODULE_PARM_DESC(ioremap_watch, "1=log every __ioremap() call and go no further. Diagnostic: it says whether the driver reaches the window through that function at all, and what it asks for");

static ulong init_mm_addr;
module_param(init_mm_addr, ulong, 0444);
MODULE_PARM_DESC(init_mm_addr, "address of the kernel's init_mm, from System.map, when it cannot be found at runtime");

static ulong klookup;
module_param(klookup, ulong, 0444);
MODULE_PARM_DESC(klookup, "address of kallsyms_lookup_name, from /proc/kallsyms; required on kernels that do not export it (before 2.6.33), which the module then calls to resolve everything else it needs");

static int fifo_recs = FIFO_RECS_DEF;
module_param(fifo_recs, int, 0444);
MODULE_PARM_DESC(fifo_recs, "record capacity of the ring buffer, rounded up to a power of two (28 bytes each, vmalloc'd)");

static int stop_on_full = 1;
module_param(stop_on_full, int, 0644);
MODULE_PARM_DESC(stop_on_full, "1=stop trapping the first time the queue overflows (default), so the capture is a contiguous run and the driver goes back to full speed instead of crawling while its accesses are discarded. 0=keep trapping and count the losses");

/* The TX descriptors and the start of their buffers, read out of memory at
 * each TX index write (dma_dd.h). Off until dd_len is set; both can be
 * changed while the trap runs, through /sys/module/wl_mmio_trap/parameters. */
static uint dd_len;
module_param(dd_len, uint, 0644);
MODULE_PARM_DESC(dd_len, "bytes of each TX buffer to record at an index write, up to 256; 0 = off (default). 168 covers the 4-byte TX offload header, the 124-byte d11 TX header and an 802.11 header");

static uint dd_budget = 256;
module_param(dd_budget, uint, 0644);
MODULE_PARM_DESC(dd_budget, "TX descriptors left to record; counts down to 0, write it again for more (default 256)");

static ulong dd_bus_off;
module_param(dd_bus_off, ulong, 0444);
MODULE_PARM_DESC(dd_bus_off, "subtracted from a DMA bus address to get the physical one (default 0, the identity the BCM63xx PCIe inbound window gives)");

/* ---- records ------------------------------------------------------------
 * Separate stream from wl_diag's. The op numbers are free in wl_diag.c's
 * enum wldiag_op, which runs to OP_IOVAR_SET=54 plus OP_DROP=255, so the
 * two can be merged by a decoder later without a renumber.
 *
 * addr is the OFFSET into the window, not the kernel VA: the VA changes
 * from boot to boot and carries no information for a decoder. */
#define OP_MMIO_R 60
#define OP_MMIO_W 61
#define OP_MMIO_MARK 62
/* A TX descriptor: addr its bus address, val the buffer's, aux the channel
 * in bits 31:24, the DD_F_* flags in 23:16 and the buffer bytes that follow
 * in 15:0. Then OP_MMIO_DATA records, twelve bytes each in addr/val/aux as
 * MARK carries its label: the 16 bytes of the descriptor as they are in
 * memory, then the buffer bytes, the last record padded with zeros. The
 * whole group is queued under one lock hold, so its sequence numbers are
 * consecutive. */
#define OP_MMIO_DD 63
#define OP_MMIO_DATA 64

/* aux bits alongside the access width */
#define MMIO_AUX_WIDTH_MASK 0x000000ff
#define MMIO_AUX_DELAY_SLOT 0x00000100

struct wl_mmio_rec {
	u64 ts_ns; u32 seq; u32 addr; u32 val; u32 aux;
	u8 op; u8 cpu; u16 _pad;
} __packed;

static struct wl_ring ring;
static void *fifo_buf;
static DEFINE_RAW_SPINLOCK(fifo_lock);
static atomic_t rec_seq = ATOMIC_INIT(0);
static atomic_t drops = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(rec_waitq);

static atomic_t n_emulated = ATOMIC_INIT(0);
static atomic_t n_delay_slot = ATOMIC_INIT(0);
static atomic_t n_unaligned = ATOMIC_INIT(0);
static atomic_t n_mismatch = ATOMIC_INIT(0);
static atomic_t n_not_ls = ATOMIC_INIT(0);
static atomic_t n_foreign = ATOMIC_INIT(0);	/* faults that were not ours */
static atomic_t n_dd = ATOMIC_INIT(0);		/* TX descriptors recorded */

static bool overflow_seen;
static void overflow_work_fn(struct work_struct *w);
static DECLARE_WORK(overflow_work, overflow_work_fn);

/* Called with fifo_lock held. Returns false when the queue was full. */
static bool push_rec_locked(struct wl_mmio_rec *r)
{
	r->ts_ns = mmio_now_ns();
	r->seq = (u32)atomic_inc_return(&rec_seq);
	r->cpu = (u8)raw_smp_processor_id();
	r->_pad = 0;

	if (wl_ring_put(&ring, r))
		return true;
	atomic_inc(&drops);
	return false;
}

/* Disengaging means invalidating ptes and flushing the tlb, an IPI this
 * context cannot issue. Hand it to a worker and let the few records that
 * arrive in the meantime be counted as losses. */
static void note_full(void)
{
	if (stop_on_full && !overflow_seen) {
		overflow_seen = true;
		schedule_work(&overflow_work);
	}
}

/* No wake_up from here on purpose: this runs inside the exception that
 * trapped wl's access, once per register access, and the reader can afford
 * to poll. proc_read() sleeps on a timeout instead. */
static void push_rec(struct wl_mmio_rec *r)
{
	unsigned long flags;
	bool ok;

	raw_spin_lock_irqsave(&fifo_lock, flags);
	ok = push_rec_locked(r);
	raw_spin_unlock_irqrestore(&fifo_lock, flags);
	if (!ok)
		note_full();
}

static void log_rec(u8 op, u32 addr, u32 val, u32 aux)
{
	struct wl_mmio_rec r;

	r.addr = addr; r.val = val; r.aux = aux;
	r.op = op;
	push_rec(&r);
}

/* A label, in the queue like any other record, so it is ordered against the
 * accesses around it and not against the clock of whoever wrote it. The
 * twelve bytes of addr/val/aux carry it, the same shape wl_diag.c's own
 * MARK uses. */
static void emit_mark(const char *label)
{
	struct wl_mmio_rec r;

	memset(&r, 0, sizeof(r));
	r.op = OP_MMIO_MARK;
	memcpy((char *)&r.addr, label, 12);
	push_rec(&r);
}

/* ---- the trapped region ------------------------------------------------- */
struct trap_region {
	unsigned long base;
	size_t len;
	void __iomem *alias;	/* this module's own view of the same pages */
	struct mmio_window *win;
	bool owns_alias;	/* false for the self-test's lowmem alias */
};

/* ---- TX descriptors ------------------------------------------------------
 * The memory the DMA engine reads, read the way it does: uncached, through
 * CKSEG1, after wl has written its caches back to post the frame. Only RAM
 * below 512 MB is read; anything else, a bus address that is not memory
 * included, is refused, and the walk records the descriptor without its
 * buffer or stops. */
static struct dd_state dd;
static DEFINE_RAW_SPINLOCK(dd_lock);

/* pfn_valid() reads min_low_pfn, which MIPS does not export to modules.
 * The memory of these boards is one bank from PHYS_OFFSET up, and
 * max_mapnr, which is exported, ends it. */
static bool dd_ram_pfn(unsigned long pfn)
{
	return pfn >= ARCH_PFN_OFFSET && pfn < max_mapnr;
}

static bool dd_mem_read(void *ctx, u32 bus, void *dst, u32 len)
{
	const volatile u8 *p;
	unsigned long phys = (unsigned long)bus - dd_bus_off;
	u8 *out = dst;
	u32 i;

	(void)ctx;
	if (!len)
		return true;
	if (phys + len < phys || phys + len > WIN_KSEG1_LIMIT ||
	    !dd_ram_pfn(phys >> PAGE_SHIFT) ||
	    !dd_ram_pfn((phys + len - 1) >> PAGE_SHIFT))
		return false;

	p = (const volatile u8 *)CKSEG1ADDR(phys);
	for (i = 0; i < len; i++)
		out[i] = p[i];
	return true;
}

/* A ring set up before the trap was engaged: its address register, read
 * through the alias right after wl wrote the same channel's index, so the
 * window is on the core wl was talking to. */
static u32 dd_ring_addr(void *ctx, unsigned int chan)
{
	struct trap_region *r = ctx;

	return __raw_readl((char __iomem *)r->alias + 0x208 + 0x40 * chan);
}

static void dd_emit(void *ctx, unsigned int chan, u32 slot, const u8 *raw,
		    const struct dd_desc *d, const u8 *buf, u32 n)
{
	struct wl_mmio_rec rec;
	unsigned long flags;
	u32 total = DD_SIZE + n, i, k;
	bool ok = true;

	(void)ctx;
	raw_spin_lock_irqsave(&fifo_lock, flags);
	memset(&rec, 0, sizeof(rec));
	rec.op = OP_MMIO_DD;
	rec.addr = slot;
	rec.val = d->addrlo;
	rec.aux = (chan << 24) | ((u32)d->flags << 16) | n;
	ok = push_rec_locked(&rec);
	for (i = 0; ok && i < total; i += 12) {
		u8 *p = (u8 *)&rec.addr;

		memset(&rec, 0, sizeof(rec));
		rec.op = OP_MMIO_DATA;
		for (k = 0; k < 12 && i + k < total; k++)
			p[k] = i + k < DD_SIZE ? raw[i + k] : buf[i + k - DD_SIZE];
		ok = push_rec_locked(&rec);
	}
	raw_spin_unlock_irqrestore(&fifo_lock, flags);

	if (ok)
		atomic_inc(&n_dd);
	else
		note_full();
}

static const struct dd_ops dd_ops = {
	.read = dd_mem_read,
	.ring_addr = dd_ring_addr,
	.emit = dd_emit,
};

static void dd_on_mmio_write(struct trap_region *r, u32 off, u32 val)
{
	unsigned long flags;
	u32 budget;

	raw_spin_lock_irqsave(&dd_lock, flags);
	budget = ACCESS_ONCE(dd_budget);
	dd_on_write(&dd, &dd_ops, r, off, val, ACCESS_ONCE(dd_len), &budget);
	dd_budget = budget;
	raw_spin_unlock_irqrestore(&dd_lock, flags);
}

static struct trap_region live;
static struct trap_region *cur;		/* NULL: nothing is being trapped */
static struct bp_site *fixup_site;
static bool engaged;
static DEFINE_MUTEX(engage_lock);

static enum bp_action fixup_bp(struct pt_regs *regs, void *ctx)
{
	struct trap_region *r = ACCESS_ONCE(cur);
	struct pt_regs *inner = (struct pt_regs *)regs->regs[4];
	struct mmio_emu_result res;
	enum mmio_emu_status st;
	unsigned long va;

	(void)ctx;

	if (!r || !inner)
		return BP_PASS;

	va = inner->cp0_badvaddr;
	if (va < r->base || va >= r->base + r->len) {
		atomic_inc(&n_foreign);
		return BP_PASS;
	}

	st = mips_mmio_emulate_one(inner, va,
				   (void __iomem *)((char __iomem *)r->alias +
						    (va - r->base)),
				   &res);
	if (st != MMIO_EMU_OK) {
		switch (st) {
		case MMIO_EMU_DELAY_SLOT:	atomic_inc(&n_delay_slot); break;
		case MMIO_EMU_UNALIGNED_LR:	atomic_inc(&n_unaligned); break;
		case MMIO_EMU_ADDR_MISMATCH:	atomic_inc(&n_mismatch); break;
		default:			atomic_inc(&n_not_ls); break;
		}
		/* Nothing was touched: let the real fixup_exception run and
		 * the fault surface exactly as it would without us. */
		return BP_PASS;
	}

	atomic_inc(&n_emulated);
	log_rec(res.is_write ? OP_MMIO_W : OP_MMIO_R,
		(u32)(va - r->base), res.value,
		(u32)res.width | (res.delay_slot ? MMIO_AUX_DELAY_SLOT : 0));
	if (res.is_write && res.width == 4 && r == &live)
		dd_on_mmio_write(r, (u32)(va - r->base), res.value);

	/* Make fixup_exception() return 1 to its caller without running:
	 * at its first word $ra is still the caller's and no frame has been
	 * pushed, so setting v0 and jumping to $ra is a clean return. */
	regs->regs[2] = 1;
	regs->cp0_epc = regs->regs[31];
	return BP_RESUMED;
}

/* ---- __ioremap observation ----------------------------------------------
 * Read-only. The point is to find out whether the driver's window comes
 * from this function and with which arguments, before anything considers
 * answering the call on its behalf.
 *
 * o32 argument placement depends on the width of phys_t: with a 64-bit
 * physical address each phys_t takes an aligned register pair (big-endian,
 * so the high half is in the lower-numbered register) and flags lands in
 * the fifth word slot, at sp+16. The module is built against the same
 * .config as the kernel, so the choice is made at compile time. */
static struct bp_site *ioremap_site;

static void ioremap_args(struct pt_regs *regs, u64 *phys, u64 *size,
			 unsigned long *flags)
{
#ifdef CONFIG_64BIT_PHYS_ADDR
	*phys = ((u64)regs->regs[4] << 32) | (u32)regs->regs[5];
	*size = ((u64)regs->regs[6] << 32) | (u32)regs->regs[7];
	*flags = *(unsigned long *)(regs->regs[29] + 16);
#else
	*phys = (u32)regs->regs[4];
	*size = (u32)regs->regs[5];
	*flags = regs->regs[6];
#endif
}

static enum bp_action ioremap_bp(struct pt_regs *regs, void *ctx)
{
	u64 phys, size;
	unsigned long flags, va;

	(void)ctx;
	ioremap_args(regs, &phys, &size, &flags);

	if (redirect && (flags & _CACHE_MASK) == _CACHE_UNCACHED &&
	    win_redirect_answer((phys_addr_t)phys, (size_t)size, &va)) {
		/* Return va to the caller without running __ioremap(): at its
		 * first word $ra is still the caller's and no frame has been
		 * pushed. */
		regs->regs[2] = va;
		regs->cp0_epc = regs->regs[31];
		pr_info("wl_mmio_trap: __ioremap(%08llx, %llu) answered with %p instead of CKSEG1\n",
			phys, size, (void *)va);
		return BP_RESUMED;
	}

	if (!ioremap_watch)
		return BP_PASS;

	pr_info("wl_mmio_trap: __ioremap(%08llx, %llu, %lx) from %p%s\n",
		phys, size, flags, (void *)regs->regs[31],
		(phys + size - 1 < WIN_KSEG1_LIMIT &&
		 (flags & _CACHE_MASK) == _CACHE_UNCACHED)
			? " -- returns CKSEG1, no pte, untrappable" : "");

	return BP_PASS;
}

/* The driver unmapping the window would take the area's page tables down
 * with it, along with the ptes this module has saved. Refuse the call; the
 * area goes when the module does. __iounmap() returns void, so there is
 * nothing to set but the resume address. */
static struct bp_site *iounmap_site;

static enum bp_action iounmap_bp(struct pt_regs *regs, void *ctx)
{
	(void)ctx;

	if (!win_redirect_owns(regs->regs[4]))
		return BP_PASS;

	pr_info("wl_mmio_trap: refusing the driver's iounmap of %p; the mapping stays until this module goes\n",
		(void *)regs->regs[4]);
	regs->cp0_epc = regs->regs[31];
	return BP_RESUMED;
}

/* ---- self-test ----------------------------------------------------------
 * Drives the whole path -- breakpoint, notifier, decode, alias access,
 * resume -- on a throwaway vmalloc page, before anything touches wl's
 * window. If the breakpoint route were wrong the fault would end in an
 * Oops in the insmod process rather than taking the machine down, which is
 * the other reason this is the first thing that runs. */
static bool selftest_run(void)
{
	static const u32 pattern = 0x5a3c69f0;
	struct trap_region t;
	volatile u32 *p;
	phys_addr_t phys;
	u32 *page;
	u32 got;
	int before;
	bool ok = false;

	page = vmalloc(PAGE_SIZE);
	if (!page) {
		pr_err("wl_mmio_trap: selftest: vmalloc failed\n");
		return false;
	}

	memset(&t, 0, sizeof(t));
	t.win = mmio_pte_arm((unsigned long)page, PAGE_SIZE);
	if (!t.win) {
		pr_err("wl_mmio_trap: selftest: no simple pte behind a fresh vmalloc page\n");
		vfree(page);
		return false;
	}
	t.base = mmio_pte_base(t.win);
	t.len = mmio_pte_len(t.win);
	phys = mmio_pte_phys(t.win);

	/* The pte this walked has to be the page's own. A page table that
	 * is not the kernel's yields a plausible wrong pfn rather than an
	 * error, which is exactly the failure this catches. */
	if (phys != (phys_addr_t)vmalloc_to_pfn(page) << PAGE_SHIFT) {
		pr_err("wl_mmio_trap: selftest: walked to pfn %08llx, vmalloc_to_pfn says %08lx -- wrong page table\n",
		       (unsigned long long)phys >> PAGE_SHIFT,
		       vmalloc_to_pfn(page));
		goto out;
	}

	/* The alias has to be UNCACHED, like the real window's is, or the
	 * test measures the cache instead of the emulator: a cached alias
	 * takes the emulated read from a line that the cached vmalloc view
	 * of the same page filled, and a cached emulated write would sit in
	 * a line instead of reaching memory. CKSEG1 is the uncached view of
	 * the page and needs no mapping, but it only reaches the low 512 MB
	 * of physical space -- CPHYSADDR() masks the rest away silently. */
	if (phys >= WIN_KSEG1_LIMIT) {
		pr_err("wl_mmio_trap: selftest: the test page is at %08llx, outside the range CKSEG1 can address; no uncached alias for it\n",
		       (unsigned long long)phys);
		goto out;
	}
	t.alias = (void __iomem *)CKSEG1ADDR((unsigned long)phys);

	/* The page still has its permanent cached KSEG0 identity mapping,
	 * and vmalloc gave it back without touching it, so drop whatever
	 * lines either cached view may be holding before writing the
	 * pattern through the uncached one. After this, memory and every
	 * view of it agree. */
	flush_data_cache_page((unsigned long)page);
	__raw_writel(pattern, t.alias);

	before = atomic_read(&n_emulated);
	cur = &t;
	smp_wmb();
	mmio_pte_close(t.win);

	p = (volatile u32 *)page;
	got = *p;

	mmio_pte_open(t.win);
	cur = NULL;
	smp_wmb();
	/* t lives on this stack: nobody may still be inside fixup_bp()
	 * holding a pointer to it when this frame goes. The die chain is an
	 * atomic notifier, so its handlers sit in an rcu read section. */
	synchronize_rcu();

	if (atomic_read(&n_emulated) == before)
		pr_err("wl_mmio_trap: selftest: the access did not trap -- breakpoint or tlb flush not effective\n");
	else if (got != pattern)
		pr_err("wl_mmio_trap: selftest: emulated load returned %08x, expected %08x\n",
		       got, pattern);
	else
		ok = true;

	if (ok)
		pr_info("wl_mmio_trap: selftest passed: trapped, decoded, resumed\n");
out:
	mmio_pte_release(t.win);
	vfree(page);
	return ok;
}

/* ---- engage / disengage -------------------------------------------------- */
static void disengage_locked(void)
{
	if (!engaged)
		return;
	mmio_pte_open(live.win);
	cur = NULL;
	engaged = false;
	smp_wmb();
	synchronize_rcu();
	pr_info("wl_mmio_trap: disengaged\n");
}

static void release_window_locked(void)
{
	disengage_locked();
	if (live.win) {
		mmio_pte_release(live.win);
		live.win = NULL;
	}
	if (live.alias) {
		if (live.owns_alias)
			iounmap(live.alias);
		live.alias = NULL;
		live.owns_alias = false;
	}
	live.base = 0;
	live.len = 0;
}

static int engage_locked(void)
{
	unsigned long flags;

	if (engaged)
		return 0;
	if (!live.win || !live.alias)
		return -ENODEV;
	overflow_seen = false;
	raw_spin_lock_irqsave(&dd_lock, flags);
	dd_init(&dd);
	raw_spin_unlock_irqrestore(&dd_lock, flags);
	cur = &live;
	smp_wmb();
	mmio_pte_close(live.win);
	engaged = true;
	pr_info("wl_mmio_trap: engaged on [%p, %p)\n",
		(void *)live.base, (void *)(live.base + live.len));
	return 0;
}

static void overflow_work_fn(struct work_struct *w)
{
	(void)w;

	mutex_lock(&engage_lock);
	if (engaged) {
		pr_warn("wl_mmio_trap: queue full after %d records; stopping the trap so the rest of the capture is one contiguous run. Raise fifo_recs, or set stop_on_full=0 to keep going and count the losses.\n",
			atomic_read(&n_emulated));
		disengage_locked();
	}
	mutex_unlock(&engage_lock);
}

/* Finds the window and maps this module's alias of it. Leaves the trap
 * off; engage_locked() is the separate step that starts faulting. */
/* Printed once, however many times discovery is retried. */
static bool bar_reported;

/* The window is one this module built and handed to the driver, so its
 * physical base is known before any pte is looked at, and CKSEG1 is the
 * uncached view of the same pages -- reachable precisely because the window
 * is in the low 512 MB, which is the fact that made the detour necessary. */
static int acquire_redirected_locked(void)
{
	struct win_bar b;
	phys_addr_t phys;
	unsigned long va;
	size_t len;
	int ret;

	if (!win_redirect_base()) {
		ret = win_find_bar(pci_vendor, pci_device, bar, !bar_reported, &b);
		bar_reported = true;
		if (ret)
			return ret;
		ret = win_redirect_create(b.phys, b.len);
		if (ret)
			return ret;
	}

	va = win_redirect_base();
	len = win_redirect_len();
	phys = win_redirect_phys();

	live.win = mmio_pte_arm(va, len);
	if (!live.win) {
		pr_err("wl_mmio_trap: the mapping this module just built has no simple ptes\n");
		return -EINVAL;
	}
	live.base = mmio_pte_base(live.win);
	live.len = mmio_pte_len(live.win);
	live.alias = (void __iomem *)CKSEG1ADDR((unsigned long)phys);
	live.owns_alias = false;

	pr_info("wl_mmio_trap: window %p +%zu (phys %08llx), alias %p\n",
		(void *)live.base, live.len, (unsigned long long)phys,
		live.alias);
	return 0;
}

/* The window is the driver's own, or one named by an override. Its
 * physical base comes out of the pte, so the alias can only be mapped once
 * the walk has happened. */
static int acquire_found_locked(void)
{
	struct win_bar b;
	phys_addr_t phys;
	unsigned long va;
	size_t len;
	int ret;

	if (win_base && win_len) {
		va = win_base;
		len = win_len;
		pr_info("wl_mmio_trap: using the win_base override %p +%zu\n",
			(void *)va, len);
	} else if (win_phys) {
		if (!bar_reported) {
			bar_reported = true;
			pr_info("wl_mmio_trap: using the win_phys override %08llx +%lu\n",
				(unsigned long long)win_phys, win_phys_len);
			if (win_phys < WIN_KSEG1_LIMIT)
				pr_warn("wl_mmio_trap: %08llx is below the %08lx that __ioremap() short-circuits to CKSEG1; there may be no pte behind it at all\n",
					(unsigned long long)win_phys,
					WIN_KSEG1_LIMIT);
		}
		ret = win_find_mapping((phys_addr_t)win_phys,
				       (size_t)win_phys_len, &va, &len);
		if (ret == -EAGAIN)
			return -EAGAIN;
		if (ret == -ERANGE)
			pr_warn("wl_mmio_trap: only %zu of %lu bytes are mapped contiguously; trapping what is there\n",
				len, win_phys_len);
	} else {
		ret = win_find_bar(pci_vendor, pci_device, bar, !bar_reported, &b);
		if (ret)
			return ret;
		if (!bar_reported) {
			bar_reported = true;
			if (b.phys < WIN_KSEG1_LIMIT)
				pr_warn("wl_mmio_trap: BAR%d is at %08llx, below the %08lx that __ioremap() short-circuits to CKSEG1. If wl maps it uncached -- and pci_iomap does -- there is no pte behind it, and redirect=1 is the way round it.\n",
					bar, (unsigned long long)b.phys,
					WIN_KSEG1_LIMIT);
		}
		ret = win_find_mapping(b.phys, b.len, &va, &len);
		if (ret == -EAGAIN)
			return -EAGAIN;
		if (ret == -ERANGE)
			pr_warn("wl_mmio_trap: only %zu of %zu bytes of the BAR are mapped contiguously; trapping what is there\n",
				len, b.len);
	}

	live.win = mmio_pte_arm(va, len);
	if (!live.win) {
		pr_err("wl_mmio_trap: %p +%zu is not a set of simple present ptes\n",
		       (void *)va, len);
		return -EINVAL;
	}
	live.base = mmio_pte_base(live.win);
	live.len = mmio_pte_len(live.win);

	phys = mmio_pte_phys(live.win);
	live.alias = ioremap_nocache(phys, live.len);
	if (!live.alias) {
		pr_err("wl_mmio_trap: cannot map an alias of %08llx +%zu\n",
		       (unsigned long long)phys, live.len);
		mmio_pte_release(live.win);
		live.win = NULL;
		return -ENOMEM;
	}
	live.owns_alias = true;

	pr_info("wl_mmio_trap: window %p +%zu (phys %08llx), alias %p\n",
		(void *)live.base, live.len, (unsigned long long)phys,
		live.alias);
	return 0;
}

static int acquire_window_locked(void)
{
	if (live.win)
		return 0;
	return redirect ? acquire_redirected_locked() : acquire_found_locked();
}

/* ---- discovery poll ----------------------------------------------------- */
static void probe_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(probe_work, probe_work_fn);
static int probe_attempts;
static int probe_max;
static bool shutting_down;

static void probe_work_fn(struct work_struct *w)
{
	int ret;

	mutex_lock(&engage_lock);
	if (shutting_down) {
		mutex_unlock(&engage_lock);
		return;
	}
	ret = acquire_window_locked();
	if (!ret) {
		if (autoarm)
			engage_locked();
		else
			pr_info("wl_mmio_trap: window found, autoarm=0: waiting for 'on'\n");
		mutex_unlock(&engage_lock);
		return;
	}
	if (ret != -EAGAIN && ret != -ENODEV) {
		pr_err("wl_mmio_trap: giving up on discovery: %d\n", ret);
		mutex_unlock(&engage_lock);
		return;
	}
	if (++probe_attempts >= probe_max) {
		pr_err("wl_mmio_trap: no mapping of the BAR appeared after %d attempts. Either %s never ioremapped it, or the BAR sits in the low 512MB of physical space and is reached through CKSEG1, which has no pte to invalidate.\n",
		       probe_attempts, target);
		mutex_unlock(&engage_lock);
		return;
	}
	mutex_unlock(&engage_lock);
	schedule_delayed_work(&probe_work, msecs_to_jiffies(poll_ms));
}

static void start_probing(void)
{
	probe_attempts = 0;
	probe_max = (poll_secs * 1000) / (poll_ms > 0 ? poll_ms : 1) + 1;
	schedule_delayed_work(&probe_work, msecs_to_jiffies(poll_ms));
}

/* ---- target module notifier --------------------------------------------- */
static int mod_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct module *mod = data;

	if (!target || !mod || strcmp(mod->name, target))
		return NOTIFY_DONE;

	switch (action) {
	case MODULE_STATE_COMING:
		pr_info("wl_mmio_trap: %s is coming up, looking for the window\n",
			target);
		if (redirect) {
			/* COMING runs before the module's init, in process
			 * context: the last moment at which the mapping can
			 * be built and trapped before the driver asks for
			 * it. A delayed poll would arrive after the driver
			 * had already taken a CKSEG1 pointer. */
			mutex_lock(&engage_lock);
			if (!acquire_window_locked() && autoarm)
				engage_locked();
			mutex_unlock(&engage_lock);
		} else {
			start_probing();
		}
		break;
	case MODULE_STATE_GOING:
		/* The mapping is about to go away with the driver. Put the
		 * ptes back before iounmap walks them. */
		cancel_delayed_work_sync(&probe_work);
		mutex_lock(&engage_lock);
		release_window_locked();
		win_redirect_destroy();
		mutex_unlock(&engage_lock);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block mod_nb = {
	.notifier_call = mod_notify,
};

/* ---- /proc --------------------------------------------------------------- */
static void print_status(void)
{
	pr_info("wl_mmio_trap: %s, window %p +%zu; emulated=%d tx_descriptors=%d drops=%d foreign=%d unhandled: delay_slot=%d unaligned=%d mismatch=%d not_loadstore=%d\n",
		engaged ? "engaged" : (live.win ? "armed, not engaged" : "no window"),
		(void *)live.base, live.len,
		atomic_read(&n_emulated), atomic_read(&n_dd), atomic_read(&drops),
		atomic_read(&n_foreign), atomic_read(&n_delay_slot),
		atomic_read(&n_unaligned), atomic_read(&n_mismatch),
		atomic_read(&n_not_ls));
}

/* Copies whole records, as many as fit in count, straight out of the ring.
 * Returns the bytes copied, 0 when it is empty, or -EFAULT. */
static ssize_t ring_to_user(char __user *buf, size_t count)
{
	size_t done = 0;

	for (;;) {
		const void *p;
		u32 n = wl_ring_peek(&ring,
				     (count - done) / sizeof(struct wl_mmio_rec),
				     &p);

		if (!n)
			return done;
		if (copy_to_user(buf + done, p, n * sizeof(struct wl_mmio_rec)))
			return -EFAULT;
		wl_ring_consume(&ring, n);
		done += n * sizeof(struct wl_mmio_rec);
	}
}

static ssize_t proc_read(struct file *f, char __user *buf, size_t count,
			 loff_t *ppos)
{
	ssize_t copied;
	u32 d;

	if (count < sizeof(struct wl_mmio_rec))
		return 0;

	d = atomic_xchg(&drops, 0);
	if (d) {
		struct wl_mmio_rec r;

		memset(&r, 0, sizeof(r));
		r.ts_ns = mmio_now_ns();
		r.op = 255;		/* OP_DROP, same sentinel wl_diag uses */
		r.aux = d;
		if (copy_to_user(buf, &r, sizeof(r)))
			return -EFAULT;
		return sizeof(r);
	}

	for (;;) {
		/* The ring needs no lock on this side: the producers serialise
		 * against each other on fifo_lock and there is one consumer.
		 * Copying straight to userspace, without the lock and without
		 * a bounce buffer, is what lets a read drain as much as the
		 * caller asked for -- a 32-record cap turned a burst into
		 * hundreds of syscalls a second on a cpu that had none to
		 * spare. */
		copied = ring_to_user(buf, count);
		if (copied)
			return copied;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* A timeout rather than a wakeup from the writer: the writer
		 * is the fault path and stays as lean as it can. 50 ms of
		 * latency on a trace nobody reads live costs nothing. */
		if (wait_event_interruptible_timeout(rec_waitq,
				wl_ring_len(&ring) || atomic_read(&drops),
				HZ / 20) < 0)
			return -ERESTARTSYS;
		if (atomic_read(&drops))
			return 0;
	}
}

static ssize_t proc_write(struct file *f, const char __user *buf, size_t count,
			  loff_t *ppos)
{
	char cmd[32];
	size_t n = count < sizeof(cmd) - 1 ? count : sizeof(cmd) - 1;
	int i;

	memset(cmd, 0, sizeof(cmd));
	if (n && copy_from_user(cmd, buf, n))
		return -EFAULT;
	for (i = 0; i < (int)sizeof(cmd); i++)
		if (cmd[i] == '\n' || cmd[i] == '\r')
			cmd[i] = 0;

	mutex_lock(&engage_lock);
	if (!strcmp(cmd, "on")) {
		if (!live.win)
			acquire_window_locked();
		engage_locked();
	} else if (!strcmp(cmd, "off")) {
		disengage_locked();
	} else if (!strcmp(cmd, "release")) {
		release_window_locked();
	} else if (!strcmp(cmd, "status")) {
		print_status();
	} else if (!strncmp(cmd, "mark ", 5)) {
		emit_mark(cmd + 5);
	} else {
		mutex_unlock(&engage_lock);
		return -EINVAL;
	}
	mutex_unlock(&engage_lock);
	return count;
}

static const struct file_operations proc_fops = {
	.owner = THIS_MODULE,
	.read  = proc_read,
	.write = proc_write,
	.llseek = no_llseek,
};

/* ---- init / exit --------------------------------------------------------- */
static int __init wl_mmio_trap_init(void)
{
	u32 slots;
	int ret;

	ret = ks_init(init_mm_addr, klookup);
	if (ret)
		return ret;

	slots = roundup_pow_of_two((unsigned int)fifo_recs);
	fifo_buf = vmalloc((size_t)slots * sizeof(struct wl_mmio_rec));
	if (!fifo_buf)
		return -ENOMEM;
	wl_ring_init(&ring, fifo_buf, slots, sizeof(struct wl_mmio_rec));

	ret = bp_hook_init();
	if (ret)
		goto err_fifo;

	fixup_site = bp_hook_add(ks_fixup_exception(), fixup_bp, NULL);
	if (IS_ERR(fixup_site)) {
		ret = PTR_ERR(fixup_site);
		fixup_site = NULL;
		pr_err("wl_mmio_trap: cannot instrument fixup_exception: %d\n", ret);
		goto err_bp;
	}

	if (!selftest_run()) {
		ret = -ENODEV;
		goto err_bp;
	}

	if (ioremap_watch || redirect) {
		ioremap_site = bp_hook_add((unsigned long)&__ioremap,
					   ioremap_bp, NULL);
		if (IS_ERR(ioremap_site)) {
			ret = PTR_ERR(ioremap_site);
			ioremap_site = NULL;
			pr_err("wl_mmio_trap: cannot instrument __ioremap: %d\n",
			       ret);
			goto err_bp;
		}
	}
	if (redirect) {
		iounmap_site = bp_hook_add((unsigned long)&__iounmap,
					   iounmap_bp, NULL);
		if (IS_ERR(iounmap_site)) {
			ret = PTR_ERR(iounmap_site);
			iounmap_site = NULL;
			pr_err("wl_mmio_trap: cannot instrument __iounmap: %d\n",
			       ret);
			goto err_bp;
		}
		pr_info("wl_mmio_trap: will answer the window's __ioremap; load %s now\n",
			target);
	} else if (ioremap_watch) {
		pr_info("wl_mmio_trap: watching __ioremap; load %s now\n", target);
	}

	if (!proc_create(PROC_NAME, 0440, NULL, &proc_fops)) {
		ret = -ENOMEM;
		goto err_bp;
	}

	ret = register_module_notifier(&mod_nb);
	if (ret)
		goto err_proc;

	/* Nothing to acquire up front when redirecting: the window does not
	 * exist until the driver asks for it and this module answers, which
	 * happens from the COMING notifier. A driver that is already loaded
	 * is holding a CKSEG1 pointer and will not ask again, so it has to
	 * be unloaded and loaded once more with this module in place. */
	if (!redirect) {
		mutex_lock(&engage_lock);
		ret = acquire_window_locked();
		if (!ret && autoarm)
			engage_locked();
		mutex_unlock(&engage_lock);
		if (ret)
			start_probing();
	}

	return 0;

err_proc:
	remove_proc_entry(PROC_NAME, NULL);
err_bp:
	bp_hook_exit();
err_fifo:
	vfree(fifo_buf);
	fifo_buf = NULL;
	return ret;
}

static void __exit wl_mmio_trap_exit(void)
{
	mutex_lock(&engage_lock);
	shutting_down = true;
	mutex_unlock(&engage_lock);

	unregister_module_notifier(&mod_nb);
	cancel_delayed_work_sync(&probe_work);

	mutex_lock(&engage_lock);
	release_window_locked();
	win_redirect_destroy();
	mutex_unlock(&engage_lock);

	/* Breakpoint out and notifier down only after the window is valid
	 * again: with the word still planted and nobody listening, do_bp()
	 * would fall through to do_trap_or_bp() and resume on the break
	 * itself, which is a loop, not a crash. */
	bp_hook_exit();

	remove_proc_entry(PROC_NAME, NULL);
	vfree(fifo_buf);

	print_status();
	pr_info("wl_mmio_trap: unloaded\n");
}

module_init(wl_mmio_trap_init);
module_exit(wl_mmio_trap_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Raw MMIO read/write tracer for wl's register window; companion to wl_diag");
