// SPDX-License-Identifier: GPL-2.0
/*
 * wl_mmio_trap, variant for kernel 4.4 on ARM (Broadcom Northstar: BCM4709,
 * Cortex-A9, 2-level MMU, little-endian).
 *
 * Traces the register accesses wl-diag cannot see: the inline readl/writel
 * and the R_REG/W_REG loads and stores through wl's own register pointer,
 * which have no function to hook. The mechanism is the MIPS trap's, retargeted:
 *
 *   1. Find wl's ioremap of the device BAR by walking init_mm's page tables
 *      for the BAR's physical pages (win_find.c, shared with the MIPS build).
 *   2. Keep this module's own uncached alias of the same physical pages
 *      (ioremap(), which is device memory on ARM).
 *   3. Invalidate wl's mapping at the pte level -- clear L_PTE_PRESENT /
 *      L_PTE_VALID / L_PTE_YOUNG, keep the PFN -- so every access to the
 *      window faults (mmio_pte.c).
 *   4. The fault lands in do_DataAbort, which dispatches FSR 7, "page
 *      translation fault", through fsr_info[7].fn. This module swaps that
 *      slot for its own handler: if the address is in the window it decodes
 *      and emulates the one trapped load/store through the alias and resumes
 *      (arm_mmio_emulate.c); otherwise it chains to the kernel's original
 *      handler. No kprobes, no breakpoint -- the dispatch table is the hook,
 *      the ARM analogue of the MIPS break on fixup_exception.
 *
 * wl's instruction never runs, so the access happens inside the exception at
 * the address wl last pointed the window to, and another CPU's access through
 * the window faults too and is never let past unseen.
 *
 * The records are those of the MIPS build (ops 60-64), so ../decode-wl-mmio.py
 * reads the stream unchanged; they go out big-endian, timestamps are
 * local_clock() (sched_clock() is not exported to modules on ARM).
 *
 * This first ARM version does not record TX descriptors (dma_dd on the MIPS
 * side); the register trace is the whole of it.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <asm/io.h>
#include <asm/cacheflush.h>

#include "arm_syms.h"
#include "mmio_pte.h"
#include "win_find.h"
#include "wl_ring.h"
#include "arm_mmio_emulate.h"
#include "dma_dd.h"

#define PROC_NAME "wl_mmio_trap"

static int autoarm = 1;
module_param(autoarm, int, 0444);
MODULE_PARM_DESC(autoarm, "1=invalidate the window as soon as it is found; 0=wait for 'on'");

static uint pci_vendor = 0x14e4;
module_param(pci_vendor, uint, 0444);
MODULE_PARM_DESC(pci_vendor, "PCI vendor of the radio (default 0x14e4)");

static uint pci_device = PCI_ANY_ID;
module_param(pci_device, uint, 0444);
MODULE_PARM_DESC(pci_device, "PCI device id of the radio, or any network function");

static int bar = 0;
module_param(bar, int, 0444);
MODULE_PARM_DESC(bar, "BAR index of the register window (default 0)");

static ulong win_phys;
module_param(win_phys, ulong, 0444);
MODULE_PARM_DESC(win_phys, "physical base of the window, when it is not a PCI BAR");

static ulong win_phys_len = 8192;
module_param(win_phys_len, ulong, 0444);
MODULE_PARM_DESC(win_phys_len, "length of win_phys (default 8192)");

static int poll_ms = 200;
module_param(poll_ms, int, 0444);
MODULE_PARM_DESC(poll_ms, "interval between discovery attempts while waiting for the ioremap");

static int poll_secs = 60;
module_param(poll_secs, int, 0444);
MODULE_PARM_DESC(poll_secs, "how long to wait for wl to map the window");

static ulong fsr_info_fn7;
module_param(fsr_info_fn7, ulong, 0444);
MODULE_PARM_DESC(fsr_info_fn7, "address of fsr_info[7].fn, for a kernel without KALLSYMS_ALL (from System.map)");

#define FIFO_RECS_DEF 65536
static int fifo_recs = FIFO_RECS_DEF;
module_param(fifo_recs, int, 0444);
MODULE_PARM_DESC(fifo_recs, "ring capacity in records, rounded down to a power of two");

static int stop_on_full = 1;
module_param(stop_on_full, int, 0644);
MODULE_PARM_DESC(stop_on_full, "1=open the window and stop tracing at the first overflow (default)");

static uint dd_len;
module_param(dd_len, uint, 0644);
MODULE_PARM_DESC(dd_len, "bytes of each TX buffer to record at an index write, up to 256; 0 = off (default). 168 covers the 4-byte TX offload header, the 124-byte d11 TX header and an 802.11 header");

static uint dd_budget = 256;
module_param(dd_budget, uint, 0644);
MODULE_PARM_DESC(dd_budget, "TX descriptors left to record; counts down to 0, write it again for more (default 256)");

static ulong dd_bus_off;
module_param(dd_bus_off, ulong, 0444);
MODULE_PARM_DESC(dd_bus_off, "subtracted from a DMA bus address to get the physical one (default 0, the identity the Northstar PCIe inbound window gives)");

/* ---- records: the MIPS build's format, ops 60-64 ----------------------- */
#define OP_MMIO_R    60
#define OP_MMIO_W    61
#define OP_MMIO_MARK 62
#define OP_MMIO_DD   63
#define OP_MMIO_DATA 64
#define OP_DROP     255

#define MMIO_AUX_WIDTH_MASK 0x000000ff

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
static atomic_t n_mismatch = ATOMIC_INIT(0);
static atomic_t n_not_ls = ATOMIC_INIT(0);
static atomic_t n_cond = ATOMIC_INIT(0);
static atomic_t n_foreign = ATOMIC_INIT(0);	/* faults that were not ours */
static atomic_t n_dd = ATOMIC_INIT(0);		/* TX descriptors recorded */

static bool overflow_seen;
static void overflow_work_fn(struct work_struct *w);
static DECLARE_WORK(overflow_work, overflow_work_fn);

static inline u64 now_ns(void)
{
	return local_clock();
}

/* Called with fifo_lock held. False when the ring was full. */
static bool push_rec_locked(struct wl_mmio_rec *r)
{
	r->ts_ns = now_ns();
	r->seq = (u32)atomic_inc_return(&rec_seq);
	r->cpu = (u8)raw_smp_processor_id();
	r->_pad = 0;
	if (wl_ring_put(&ring, r))
		return true;
	atomic_inc(&drops);
	return false;
}

static void note_full(void)
{
	if (stop_on_full && !overflow_seen) {
		overflow_seen = true;
		schedule_work(&overflow_work);
	}
}

/* No wake_up: this runs inside the exception that trapped the access, once
 * per access; the reader polls. */
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
	phys_addr_t phys;
	void __iomem *alias;		/* this module's uncached view */
	struct mmio_window *win;
};

static struct trap_region live;
static DEFINE_RAW_SPINLOCK(region_lock);

/* The region the fault handler emulates against: &live in normal operation,
 * a temporary on the self-test's stack while it runs. The handler reads it
 * once per fault; a write is published with smp_wmb() before the window is
 * closed and cleared after it is opened, so the handler never sees a region
 * whose ptes are not the ones it is faulting on. */
static struct trap_region *active;

/* The original fault handler for FSR 7, saved when the slot is taken. */
static fault_fn_t orig_fault_fn;
static fault_fn_t *fault_slot;

static bool region_owns(const struct trap_region *r, unsigned long va)
{
	return r && r->alias && va >= r->base && va < r->base + r->len;
}

/* ---- TX descriptors -----------------------------------------------------
 * The d11 TX header and the DMA descriptor pointing at it never cross the
 * window: the engine reads them from RAM. The TX channel's index write does
 * cross it, and dma_dd.c walks the descriptors posted since the previous
 * index out of RAM at that write. Off until dd_len is set; one record group
 * per descriptor (OP_MMIO_DD + OP_MMIO_DATA), as on MIPS.
 */
static struct dd_state dd;
static DEFINE_RAW_SPINLOCK(dd_lock);

/*
 * Read @len bytes of RAM at the physical address @bus - dd_bus_off. ARM
 * ioremap() refuses RAM, so the read is through the cached linear map,
 * preceded by a clean+invalidate of the range so it reflects what wl wrote
 * before posting the frame.
 *
 * A highmem page has no linear address, and kmap from inside a data abort
 * is not safe, so a buffer there is refused (dd_dump records the descriptor
 * with DD_F_NOBUF). The 16-byte descriptors live in the coherent ring, in
 * lowmem; it is a frame buffer that could be high.
 *
 * SALAME: on a Cortex-A9 with an L2C-310 outer cache, a clean of the inner
 * cache alone does not guarantee the line reached DRAM if wl's write is
 * still in the outer cache; wl flushes the descriptor and buffer for DMA
 * before the index write, so in practice they are in DRAM by the time this
 * runs, but that ordering is assumed, not read from the blob.
 */
static bool dd_mem_read(void *ctx, u32 bus, void *dst, u32 len)
{
	unsigned long phys = (unsigned long)bus - dd_bus_off;
	unsigned long pfn = phys >> PAGE_SHIFT;
	unsigned long last = (phys + len - 1) >> PAGE_SHIFT;
	void *va;

	(void)ctx;
	if (!len)
		return true;
	if (phys + len < phys || !pfn_valid(pfn) || !pfn_valid(last))
		return false;
	if (PageHighMem(pfn_to_page(pfn)) || PageHighMem(pfn_to_page(last)))
		return false;

	va = phys_to_virt(phys);
	__cpuc_flush_dcache_area(va, len);
	memcpy(dst, va, len);
	return true;
}

/* A ring set up before the trap was engaged: its address register, read
 * through the alias right after wl wrote the same channel's index, so the
 * window is on the core wl was talking to. DMA64 layout: channel n at
 * 0x200 + 0x40 * n, TX ring address low word at +0x08. */
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
	bool ok;

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

/*
 * The replacement for fsr_info[7].fn. On a fault inside the window, decode
 * and emulate the one load/store and return 0 (handled); otherwise chain to
 * the handler this one replaced. Runs in the data-abort with interrupts as
 * do_DataAbort left them.
 */
static int trap_fault_fn(unsigned long addr, unsigned int fsr,
			 struct pt_regs *regs)
{
	struct trap_region *r = READ_ONCE(active);
	struct arm_emu_result res;
	struct arm_emu_regs er;
	enum arm_emu_status st;
	void __iomem *access;
	u32 insn;
	int i;

	if (!region_owns(r, addr)) {
		atomic_inc(&n_foreign);
		return orig_fault_fn(addr, fsr, regs);
	}

	/* A32 only: the faulting instruction is one 32-bit word at the pc. */
	insn = *(const u32 *)regs->ARM_pc;
	for (i = 0; i < 16; i++)
		er.r[i] = regs->uregs[i];
	er.cpsr = regs->ARM_cpsr;

	access = (char __iomem *)r->alias + (addr - r->base);
	st = arm_mmio_emulate_one(&er, insn, addr, access, &res);
	if (st != ARM_MMIO_OK) {
		switch (st) {
		case ARM_MMIO_ADDR_MISMATCH: atomic_inc(&n_mismatch); break;
		case ARM_MMIO_COND_FALSE:    atomic_inc(&n_cond); break;
		default:                     atomic_inc(&n_not_ls); break;
		}
		/* Refused: let the kernel's handler oops on it rather than
		 * guess. It points at a real decode gap. */
		return orig_fault_fn(addr, fsr, regs);
	}

	for (i = 0; i < 16; i++)
		regs->uregs[i] = er.r[i];

	log_rec(res.is_write ? OP_MMIO_W : OP_MMIO_R,
		(u32)(addr - r->base), res.value, res.width);
	atomic_inc(&n_emulated);
	if (res.is_write && res.width == 4 && r == &live)
		dd_on_mmio_write(r, (u32)(addr - r->base), res.value);
	return 0;
}

static void take_slot(void)
{
	fault_slot = as_fault_slot();
	orig_fault_fn = *fault_slot;
	smp_wmb();		/* orig_fault_fn visible before the slot points here */
	WRITE_ONCE(*fault_slot, trap_fault_fn);
}

static void give_slot_back(void)
{
	if (fault_slot && READ_ONCE(*fault_slot) == trap_fault_fn)
		WRITE_ONCE(*fault_slot, orig_fault_fn);
	fault_slot = NULL;
}

/* ---- arm / disarm ------------------------------------------------------- */

static void region_close(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&region_lock, flags);
	if (live.win && !mmio_pte_is_closed(live.win)) {
		WRITE_ONCE(active, &live);
		smp_wmb();		/* active before the ptes go invalid */
		mmio_pte_close(live.win);
	}
	raw_spin_unlock_irqrestore(&region_lock, flags);
}

static void region_open(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&region_lock, flags);
	if (live.win && mmio_pte_is_closed(live.win)) {
		mmio_pte_open(live.win);
		smp_wmb();		/* ptes valid before active is cleared */
		WRITE_ONCE(active, NULL);
	}
	raw_spin_unlock_irqrestore(&region_lock, flags);
}

/* Must run with the window OPEN: closing invalidates ptes and flushes the
 * tlb, which cannot happen from inside the fault. The worker is where the
 * overflow stop opens it. */
static void overflow_work_fn(struct work_struct *w)
{
	region_open();
	pr_warn("wl_mmio_trap: ring overflowed, window opened and tracing stopped (stop_on_full=1)\n");
}

/* ---- window discovery --------------------------------------------------- */

static int find_window(phys_addr_t *phys, size_t *len)
{
	struct win_bar b;
	int err;

	if (win_phys) {
		*phys = (phys_addr_t)win_phys;
		*len = (size_t)win_phys_len;
		return 0;
	}
	err = win_find_bar(pci_vendor, pci_device, bar, true, &b);
	if (err) {
		pr_err("wl_mmio_trap: no matching PCI function; pass win_phys= for a non-PCI window\n");
		return err;
	}
	*phys = b.phys;
	*len = b.len;
	return 0;
}

/* Poll init_mm until wl has ioremapped the BAR, then build the window and
 * the alias. Returns 0 with live filled, or a negative errno. */
static int arm_window(void)
{
	phys_addr_t phys;
	size_t len, mapped;
	unsigned long va;
	int tries, err;

	err = find_window(&phys, &len);
	if (err)
		return err;

	for (tries = 0; tries <= poll_secs * 1000 / (poll_ms ?: 1); tries++) {
		if (win_find_mapping(phys, len, &va, &mapped) == 0)
			break;
		msleep(poll_ms);
	}
	if (!va) {
		pr_err("wl_mmio_trap: no mapping of %pa appeared; is wl loaded and the interface up?\n",
		       &phys);
		return -EAGAIN;
	}

	live.alias = ioremap(phys, mapped);
	if (!live.alias) {
		pr_err("wl_mmio_trap: ioremap of the alias failed\n");
		return -ENOMEM;
	}
	live.win = mmio_pte_arm(va, mapped);
	if (!live.win) {
		iounmap(live.alias);
		live.alias = NULL;
		pr_err("wl_mmio_trap: could not take the window's ptes\n");
		return -EINVAL;
	}
	live.base = va;
	live.len = mapped;
	live.phys = phys;

	if (autoarm)
		region_close();
	pr_info("wl_mmio_trap: window %pa at %p, %zu bytes; %s\n",
		&phys, (void *)va, mapped,
		autoarm ? "trapping" : "autoarm=0: waiting for 'on'");
	return 0;
}

static void disarm_window(void)
{
	give_slot_back();
	if (live.win) {
		mmio_pte_release(live.win);
		live.win = NULL;
	}
	if (live.alias) {
		iounmap(live.alias);
		live.alias = NULL;
	}
	live.base = 0;
	live.len = 0;
}

/* ---- self-test ----------------------------------------------------------
 * Trap, decode and resume a load this module issues itself, on a fresh
 * vmalloc page, before touching wl. It needs the slot already taken. A
 * failure here means the pte invalidation, the tlb flush or the dispatch
 * hook is not effective on this build, and nothing has touched the device.
 */
static bool selftest_run(void)
{
	static const u32 pattern = 0x5a3c69f0;
	struct trap_region t;
	volatile u32 *p;
	phys_addr_t phys;
	u32 *page, got;
	struct page *test_pg;
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

	/* The pte walked must be the page's own: a wrong page table yields a
	 * plausible wrong pfn rather than an error, which this catches. */
	if (phys != (phys_addr_t)vmalloc_to_pfn(page) << PAGE_SHIFT) {
		pr_err("wl_mmio_trap: selftest: walked to pfn %08llx, vmalloc_to_pfn says %08lx -- wrong page table\n",
		       (unsigned long long)phys >> PAGE_SHIFT, vmalloc_to_pfn(page));
		goto out;
	}

	/* ARM's ioremap() refuses RAM (pfn_valid), so the test page cannot get
	 * the device-memory alias the real window does. A second vmap() of the
	 * same page stands in: cacheable like the vmalloc view, and the
	 * Cortex-A9 D-cache is PIPT, so the two views agree without aliasing.
	 * The test still proves trap, decode, emulate and resume through an
	 * alias; that the window's alias is uncached rests on ioremap() of the
	 * BAR. */
	test_pg = vmalloc_to_page(page);
	t.alias = (void __iomem *)vmap(&test_pg, 1, VM_MAP, PAGE_KERNEL);
	if (!t.alias) {
		pr_err("wl_mmio_trap: selftest: vmap of the test alias failed\n");
		goto out;
	}
	flush_kernel_vmap_range(page, PAGE_SIZE);
	__raw_writel(pattern, t.alias);

	before = atomic_read(&n_emulated);
	WRITE_ONCE(active, &t);
	smp_wmb();
	mmio_pte_close(t.win);

	p = (volatile u32 *)page;
	got = *p;			/* traps, emulates, resumes */

	mmio_pte_open(t.win);
	smp_wmb();
	WRITE_ONCE(active, NULL);
	/* t is on this stack: no fault may still be reading it when the frame
	 * goes. do_DataAbort runs with interrupts disabled on this cpu, and
	 * open()'s tlb flush has IPI'd the others, so after it no cpu is in
	 * the handler against t; a grace period is belt-and-braces. */
	synchronize_sched();

	if (atomic_read(&n_emulated) == before)
		pr_err("wl_mmio_trap: selftest: the access did not trap -- tlb flush or dispatch hook not effective\n");
	else if (got != pattern)
		pr_err("wl_mmio_trap: selftest: emulated load returned %08x, expected %08x\n",
		       got, pattern);
	else
		ok = true;

	if (ok)
		pr_info("wl_mmio_trap: selftest passed: trapped, decoded, resumed\n");
out:
	if (t.alias)
		vunmap((__force void *)t.alias);
	mmio_pte_release(t.win);
	vfree(page);
	return ok;
}

/* ---- /proc/wl_mmio_trap ------------------------------------------------- */

/* Big-endian, the stream decode-wl-mmio.py reads, as the MIPS build writes
 * it natively. A MARK's label and a DATA record's bytes are already in
 * stream order in addr/val/aux (memcpy'd there), so those stay as they are. */
static void to_wire(struct wl_mmio_rec *r)
{
	r->ts_ns = (__force u64)cpu_to_be64(r->ts_ns);
	r->seq = (__force u32)cpu_to_be32(r->seq);
	if (r->op != OP_MMIO_MARK && r->op != OP_MMIO_DATA) {
		r->addr = (__force u32)cpu_to_be32(r->addr);
		r->val = (__force u32)cpu_to_be32(r->val);
		r->aux = (__force u32)cpu_to_be32(r->aux);
	}
	r->_pad = (__force u16)cpu_to_be16(r->_pad);
}

/*
 * The stream is a byte stream: a read takes as many bytes as fit, and the
 * rest of a record waits for that reader's next read. Returning 0 for a
 * buffer smaller than a record would be end-of-file to a reader that fills
 * a buffer with repeated reads (busybox cat stopped after 146 records, 4088
 * bytes). The half-read record belongs to the open file, so the writes
 * (on, off, mark) do not touch it, and a reader that quits mid-record only
 * loses that record (the sequence numbers show the gap); the next reader
 * starts on a record.
 */
struct rd_state {
	struct mutex lock;
	u8 pend[sizeof(struct wl_mmio_rec)];
	unsigned int off, len;
};

/* Taking a record off the ring: one consumer at a time. */
static DEFINE_MUTEX(ring_rd_lock);

/* The next record, on the wire, into st->pend; false if there is none. */
static bool rd_next(struct rd_state *st)
{
	struct wl_mmio_rec r;
	const void *p;
	bool ok = true;
	u32 d;

	mutex_lock(&ring_rd_lock);
	d = atomic_xchg(&drops, 0);
	if (d) {
		memset(&r, 0, sizeof(r));
		r.ts_ns = now_ns();
		r.op = OP_DROP;
		r.aux = d;
	} else if (wl_ring_peek(&ring, 1, &p)) {
		memcpy(&r, p, sizeof(r));
		wl_ring_consume(&ring, 1);
	} else {
		ok = false;
	}
	mutex_unlock(&ring_rd_lock);
	if (!ok)
		return false;
	to_wire(&r);
	memcpy(st->pend, &r, sizeof(r));
	st->off = 0;
	st->len = sizeof(r);
	return true;
}

static ssize_t proc_read(struct file *f, char __user *buf, size_t count,
			 loff_t *ppos)
{
	struct rd_state *st = f->private_data;
	size_t done = 0, n;
	ssize_t ret;

	if (mutex_lock_interruptible(&st->lock))
		return -ERESTARTSYS;
	while (done < count) {
		if (st->off == st->len && !rd_next(st)) {
			if (done)
				break;
			if (f->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				goto out;
			}
			if (wait_event_interruptible_timeout(rec_waitq,
				wl_ring_len(&ring) || atomic_read(&drops), HZ / 5) < 0) {
				ret = -ERESTARTSYS;
				goto out;
			}
			continue;
		}
		n = min_t(size_t, count - done, st->len - st->off);
		if (copy_to_user(buf + done, st->pend + st->off, n)) {
			ret = -EFAULT;
			goto out;
		}
		st->off += n;
		done += n;
	}
	ret = done;
out:
	mutex_unlock(&st->lock);
	return ret;
}

static int proc_open(struct inode *inode, struct file *f)
{
	struct rd_state *st;

	if (!(f->f_mode & FMODE_READ))
		return 0;
	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	mutex_init(&st->lock);
	f->private_data = st;
	return 0;
}

static int proc_release(struct inode *inode, struct file *f)
{
	kfree(f->private_data);
	return 0;
}

static ssize_t proc_write(struct file *f, const char __user *buf, size_t count,
			  loff_t *ppos)
{
	char cmd[24];	/* "mark " + a 12-character label + newline */
	size_t n = min(count, sizeof(cmd) - 1);

	if (copy_from_user(cmd, buf, n))
		return -EFAULT;
	cmd[n] = 0;
	if (n && (cmd[n - 1] == '\n'))
		cmd[n - 1] = 0;

	if (!strcmp(cmd, "on")) {
		region_close();
	} else if (!strcmp(cmd, "off")) {
		region_open();
	} else if (!strcmp(cmd, "status")) {
		pr_info("wl_mmio_trap: %s, emulated=%d dd=%d foreign=%d not-ls=%d mismatch=%d cond=%d drops=%d\n",
			live.win && mmio_pte_is_closed(live.win) ? "trapping" : "open",
			atomic_read(&n_emulated), atomic_read(&n_dd),
			atomic_read(&n_foreign), atomic_read(&n_not_ls),
			atomic_read(&n_mismatch), atomic_read(&n_cond),
			atomic_read(&drops));
	} else if (!strncmp(cmd, "mark ", 5)) {
		char label[13] = { 0 };

		strncpy(label, cmd + 5, 12);
		emit_mark(label);
	} else {
		return -EINVAL;
	}
	return count;
}

static unsigned int proc_poll(struct file *f, poll_table *wait)
{
	struct rd_state *st = f->private_data;

	poll_wait(f, &rec_waitq, wait);
	if ((st && READ_ONCE(st->off) < READ_ONCE(st->len)) ||
	    wl_ring_len(&ring) || atomic_read(&drops))
		return POLLIN | POLLRDNORM;
	return 0;
}

static const struct file_operations proc_fops = {
	.owner = THIS_MODULE,
	.open = proc_open,
	.release = proc_release,
	.read = proc_read,
	.write = proc_write,
	.poll = proc_poll,
	.llseek = no_llseek,
};

/* ---- init / exit -------------------------------------------------------- */

static int __init wmt_init(void)
{
	int slots, err;

	err = as_init(fsr_info_fn7);
	if (err)
		return err;

	dd_init(&dd);

	if (fifo_recs < 4096)
		fifo_recs = 4096;
	slots = 1 << (fls(fifo_recs) - 1);
	fifo_buf = vmalloc((size_t)slots * sizeof(struct wl_mmio_rec));
	if (!fifo_buf)
		return -ENOMEM;
	wl_ring_init(&ring, fifo_buf, slots, sizeof(struct wl_mmio_rec));

	if (!proc_create(PROC_NAME, 0440, NULL, &proc_fops)) {
		vfree(fifo_buf);
		fifo_buf = NULL;
		return -ENOMEM;
	}

	/* Take the dispatch slot and prove the mechanism on our own page
	 * before wl's window is touched. */
	take_slot();
	if (!selftest_run()) {
		give_slot_back();
		remove_proc_entry(PROC_NAME, NULL);
		vfree(fifo_buf);
		fifo_buf = NULL;
		return -EIO;
	}
	/* The self-test's emulated access left one record in the ring;
	 * start the real trace from empty. */
	wl_ring_init(&ring, fifo_buf, slots, sizeof(struct wl_mmio_rec));
	atomic_set(&rec_seq, 0);
	atomic_set(&n_emulated, 0);
	atomic_set(&n_dd, 0);

	err = arm_window();
	if (err) {
		/* Unlike the MIPS build's ioremap watch, this version does
		 * not sit on wl's ioremap call: load it once wl is up. */
		pr_warn("wl_mmio_trap: window not armed yet (%d); load after wl is up, or pass win_phys=\n",
			err);
		give_slot_back();
		remove_proc_entry(PROC_NAME, NULL);
		vfree(fifo_buf);
		fifo_buf = NULL;
		return err;
	}
	return 0;
}

static void __exit wmt_exit(void)
{
	region_open();
	disarm_window();
	flush_work(&overflow_work);
	remove_proc_entry(PROC_NAME, NULL);
	vfree(fifo_buf);
	fifo_buf = NULL;
	pr_info("wl_mmio_trap: unloaded (emulated %d, dd %d, foreign %d, drops %d)\n",
		atomic_read(&n_emulated), atomic_read(&n_dd),
		atomic_read(&n_foreign), atomic_read(&drops));
}

module_init(wmt_init);
module_exit(wmt_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Page-fault MMIO tracer for Broadcom wl, ARM");
