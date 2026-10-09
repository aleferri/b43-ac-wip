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

/* Write one HOSTFn slot as it stands, changed or not. */
void b43_phy_ac_mhf_write(struct b43_wldev *dev, u16 slot)
{
	if (WARN_ON(slot > 4))
		return;

	b43_shm_write16(dev, B43_SHM_SHARED, b43_phy_ac_hostf_regs[slot],
			dev->phy.ac->mhfs[slot]);
}

/*
 * Force the PHY clock on or release it. The stock driver pairs the core's
 * forced gated clock with the MAC's side of it in PSM_PHY_HDR: set after the
 * IOCTL on the way in, cleared to CLOCK_EN before it on the way out (0x0006
 * and 0x0002 at the bus, 52 times each on the agcombo ch36 attach).
 */
void b43_phy_ac_force_clock(struct b43_wldev *dev, bool force)
{
	if (force) {
		b43_phy_force_clock(dev, true);
		b43_write16(dev, B43_MMIO_PSM_PHY_HDR,
			    B43_PSM_HDR_MAC_PHY_CLOCK_EN |
			    B43_PSM_HDR_MAC_PHY_FORCE_CLK);
		return;
	}
	b43_write16(dev, B43_MMIO_PSM_PHY_HDR, B43_PSM_HDR_MAC_PHY_CLOCK_EN);
	b43_phy_force_clock(dev, false);
}

/*
 * The MAC's operating width, @bw the chanspec's width field (B43_MAC_BW_*).
 * At the bus the stock driver reads MACCONTROL, clears PHY0 (0x3e6), sets
 * the PHY bandwidth clock of the core's IOCTL and reads MACCONTROL again.
 */
void b43_mac_bw_set(struct b43_wldev *dev, u32 bw)
{
	u32 clk = bw == B43_MAC_BW_80 ? B43_BCMA_IOCTL_PHY_BW_80MHZ :
		  bw == B43_MAC_BW_40 ? B43_BCMA_IOCTL_PHY_BW_40MHZ :
					B43_BCMA_IOCTL_PHY_BW_20MHZ;

	b43_read32(dev, B43_MMIO_MACCTL);
	b43_write16(dev, B43_MMIO_PHY0, 0);
	b43_phy_bw_clk_set(dev, clk);
	b43_read32(dev, B43_MMIO_MACCTL);
}
