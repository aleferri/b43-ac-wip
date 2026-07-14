// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver AC-PHY low-level helpers.
 *
 * Isolati in un file a parte da phy_ac.c per due motivi:
 *   1. Sono le uniche primitive AC-PHY che non stanno nel PHY-table path
 *      (tables_phy_ac.c) né nel radio-specific path (radio_2069.c) — cioè
 *      helper "trasversali" al MAC/PHY boundary.
 *   2. Nel test env userspace il --wrap del linker intercetta le chiamate
 *      inter-object; se la definizione fosse nello stesso .o del chiamante,
 *      la risoluzione locale bypasserebbe il wrap e il tracer perderebbe
 *      le op MAC.MHF / MAC.MCTRL.
 */

#include "b43.h"
#include "phy_ac.h"
#include "main.h"

/*
 * SHM r/m/w sui 5 slot HOSTFn. Il vendor AC-PHY tocca 5 slot indipendenti
 * (0=HOSTF1..4=HOSTF5); b43_hf_write mainline copre solo i 3 bassi come
 * un u64 unico, che non ha la semantica maskset atomica richiesta qui.
 *
 * Il layout HOSTFn nel kernel è sparso in offset non contigui (HOSTF4 a
 * +0x18 da HOSTF3, HOSTF5 a +0x5c da HOSTF4), quindi una lookup table
 * risolve lo slot al registro.
 *
 * slot: 0..4
 * mask: bit da preservare del vecchio valore
 * val:  bit da imporre (già mascherati a ~mask dal chiamante)
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
 * r/m/w su B43_MMIO_MACCTL. Il core b43 mainline non esporta questo helper
 * — il pattern equivalente è b43_maskset32 inlineato per ogni call site.
 * Qui è isolato per leggibilità del setup AC-PHY e per essere wrappabile
 * dal tracer di test.
 */
void b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set)
{
	b43_maskset32(dev, B43_MMIO_MACCTL, mask, set);
}
