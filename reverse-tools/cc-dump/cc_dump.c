// SPDX-License-Identifier: GPL-2.0
/*
 * cc_dump - dump BCM43xx ChipCommon PMU state from a running device.
 *
 * Loaded on the D6220 (stock kernel-3.4-rt firmware) while the vendor `wl`
 * driver has already initialised the AC radio. It ioremaps the chip's
 * ChipCommon core and prints the PMU resource masks and related state, so the
 * vendor-initialised max_res_mask/min_res_mask can be compared against what our
 * b43+bcma port programs (see the "PMU res mask pre-write" line emitted by
 * bcma_pmu_resources_init(), patch 0007).
 *
 * No bcma/ssb API is used: the stock firmware manages the chip through the
 * proprietary wl stack, so this reads registers by raw ioremap instead.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/version.h>
#include <linux/swab.h>

/* pr_warn è un alias di pr_warning aggiunto in 2.6.35; il DSL-3580L (2.6.30)
 * ha solo pr_warning. #ifndef così un backport che lo definisce già vince. */
#ifndef pr_warn
#define pr_warn pr_warning
#endif

/* ChipCommon register offsets (include/linux/bcma/bcma_driver_chipcommon.h) */
#define CC_CHIPID		0x0000
#define CC_CHIPSTATUS		0x002C
#define CC_PMU_CTL		0x0600
#define CC_PMU_CAP		0x0604
#define CC_PMU_STAT		0x0608
#define CC_PMU_RES_STATE	0x060C
#define CC_PMU_RES_PENDING	0x0610
#define CC_PMU_TIMER		0x0614
#define CC_PMU_MINRES_MSK	0x0618
#define CC_PMU_MAXRES_MSK	0x061C
#define CC_PMU_RES_TABSEL	0x0620
#define CC_PMU_RES_DEPMSK	0x0624
#define CC_PMU_REGCTL_ADDR	0x0658
#define CC_PMU_REGCTL_DATA	0x065C
#define CC_PMU_PLLCTL_ADDR	0x0660
#define CC_PMU_PLLCTL_DATA	0x0664

static unsigned long base = 0x18000000UL;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base,
	"phys addr of the target ChipCommon core (default SI enum 0x18000000; override for a PCIe BAR / other mapping)");

static unsigned long len = 0x1000UL;
module_param(len, ulong, 0444);
MODULE_PARM_DESC(len, "ioremap length (default 0x1000)");

static int bswap;
module_param(bswap, int, 0444);
MODULE_PARM_DESC(bswap,
	"byte-swap register accesses (set 1 on a big-endian host if chipid reads back garbled)");

static int indirect;
module_param(indirect, int, 0444);
MODULE_PARM_DESC(indirect,
	"also walk res-dep/regctl/pllctl indirect tables; writes the *_ADDR select ports, so only use when the chip is idle");

static int nres = 16;
module_param(nres, int, 0444);
static int nreg = 8;
module_param(nreg, int, 0444);
static int npll = 16;
module_param(npll, int, 0444);

static void __iomem *cc;

static inline u32 rd(u32 off)
{
	u32 v = readl(cc + off);

	return bswap ? swab32(v) : v;
}

static inline void wr(u32 off, u32 v)
{
	writel(bswap ? swab32(v) : v, cc + off);
}

static int __init ccd_init(void)
{
	int i;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0)
	cc = ioremap_nocache(base, len);
#else
	cc = ioremap(base, len);
#endif
	if (!cc) {
		pr_err("cc-dump: ioremap(0x%lx, 0x%lx) failed\n", base, len);
		return -ENOMEM;
	}

	pr_info("cc-dump: ChipCommon @ phys 0x%lx (bswap=%d)\n", base, bswap);
	pr_info("cc-dump:   chipid        0x%08x\n", rd(CC_CHIPID));
	pr_info("cc-dump:   chipstatus    0x%08x\n", rd(CC_CHIPSTATUS));
	pr_info("cc-dump:   pmu_ctl       0x%08x\n", rd(CC_PMU_CTL));
	pr_info("cc-dump:   pmu_cap       0x%08x\n", rd(CC_PMU_CAP));
	pr_info("cc-dump:   pmu_stat      0x%08x\n", rd(CC_PMU_STAT));
	pr_info("cc-dump:   res_state     0x%08x\n", rd(CC_PMU_RES_STATE));
	pr_info("cc-dump:   res_pending   0x%08x\n", rd(CC_PMU_RES_PENDING));
	pr_info("cc-dump:   min_res_mask  0x%08x\n", rd(CC_PMU_MINRES_MSK));
	pr_info("cc-dump:   max_res_mask  0x%08x   <-- compare vs driver's 0x1ff\n",
		rd(CC_PMU_MAXRES_MSK));

	if (indirect) {
		pr_warn("cc-dump: indirect walk writes the *_ADDR select ports; racy if wl is live\n");
		for (i = 0; i < nres; i++) {
			wr(CC_PMU_RES_TABSEL, i);
			(void)rd(CC_PMU_RES_TABSEL);	/* posting read */
			pr_info("cc-dump:   res[%2d] depmsk 0x%08x\n",
				i, rd(CC_PMU_RES_DEPMSK));
		}
		for (i = 0; i < nreg; i++) {
			wr(CC_PMU_REGCTL_ADDR, i);
			(void)rd(CC_PMU_REGCTL_ADDR);
			pr_info("cc-dump:   regctl[%d]     0x%08x\n",
				i, rd(CC_PMU_REGCTL_DATA));
		}
		for (i = 0; i < npll; i++) {
			wr(CC_PMU_PLLCTL_ADDR, i);
			(void)rd(CC_PMU_PLLCTL_ADDR);
			pr_info("cc-dump:   pllctl[%2d]    0x%08x\n",
				i, rd(CC_PMU_PLLCTL_DATA));
		}
	}
	return 0;
}

static void __exit ccd_exit(void)
{
	if (cc)
		iounmap(cc);
}

module_init(ccd_init);
module_exit(ccd_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("b43-add-bcm43xx-ac");
MODULE_DESCRIPTION("Dump BCM43xx ChipCommon PMU resource masks/state from a running device");
