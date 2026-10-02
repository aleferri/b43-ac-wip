#include "fakephy.h"
#include <string.h>

void fakephy_init(struct fakephy *p, uint16_t phy_default, uint16_t rad_default)
{
	memset(p, 0, sizeof(*p));
	p->phy_default = phy_default;
	p->rad_default = rad_default;
}

void fakephy_phy_write(struct fakephy *p, uint16_t addr, uint16_t val)
{
	if (addr >= FAKEPHY_PHY_CELLS)
		return;
	p->phy[addr] = val;
	p->phy_seen[addr] = 1;
	p->phy_wr++;
}

uint16_t fakephy_phy_read(struct fakephy *p, uint16_t addr)
{
	p->phy_rd++;
	if (addr >= FAKEPHY_PHY_CELLS)
		return p->phy_default;
	return p->phy_seen[addr] ? p->phy[addr] : p->phy_default;
}

void fakephy_rad_write(struct fakephy *p, uint16_t addr, uint16_t val)
{
	if (addr >= FAKEPHY_RAD_CELLS)
		return;
	p->rad[addr] = val;
	p->rad_seen[addr] = 1;
	p->rad_wr++;
}

uint16_t fakephy_rad_read(struct fakephy *p, uint16_t addr)
{
	p->rad_rd++;
	if (addr >= FAKEPHY_RAD_CELLS)
		return p->rad_default;
	return p->rad_seen[addr] ? p->rad[addr] : p->rad_default;
}

void fakephy_observe_phy_read(struct fakephy *p, uint16_t addr, uint16_t val)
{
	if (addr >= FAKEPHY_PHY_CELLS)
		return;
	if (p->phy_seen[addr] && p->phy[addr] != val)
		p->phy_readback_diverge++;
	p->phy[addr] = val;
	p->phy_seen[addr] = 1;
}
