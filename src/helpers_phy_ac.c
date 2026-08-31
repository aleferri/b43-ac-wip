// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver AC-PHY low-level helpers.
 *
 * Kept out of phy_ac.c for two reasons:
 *   1. These are the only AC-PHY primitives that belong to neither the
 *      PHY-table path (tables_phy_ac.c) nor the radio-specific path
 *      (radio_2069.c); they sit across the MAC/PHY boundary.
 *   2. The userspace trace harness intercepts these symbols with the
 *      linker's --wrap, which only applies to cross-object calls. A
 *      definition in the caller's own object would be resolved locally,
 *      bypass the wrap, and drop the MAC.MHF / MAC.MCTRL ops from the
 *      trace.
 */

#include "b43.h"
#include "phy_ac.h"
#include "main.h"

/*
 * Read/modify/write of one of the five HOSTFn SHM slots.
 *
 * The AC-PHY touches all five slots (0 = HOSTF1 .. 4 = HOSTF5)
 * independently. b43_hf_write() covers only the low three and treats them
 * as a single u64, which does not give the per-slot mask/set semantics
 * needed here.
 *
 * The HOSTFn registers are not contiguous (HOSTF4 is +0x18 from HOSTF3,
 * HOSTF5 is +0x5c from HOSTF4), hence the lookup table.
 *
 * slot: 0..4
 * mask: bits of the old value to preserve
 * val:  bits to force in, already masked to ~mask by the caller
 */
static const u16 b43_phy_ac_hostf_regs[5] = {
	B43_SHM_SH_HOSTF1,
	B43_SHM_SH_HOSTF2,
	B43_SHM_SH_HOSTF3,
	B43_SHM_SH_HOSTF4,
	B43_SHM_SH_HOSTF5,
};

void b43_phy_ac_mhf_maskset(struct b43_wldev *dev, u16 slot, u16 mask, u16 val)
{
	u16 reg, cur;

	if (WARN_ON(slot > 4))
		return;

	reg = b43_phy_ac_hostf_regs[slot];
	cur = b43_shm_read16(dev, B43_SHM_SHARED, reg);
	b43_shm_write16(dev, B43_SHM_SHARED, reg, (cur & mask) | val);
}

/*
 * Read/modify/write of B43_MMIO_MACCTL. The b43 core has no such helper;
 * call sites inline b43_maskset32() instead. Wrapping it here keeps the
 * AC-PHY setup readable and gives the trace harness a symbol to intercept.
 */
void b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set)
{
	b43_maskset32(dev, B43_MMIO_MACCTL, mask, set);
}
