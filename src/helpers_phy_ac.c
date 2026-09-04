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
 * Update one of the five HOSTFn slots.
 *
 * The word is kept in dev->phy.ac->mhfs and the cell is written only when the
 * update changes it, which is what the stock driver does; see the comment on
 * the field. The cell is never read back.
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
	struct b43_phy_ac *ac = dev->phy.ac;
	u16 old;

	if (WARN_ON(slot > 4))
		return;

	old = ac->mhfs[slot];
	ac->mhfs[slot] = (old & mask) | val;

	if (ac->mhf_writethrough && ac->mhfs[slot] != old)
		b43_shm_write16(dev, B43_SHM_SHARED,
				b43_phy_ac_hostf_regs[slot], ac->mhfs[slot]);
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
