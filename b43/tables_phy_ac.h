/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B43_TABLES_PHY_AC_H_
#define B43_TABLES_PHY_AC_H_

#include <linux/types.h>

struct b43_wldev;

/* Load the AC-PHY init tables into the PHY's table memory. */
void b43_phy_ac_tables_init(struct b43_wldev *dev);

/* Bulk write into one of the AC-PHY's internal tables. */
void b43_actab_write_bulk(struct b43_wldev *dev,
			  u16 id, u16 offset, u8 width,
			  size_t len, const void *data);
/*
 * write_bulk with an implicit all-zero source: emits the same op sequence
 * (gate peek, WR TABLE_ID/OFFSET, len cells) without making the caller
 * keep a buffer of zeroes around.
 */
void b43_actab_zerofill(struct b43_wldev *dev,
			u16 id, u16 offset, u8 width, size_t len);
/*
 * Variants for callers that already hold the 0x019e gate locked across a
 * run of bulk writes: they do not re-read it before each access.
 */
void b43_actab_write_bulk_locked(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data);
void b43_actab_zerofill_locked(struct b43_wldev *dev,
			       u16 id, u16 offset, u8 width, size_t len);
/*
 * write_bulk for callers that enter with the 0x019e gate unlocked, as
 * b43_phy_ac_post_noise_shaping_rx_regprog() leaves it. Emits peek +
 * idempotent relock + WR TABLE_ID/OFFSET/DATA, so one op more than
 * write_bulk. The vendor blob has a single write_bulk that senses the gate
 * state at runtime; here the callers pick the variant.
 */
void b43_actab_write_bulk_reopen(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data);
/*
 * Self-contained variant: peek 0x019e + relock + WR TABLE_ID/OFFSET/DATA +
 * closing unlock, so one op more than write_bulk_reopen. Each table write
 * carries its own gate scope instead of relying on a lock the caller holds
 * across several of them.
 */
void b43_actab_write_bulk_scoped(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data);
void b43_actab_read_bulk(struct b43_wldev *dev,
			 u16 id, u16 offset, u8 width,
			 size_t len, void *data);

/*
 * Save+set / restore of bit 0x0002 of B43_PHY_AC_REG_TBL_WRITE_GATE (PHY reg
 * 0x19E).
 */
u16  b43_phy_ac_tbl_write_lock(struct b43_wldev *dev);
void b43_phy_ac_tbl_write_unlock(struct b43_wldev *dev, u16 saved);

#endif /* B43_TABLES_PHY_AC_H_ */
