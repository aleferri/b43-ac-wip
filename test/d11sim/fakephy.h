/*
 * fakephy.h - a fictional PHY/radio behind the core's PHY and radio ports.
 *
 * nexmon-style: there is no PHY model, only a register file that remembers
 * what was written and hands it back on read. Unwritten registers read as a
 * configurable default. The point is to let code that pokes the PHY during
 * bring-up run to completion without a silicon model, while keeping the PHY
 * strictly outside the core state the equivalence check looks at.
 *
 * When replaying a bus capture, reads carry the value the real PHY returned;
 * fakephy_observe() seeds the shadow with it so a later read is consistent and
 * so divergences between what the driver wrote and what it read back can be
 * counted, without any of it reaching core RAM.
 */
#ifndef FAKEPHY_H
#define FAKEPHY_H

#include <stdint.h>

#define FAKEPHY_PHY_CELLS  0x2000
#define FAKEPHY_RAD_CELLS  0x1000

struct fakephy {
	uint16_t phy[FAKEPHY_PHY_CELLS];
	uint16_t rad[FAKEPHY_RAD_CELLS];
	uint8_t  phy_seen[FAKEPHY_PHY_CELLS];
	uint8_t  rad_seen[FAKEPHY_RAD_CELLS];
	uint16_t phy_default, rad_default;

	unsigned long phy_wr, phy_rd, rad_wr, rad_rd;
	unsigned long phy_readback_diverge;
};

void fakephy_init(struct fakephy *p, uint16_t phy_default, uint16_t rad_default);

void fakephy_phy_write(struct fakephy *p, uint16_t addr, uint16_t val);
uint16_t fakephy_phy_read(struct fakephy *p, uint16_t addr);
void fakephy_rad_write(struct fakephy *p, uint16_t addr, uint16_t val);
uint16_t fakephy_rad_read(struct fakephy *p, uint16_t addr);

/* Record the value the real hardware returned for a PHY read, for consistency
 * seeding and read-back divergence stats during capture replay. */
void fakephy_observe_phy_read(struct fakephy *p, uint16_t addr, uint16_t val);

#endif /* FAKEPHY_H */
