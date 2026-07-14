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
 * Variante di write_bulk usata quando il gate 0x019e è UNLOCKATO all'entrata
 * (per esempio dopo b43_phy_ac_post_noise_shaping_rx_regprog #37944 unlock).
 * Emette peek + relock idempotente + WR TABLE_ID/OFFSET/DATA — cioè +1 op
 * (il relock) rispetto a write_bulk. Nel vendor blob è la stessa funzione
 * di write_bulk che rileva runtime lo stato del gate; qui esplicitiamo la
 * variante perché il tracer non simula il vero registro. */
void b43_actab_write_bulk_reopen(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data);
/*
 * Variante "auto-contained": emette peek 019e + relock + WR TABLE_ID/OFFSET/
 * DATA + unlock finale. È il pattern osservato nella fase B4 (vendor
 * #41503+): ogni TBL.WR ha il proprio scope gate, senza affidamento su un
 * lock esterno mantenuto per più operazioni. +1 op (unlock finale) rispetto
 * a write_bulk_reopen.
 */
void b43_actab_write_bulk_scoped(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data);
void b43_actab_read_bulk(struct b43_wldev *dev,
			 u16 id, u16 offset, u8 width,
			 size_t len, void *data);

/*
 * Some tables (e.g. id 0x11) are written one cell at a time through the
 * alternate data register 0x0011 -- not DATA_LO/DATA_HI -- re-selecting
 * id+offset per word. This helper reproduces that access.
 */
void b43_actab_write_r11(struct b43_wldev *dev,
			 u16 id, u16 offset, size_t len, const u16 *data);

/*
 * Save+set / restore of bit 0x0002 of B43_PHY_AC_REG_TBL_WRITE_GATE (PHY reg
 * 0x19E).
 */
u16  b43_phy_ac_tbl_write_lock(struct b43_wldev *dev);
void b43_phy_ac_tbl_write_unlock(struct b43_wldev *dev, u16 saved);

#endif /* B43_TABLES_PHY_AC_H_ */
