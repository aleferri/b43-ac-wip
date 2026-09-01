// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom B43 wireless driver AC-PHY init tables and the bulk write path that
 * feeds them into the chip.
 */

#include "b43.h"
#include "phy_ac.h"
#include "tables_phy_ac.h"

/* PHY-table-write gate (file-local) */

u16 b43_phy_ac_tbl_write_lock(struct b43_wldev *dev)
{
	u16 saved = b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);

	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_TBL_WRITE_GATE_LOCK,
			B43_PHY_AC_TBL_WRITE_GATE_LOCK);
	return saved;
}

void b43_phy_ac_tbl_write_unlock(struct b43_wldev *dev, u16 saved)
{
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_TBL_WRITE_GATE_LOCK,
			saved & B43_PHY_AC_TBL_WRITE_GATE_LOCK);
}

/* Bulk table write */

/*
 * Shared body of the bulk writes: write a contiguous run of values into one of
 * the PHY's internal tables. @peek decides whether to re-read the 0x019e gate
 * before the id/offset/data sequence. The stock driver re-reads it for every
 * bulk emitted with the gate unlocked, but not when the caller holds it locked
 * across a run -- in a table load there is one peek, at the lock.
 */
static void actab_write_bulk_common(struct b43_wldev *dev,
				    u16 id, u16 offset, u8 width,
				    size_t len, const void *data, bool peek)
{
	size_t i;

	if (peek)
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);

	b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
	b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, offset);

	switch (width) {
	case 8: {
		const u8 *p = data;
		/*
		 * id=0x20 usa un DATA register alternativo (0x011) invece del
		 * default DATA_LO (0x00f). Vedi vendor d6220 ch36 #34564+.
		 */
		u16 data_reg = (id == 0x20)
			? B43_PHY_AC_TABLE_DATA_2
			: B43_PHY_AC_TABLE_DATA_LO;

		for (i = 0; i < len; i++)
			b43_phy_write(dev, data_reg, p[i]);
		break;
	}
	case 16: {
		const u16 *p = data;

		for (i = 0; i < len; i++)
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO, p[i]);
		break;
	}
	case 32: {
		const u32 *p = data;

		for (i = 0; i < len; i++) {
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_HI,
				      (u16)(p[i] >> 16));
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO,
				      (u16)(p[i] & 0xffff));
		}
		break;
	}
	default:
		b43warn(dev->wl,
			"actab_write_bulk: unsupported width %u (id=0x%x len=%zu)\n",
			width, id, len);
		break;
	}
}

/*
 * write_bulk for callers that enter with the 0x019e gate unlocked: emits a peek
 * and an idempotent relock before the id/offset/data writes. In the vendor blob
 * this is decided at runtime; here it is a function of its own.
 */
void b43_actab_write_bulk(struct b43_wldev *dev,
			  u16 id, u16 offset, u8 width,
			  size_t len, const void *data)
{
	actab_write_bulk_common(dev, id, offset, width, len, data, true);
}

void b43_actab_write_bulk_locked(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data)
{
	actab_write_bulk_common(dev, id, offset, width, len, data, false);
}

static void actab_zerofill_common(struct b43_wldev *dev, u16 id, u16 offset,
				  u8 width, size_t len, bool peek)
{
	size_t i;

	/* Stesso prologo di actab_write_bulk_common: vedi la nota su `peek`. */
	if (peek)
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);

	b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
	b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, offset);

	switch (width) {
	case 8: {
		u16 data_reg = (id == 0x20)
			? B43_PHY_AC_TABLE_DATA_2
			: B43_PHY_AC_TABLE_DATA_LO;

		for (i = 0; i < len; i++)
			b43_phy_write(dev, data_reg, 0);
		break;
	}
	case 16:
		for (i = 0; i < len; i++)
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO, 0);
		break;
	case 32:
		for (i = 0; i < len; i++) {
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_HI, 0);
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO, 0);
		}
		break;
	default:
		b43warn(dev->wl,
			"actab_zerofill: unsupported width %u (id=0x%x len=%zu)\n",
			width, id, len);
		break;
	}
}

void b43_actab_zerofill(struct b43_wldev *dev,
			u16 id, u16 offset, u8 width, size_t len)
{
	actab_zerofill_common(dev, id, offset, width, len, true);
}

void b43_actab_zerofill_locked(struct b43_wldev *dev,
			       u16 id, u16 offset, u8 width, size_t len)
{
	actab_zerofill_common(dev, id, offset, width, len, false);
}

void b43_actab_write_bulk_reopen(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data)
{
	size_t i;

	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_TBL_WRITE_GATE_LOCK,
			B43_PHY_AC_TBL_WRITE_GATE_LOCK);

	b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
	b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, offset);

	switch (width) {
	case 16: {
		const u16 *p = data;

		for (i = 0; i < len; i++)
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO, p[i]);
		break;
	}
	default:
		b43warn(dev->wl,
			"actab_write_bulk_reopen: unsupported width %u\n",
			width);
		break;
	}
}

/*
 * Self-contained variant, as used in phase B4: each table write carries its own
 * gate scope -- peek 0x019e and lock on entry, unlock on exit. This is the
 * pattern for callers that do not hold an outer lock across several
 * consecutive operations.
 *
 * Supports width 16, data through DATA_LO, and width 32, data through DATA_HI
 * then DATA_LO, high half first as in write_bulk's 32-bit case.
 */
void b43_actab_write_bulk_scoped(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data)
{
	size_t i;

	b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_TBL_WRITE_GATE_LOCK,
			B43_PHY_AC_TBL_WRITE_GATE_LOCK);

	b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
	b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, offset);

	switch (width) {
	case 16: {
		const u16 *p = data;

		for (i = 0; i < len; i++)
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO, p[i]);
		break;
	}
	case 32: {
		const u32 *p = data;

		for (i = 0; i < len; i++) {
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_HI,
				      (u16)(p[i] >> 16));
			b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_LO,
				      (u16)(p[i] & 0xffff));
		}
		break;
	}
	default:
		b43warn(dev->wl,
			"actab_write_bulk_scoped: unsupported width %u\n",
			width);
		break;
	}

	b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
			(u16)~B43_PHY_AC_TBL_WRITE_GATE_LOCK, 0);
}

void b43_actab_read_bulk(struct b43_wldev *dev,
			 u16 id, u16 offset, u8 width,
			 size_t len, void *data)
{
	size_t i;
	u16 gate;

	/* As in actab_write_bulk(), the 0x019e peek is the saved value for the
	 * restore at the end of the enclosing scope, tx_lpf or similar. */
	gate = b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);

	/*
	 * The vendor's relock -- MOD 0x019e setting bit 1 -- is conditional: it
	 * appears only when the preceding peek came back with the gate unlocked.
	 * The captures show both cases, an unlock followed by a peek of 0 and
	 * then a relock, and a peek that already reads 0x02 followed straight by
	 * the data write with no relock.
	 *
	 * This is specific to actab_read_bulk(); actab_write_bulk() never does
	 * it, matching the vendor, where a table write always follows peek then
	 * data write.
	 */
	if (!(gate & 0x0002))
		b43_phy_maskset(dev, B43_PHY_AC_REG_TBL_WRITE_GATE,
				(u16)~0x0002, 0x0002);

	b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
	b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, offset);

	switch (width) {
	case 8: {
		u8 *p = data;
		/*
		 * id=0x20 usa DATA_2 (0x011) invece di DATA_LO. Vedi actab_write_bulk
		 * per la controparte in scrittura (vendor d6220 ch36 #34564+, #38131).
		 */
		u16 data_reg = (id == 0x20)
			? B43_PHY_AC_TABLE_DATA_2
			: B43_PHY_AC_TABLE_DATA_LO;

		for (i = 0; i < len; i++)
			p[i] = b43_phy_read(dev, data_reg) & 0xff;
		break;
	}
	case 16: {
		u16 *p = data;
		/*
		 * id=0x20 usa DATA_2 (0x011) come per width=8 — verificato
		 * al vendor d6220 #46085/#46092 (TBL.RD id=0x0020 off=0x14/0x1e
		 * emette PHY.RD addr=0x0011).
		 */
		u16 data_reg = (id == 0x20)
			? B43_PHY_AC_TABLE_DATA_2
			: B43_PHY_AC_TABLE_DATA_LO;

		for (i = 0; i < len; i++)
			p[i] = b43_phy_read(dev, data_reg);
		break;
	}
	case 32: {
		u32 *p = data;

		for (i = 0; i < len; i++) {
			u16 lo = b43_phy_read(dev, B43_PHY_AC_TABLE_DATA_LO);
			u16 hi = b43_phy_read(dev, B43_PHY_AC_TABLE_DATA_HI);

			p[i] = ((u32)hi << 16) | lo;
		}
		break;
	}
	case 48: {
		u16 *p = data;	/* three halfwords per entry, low first */

		for (i = 0; i < len; i++) {
			p[i * 3 + 0] = b43_phy_read(dev, B43_PHY_AC_TABLE_DATA_2);
			p[i * 3 + 1] = b43_phy_read(dev, B43_PHY_AC_TABLE_DATA_2);
			p[i * 3 + 2] = b43_phy_read(dev, B43_PHY_AC_TABLE_DATA_2);
		}
		break;
	}
	default:
		b43warn(dev->wl,
			"actab_read_bulk: unsupported width %u (id=0x%x len=%zu)\n",
			width, id, len);
		break;
	}
}

/* Table descriptors and data */

/*
 * Sentinel for descriptors that are not per-core (one instance for the whole
 * chip).
 */
#define TBL_SHARED		0xff

struct b43_phy_ac_table_desc {
	u16 id;
	u16 offset;
	u8 width;
	u8 core;		/* 0..num_cores-1, or TBL_SHARED */
	size_t len;
	const void *data;	/* NULL when zero is set */
	const char *name;	/* for dmesg on skip */
	bool zero;		/* fill len cells with 0 instead of copying data */
	bool first_only;	/* written on a first bring-up only, see below */
};

/*
 * The rev-0 tables follow, one static const u{8,16,32} array per descriptor
 * entry in b43_phy_ac_tables_rev0[] (declared further down).
 *
 * est_pwr and papd_comp_rfpwr share id and offset by design: the vendor writes
 * est_pwr first and papd_comp_rfpwr over it later in the same init sequence
 * (agcombo attach #1345 and #2899). Do not "fix" the duplicate.
 *
 * sqthreshold (id 0x06) is not here: the vendor never writes it. The only
 * captures containing the init table load are the two agcombo ones, and in
 * neither does TABLE_ID take 0x06, nor does the payload appear at any other
 * id. The blob has a descriptor for it, so it is presumably written under a
 * condition none of the captures exercise -- 2.4 GHz is the obvious
 * candidate, and is out of scope here.
 */

static const u8 acphy_tx_evm_tbl_rev0[38] = {
	0x09, 0x0e, 0x11, 0x14, 0x17, 0x1a, 0x1d, 0x20,
	0x09, 0x0e, 0x11, 0x14, 0x17, 0x1a, 0x1d, 0x20,
	0x22, 0x24, 0x09, 0x0e, 0x11, 0x14, 0x17, 0x1a,
	0x1d, 0x20, 0x22, 0x24, 0x09, 0x0e, 0x11, 0x14,
	0x17, 0x1a, 0x1d, 0x20, 0x22, 0x24,
};

static const u16 acphy_mcs_tbl_rev0[128] = {
	0x0000, 0x0008, 0x000a, 0x0010, 0x0012, 0x0019, 0x001a, 0x001c,
	0x0080, 0x0088, 0x008a, 0x0090, 0x0092, 0x0099, 0x009a, 0x009c,
	0x0100, 0x0108, 0x010a, 0x0110, 0x0112, 0x0119, 0x011a, 0x011c,
	0x0180, 0x0188, 0x018a, 0x0190, 0x0192, 0x0199, 0x019a, 0x019c,
	0x0000, 0x0098, 0x00a0, 0x00a8, 0x009a, 0x00a2, 0x00aa, 0x0120,
	0x0128, 0x0128, 0x0130, 0x0138, 0x0138, 0x0140, 0x0122, 0x012a,
	0x012a, 0x0132, 0x013a, 0x013a, 0x0142, 0x01a8, 0x01b0, 0x01b8,
	0x01b0, 0x01b8, 0x01c0, 0x01c8, 0x01c0, 0x01c8, 0x01d0, 0x01d0,
	0x01d8, 0x01aa, 0x01b2, 0x01ba, 0x01b2, 0x01ba, 0x01c2, 0x01ca,
	0x01c2, 0x01ca, 0x01d2, 0x01d2, 0x01da, 0x0001, 0x0002, 0x0004,
	0x0009, 0x000c, 0x0011, 0x0014, 0x0018, 0x0020, 0x0021, 0x0022,
	0x0024, 0x0081, 0x0082, 0x0084, 0x0089, 0x008c, 0x0091, 0x0094,
	0x0098, 0x00a0, 0x00a1, 0x00a2, 0x00a4, 0x0007, 0x0007, 0x0007,
	0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007,
	0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007,
	0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007,
};

static const u32 phasetrack_tbl_rev0[22] = {
	0x00035700, 0x0002cc9a, 0x00026666, 0x0001581f, 0x0001581f, 0x0001581f,
	0x0001581f, 0x0001581f, 0x0001581f, 0x0001581f, 0x0001581f, 0x00035700,
	0x0002cc9a, 0x00026666, 0x0001581f, 0x0001581f, 0x0001581f, 0x0001581f,
	0x0001581f, 0x0001581f, 0x0001581f, 0x0001581f,
};

static const u16 est_pwr_lut_core0_rev0[128] = {
	0x554a, 0x554a, 0x544a, 0x544a, 0x5449, 0x5449, 0x5349, 0x5349, 0x5248, 0x5249, 0x5248, 0x5248,
	0x5147, 0x5147, 0x5147, 0x5147, 0x5046, 0x5046, 0x4f46, 0x4f46, 0x4f45, 0x4f45, 0x4e44, 0x4e44,
	0x4e44, 0x4e44, 0x4d43, 0x4d43, 0x4c43, 0x4c43, 0x4c42, 0x4c42, 0x4b42, 0x4b42, 0x4a41, 0x4a41,
	0x4940, 0x4940, 0x4940, 0x4940, 0x483f, 0x483f, 0x473e, 0x473e, 0x463e, 0x463e, 0x463d, 0x463d,
	0x453c, 0x453c, 0x443b, 0x443b, 0x433b, 0x433b, 0x423a, 0x423a, 0x4139, 0x4139, 0x4038, 0x4038,
	0x4037, 0x4037, 0x3f36, 0x3f36, 0x3e36, 0x3e36, 0x3d35, 0x3d35, 0x3c34, 0x3b34, 0x3a33, 0x3a33,
	0x3932, 0x3932, 0x3831, 0x3830, 0x372f, 0x372f, 0x362e, 0x362e, 0x352d, 0x342d, 0x332c, 0x332c,
	0x322b, 0x322a, 0x3129, 0x3029, 0x2f28, 0x2f27, 0x2e26, 0x2d26, 0x2c25, 0x2c24, 0x2b23, 0x2a23,
	0x2922, 0x2821, 0x2720, 0x261f, 0x251e, 0x241d, 0x231c, 0x221b, 0x211a, 0x2019, 0x1f17, 0x1e16,
	0x1d15, 0x1c14, 0x1a12, 0x1911, 0x180f, 0x170e, 0x150c, 0x140a, 0x1208, 0x1006, 0x0e03, 0x0d01,
	0x0bfe, 0x09fb, 0x07f8, 0x05f5, 0x02f1, 0x00ec, 0xfde7, 0xfde7,
};

static const u16 est_pwr_lut_core1_rev0[128] = {
	0x554a, 0x554a, 0x544a, 0x544a, 0x5449, 0x5449, 0x5349, 0x5349, 0x5248, 0x5249, 0x5248, 0x5248,
	0x5147, 0x5147, 0x5147, 0x5147, 0x5046, 0x5046, 0x4f46, 0x4f46, 0x4f45, 0x4f45, 0x4e44, 0x4e44,
	0x4e44, 0x4e44, 0x4d43, 0x4d43, 0x4c43, 0x4c43, 0x4c42, 0x4c42, 0x4b42, 0x4b42, 0x4a41, 0x4a41,
	0x4940, 0x4940, 0x4940, 0x4940, 0x483f, 0x483f, 0x473e, 0x473e, 0x463e, 0x463e, 0x463d, 0x463d,
	0x453c, 0x453c, 0x443b, 0x443b, 0x433b, 0x433b, 0x423a, 0x423a, 0x4139, 0x4139, 0x4038, 0x4038,
	0x4037, 0x4037, 0x3f36, 0x3f36, 0x3e36, 0x3e36, 0x3d35, 0x3d35, 0x3c34, 0x3b34, 0x3a33, 0x3a33,
	0x3932, 0x3932, 0x3831, 0x3830, 0x372f, 0x372f, 0x362e, 0x362e, 0x352d, 0x342d, 0x332c, 0x332c,
	0x322b, 0x322a, 0x3129, 0x3029, 0x2f28, 0x2f27, 0x2e26, 0x2d26, 0x2c25, 0x2c24, 0x2b23, 0x2a23,
	0x2922, 0x2821, 0x2720, 0x261f, 0x251e, 0x241d, 0x231c, 0x221b, 0x211a, 0x2019, 0x1f17, 0x1e16,
	0x1d15, 0x1c14, 0x1a12, 0x1911, 0x180f, 0x170e, 0x150c, 0x140a, 0x1208, 0x1006, 0x0e03, 0x0d01,
	0x0bfe, 0x09fb, 0x07f8, 0x05f5, 0x02f1, 0x00ec, 0xfde7, 0xfde7,
};

static const u16 est_pwr_lut_core2_rev0[128] = {
	0x554a, 0x554a, 0x544a, 0x544a, 0x5449, 0x5449, 0x5349, 0x5349, 0x5248, 0x5249, 0x5248, 0x5248,
	0x5147, 0x5147, 0x5147, 0x5147, 0x5046, 0x5046, 0x4f46, 0x4f46, 0x4f45, 0x4f45, 0x4e44, 0x4e44,
	0x4e44, 0x4e44, 0x4d43, 0x4d43, 0x4c43, 0x4c43, 0x4c42, 0x4c42, 0x4b42, 0x4b42, 0x4a41, 0x4a41,
	0x4940, 0x4940, 0x4940, 0x4940, 0x483f, 0x483f, 0x473e, 0x473e, 0x463e, 0x463e, 0x463d, 0x463d,
	0x453c, 0x453c, 0x443b, 0x443b, 0x433b, 0x433b, 0x423a, 0x423a, 0x4139, 0x4139, 0x4038, 0x4038,
	0x4037, 0x4037, 0x3f36, 0x3f36, 0x3e36, 0x3e36, 0x3d35, 0x3d35, 0x3c34, 0x3b34, 0x3a33, 0x3a33,
	0x3932, 0x3932, 0x3831, 0x3830, 0x372f, 0x372f, 0x362e, 0x362e, 0x352d, 0x342d, 0x332c, 0x332c,
	0x322b, 0x322a, 0x3129, 0x3029, 0x2f28, 0x2f27, 0x2e26, 0x2d26, 0x2c25, 0x2c24, 0x2b23, 0x2a23,
	0x2922, 0x2821, 0x2720, 0x261f, 0x251e, 0x241d, 0x231c, 0x221b, 0x211a, 0x2019, 0x1f17, 0x1e16,
	0x1d15, 0x1c14, 0x1a12, 0x1911, 0x180f, 0x170e, 0x150c, 0x140a, 0x1208, 0x1006, 0x0e03, 0x0d01,
	0x0bfe, 0x09fb, 0x07f8, 0x05f5, 0x02f1, 0x00ec, 0xfde7, 0xfde7,
};

static const u16 papd_comp_rfpwr_tbl_core0_rev0[128] = {
	0x0048, 0x0047, 0x0046, 0x0044, 0x0043, 0x0042, 0x0040, 0x003f, 0x003e, 0x003c, 0x003a, 0x0038,
	0x0037, 0x0035, 0x0033, 0x0031, 0x002e, 0x002c, 0x002a, 0x0027, 0x0024, 0x0020, 0x001d, 0x0019,
	0x0019, 0x0019, 0x0019, 0x0014, 0x0014, 0x0014, 0x000f, 0x000f, 0x000f, 0x0008, 0x0008, 0x0008,
	0x0008, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0xfff7, 0xfff7, 0xfff7, 0xfff7, 0xfff7, 0xfff7,
	0xfff7, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8,
	0xffe8, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
};

static const u16 papd_comp_rfpwr_tbl_core1_rev0[128] = {
	0x0048, 0x0047, 0x0046, 0x0044, 0x0043, 0x0042, 0x0040, 0x003f, 0x003e, 0x003c, 0x003a, 0x0038,
	0x0037, 0x0035, 0x0033, 0x0031, 0x002e, 0x002c, 0x002a, 0x0027, 0x0024, 0x0020, 0x001d, 0x0019,
	0x0019, 0x0019, 0x0019, 0x0014, 0x0014, 0x0014, 0x000f, 0x000f, 0x000f, 0x0008, 0x0008, 0x0008,
	0x0008, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0xfff7, 0xfff7, 0xfff7, 0xfff7, 0xfff7, 0xfff7,
	0xfff7, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8,
	0xffe8, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
};

static const u16 papd_comp_rfpwr_tbl_core2_rev0[128] = {
	0x0048, 0x0047, 0x0046, 0x0044, 0x0043, 0x0042, 0x0040, 0x003f, 0x003e, 0x003c, 0x003a, 0x0038,
	0x0037, 0x0035, 0x0033, 0x0031, 0x002e, 0x002c, 0x002a, 0x0027, 0x0024, 0x0020, 0x001d, 0x0019,
	0x0019, 0x0019, 0x0019, 0x0014, 0x0014, 0x0014, 0x000f, 0x000f, 0x000f, 0x0008, 0x0008, 0x0008,
	0x0008, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0xfff7, 0xfff7, 0xfff7, 0xfff7, 0xfff7, 0xfff7,
	0xfff7, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8, 0xffe8,
	0xffe8, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
	0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0, 0xffd0,
};

static const u32 papd_comp_epsilon_tbl_core0_rev0[64] = {
	0x00000000, 0x03fdfffe, 0x00002003, 0x03fe4001, 0x03ff000a, 0x03fea003,
	0x03fe7fff, 0x03fe9fff, 0x03fee00a, 0x00000029, 0x0001403c, 0x0002004b,
	0x0003a04b, 0x0004a04e, 0x0005e051, 0x0007004a, 0x0008003f, 0x00092034,
	0x000aa02f, 0x000ba024, 0x000d2022, 0x000e8021, 0x0010001e, 0x0011e01c,
	0x0013c018, 0x00152014, 0x00172010, 0x0019200a, 0x001a7ff9, 0x001bffe9,
	0x001d7fd9, 0x001f1fc9, 0x0020bfb9, 0x0021ffa7, 0x00233f95, 0x00243f84,
	0x00257f73, 0x00269f63, 0x00283f57, 0x002a1f53, 0x002bff4c, 0x002e3f4a,
	0x002fbf3e, 0x0030ff2f, 0x00321f21, 0x00339f12, 0x0034df02, 0x00359eee,
	0x0035ded8, 0x00361ec1, 0x0036beac, 0x00373e96, 0x0037de82, 0x003e5ea3,
	0x004b5ef3, 0x0056bf29, 0x0061bf51, 0x00723fb9, 0x00926203, 0x01308ef5,
	0x0152cfff, 0x01516fff, 0x014f6fff, 0x014e6fff,
};

static const u32 papd_comp_epsilon_tbl_core1_rev0[64] = {
	0x00000000, 0x03fdfffe, 0x00002003, 0x03fe4001, 0x03ff000a, 0x03fea003,
	0x03fe7fff, 0x03fe9fff, 0x03fee00a, 0x00000029, 0x0001403c, 0x0002004b,
	0x0003a04b, 0x0004a04e, 0x0005e051, 0x0007004a, 0x0008003f, 0x00092034,
	0x000aa02f, 0x000ba024, 0x000d2022, 0x000e8021, 0x0010001e, 0x0011e01c,
	0x0013c018, 0x00152014, 0x00172010, 0x0019200a, 0x001a7ff9, 0x001bffe9,
	0x001d7fd9, 0x001f1fc9, 0x0020bfb9, 0x0021ffa7, 0x00233f95, 0x00243f84,
	0x00257f73, 0x00269f63, 0x00283f57, 0x002a1f53, 0x002bff4c, 0x002e3f4a,
	0x002fbf3e, 0x0030ff2f, 0x00321f21, 0x00339f12, 0x0034df02, 0x00359eee,
	0x0035ded8, 0x00361ec1, 0x0036beac, 0x00373e96, 0x0037de82, 0x003e5ea3,
	0x004b5ef3, 0x0056bf29, 0x0061bf51, 0x00723fb9, 0x00926203, 0x01308ef5,
	0x0152cfff, 0x01516fff, 0x014f6fff, 0x014e6fff,
};

static const u32 papd_comp_epsilon_tbl_core2_rev0[64] = {
	0x00000000, 0x03fdfffe, 0x00002003, 0x03fe4001, 0x03ff000a, 0x03fea003,
	0x03fe7fff, 0x03fe9fff, 0x03fee00a, 0x00000029, 0x0001403c, 0x0002004b,
	0x0003a04b, 0x0004a04e, 0x0005e051, 0x0007004a, 0x0008003f, 0x00092034,
	0x000aa02f, 0x000ba024, 0x000d2022, 0x000e8021, 0x0010001e, 0x0011e01c,
	0x0013c018, 0x00152014, 0x00172010, 0x0019200a, 0x001a7ff9, 0x001bffe9,
	0x001d7fd9, 0x001f1fc9, 0x0020bfb9, 0x0021ffa7, 0x00233f95, 0x00243f84,
	0x00257f73, 0x00269f63, 0x00283f57, 0x002a1f53, 0x002bff4c, 0x002e3f4a,
	0x002fbf3e, 0x0030ff2f, 0x00321f21, 0x00339f12, 0x0034df02, 0x00359eee,
	0x0035ded8, 0x00361ec1, 0x0036beac, 0x00373e96, 0x0037de82, 0x003e5ea3,
	0x004b5ef3, 0x0056bf29, 0x0061bf51, 0x00723fb9, 0x00926203, 0x01308ef5,
	0x0152cfff, 0x01516fff, 0x014f6fff, 0x014e6fff,
};

static const u32 papd_cal_scalars_tbl_core0_rev0[64] = {
	0x0b5e002d, 0x0ae2002f, 0x0a3b0032, 0x09a70035, 0x09220038, 0x08ab003b,
	0x081f003f, 0x07a20043, 0x07340047, 0x06d2004b, 0x067a004f, 0x06170054,
	0x05bf0059, 0x0571005e, 0x051e0064, 0x04d3006a, 0x04910070, 0x044c0077,
	0x040f007e, 0x03d90085, 0x03a1008d, 0x036f0095, 0x033d009e, 0x030b00a8,
	0x02e000b2, 0x02b900bc, 0x029200c7, 0x026d00d3, 0x024900e0, 0x022900ed,
	0x020a00fb, 0x01ec010a, 0x01d20119, 0x01b7012a, 0x019e013c, 0x0188014e,
	0x01720162, 0x015d0177, 0x0149018e, 0x013701a5, 0x012601be, 0x011501d8,
	0x010601f4, 0x00f70212, 0x00e90231, 0x00dc0253, 0x00d00276, 0x00c4029b,
	0x00b902c3, 0x00af02ed, 0x00a50319, 0x009c0348, 0x0093037a, 0x008b03af,
	0x008303e6, 0x007c0422, 0x00750460, 0x006e04a3, 0x006804e9, 0x00620533,
	0x005d0582, 0x005805d6, 0x0053062e, 0x004e068c,
};

static const u32 papd_cal_scalars_tbl_core1_rev0[64] = {
	0x0b5e002d, 0x0ae2002f, 0x0a3b0032, 0x09a70035, 0x09220038, 0x08ab003b,
	0x081f003f, 0x07a20043, 0x07340047, 0x06d2004b, 0x067a004f, 0x06170054,
	0x05bf0059, 0x0571005e, 0x051e0064, 0x04d3006a, 0x04910070, 0x044c0077,
	0x040f007e, 0x03d90085, 0x03a1008d, 0x036f0095, 0x033d009e, 0x030b00a8,
	0x02e000b2, 0x02b900bc, 0x029200c7, 0x026d00d3, 0x024900e0, 0x022900ed,
	0x020a00fb, 0x01ec010a, 0x01d20119, 0x01b7012a, 0x019e013c, 0x0188014e,
	0x01720162, 0x015d0177, 0x0149018e, 0x013701a5, 0x012601be, 0x011501d8,
	0x010601f4, 0x00f70212, 0x00e90231, 0x00dc0253, 0x00d00276, 0x00c4029b,
	0x00b902c3, 0x00af02ed, 0x00a50319, 0x009c0348, 0x0093037a, 0x008b03af,
	0x008303e6, 0x007c0422, 0x00750460, 0x006e04a3, 0x006804e9, 0x00620533,
	0x005d0582, 0x005805d6, 0x0053062e, 0x004e068c,
};

static const u32 papd_cal_scalars_tbl_core2_rev0[64] = {
	0x0b5e002d, 0x0ae2002f, 0x0a3b0032, 0x09a70035, 0x09220038, 0x08ab003b,
	0x081f003f, 0x07a20043, 0x07340047, 0x06d2004b, 0x067a004f, 0x06170054,
	0x05bf0059, 0x0571005e, 0x051e0064, 0x04d3006a, 0x04910070, 0x044c0077,
	0x040f007e, 0x03d90085, 0x03a1008d, 0x036f0095, 0x033d009e, 0x030b00a8,
	0x02e000b2, 0x02b900bc, 0x029200c7, 0x026d00d3, 0x024900e0, 0x022900ed,
	0x020a00fb, 0x01ec010a, 0x01d20119, 0x01b7012a, 0x019e013c, 0x0188014e,
	0x01720162, 0x015d0177, 0x0149018e, 0x013701a5, 0x012601be, 0x011501d8,
	0x010601f4, 0x00f70212, 0x00e90231, 0x00dc0253, 0x00d00276, 0x00c4029b,
	0x00b902c3, 0x00af02ed, 0x00a50319, 0x009c0348, 0x0093037a, 0x008b03af,
	0x008303e6, 0x007c0422, 0x00750460, 0x006e04a3, 0x006804e9, 0x00620533,
	0x005d0582, 0x005805d6, 0x0053062e, 0x004e068c,
};

#define TBL_POPULATED(_id, _off, _w, _core, _arr, _name) \
	{ .id = (_id), .offset = (_off), .width = (_w), .core = (_core), \
	  .len = ARRAY_SIZE(_arr), .data = (_arr), .name = (_name) }

#define TBL_ZERO(_id, _off, _w, _core, _len, _name) \
	{ .id = (_id), .offset = (_off), .width = (_w), .core = (_core), \
	  .len = (_len), .data = NULL, .name = (_name), .zero = true }

/*
 * Same two, for the tables the stock driver writes on a first bring-up and
 * never again.
 *
 * The d6220 captures separate cleanly: the attach selects table ids
 * 1, 2, 3, 5, 0x41, 0x47, 0x48 and their per-core mirrors, while a warm cycle
 * -- both the down-to-bss capture and every one of the 32 sweep segments --
 * selects the same reduced set and none of those. So this is a phase
 * distinction, not a board or chip one.
 *
 * It reads as deliberate rather than incidental: the attach-only entries are
 * the PHY constants that never change once loaded (mcs, tx_evm, phasetrack)
 * and the PAPD state -- the epsilon and scalar tables and the 0x41 zero-fill.
 * Rewriting the latter on every channel change would throw away the
 * calibration the device has accumulated, which is also why the 0x42 and 0x62
 * zero-fills are *not* in this set: those the vendor does redo.
 */
#define TBL_POPULATED_FIRST(_id, _off, _w, _core, _arr, _name) \
	{ .id = (_id), .offset = (_off), .width = (_w), .core = (_core), \
	  .len = ARRAY_SIZE(_arr), .data = (_arr), .name = (_name), \
	  .first_only = true }

#define TBL_ZERO_FIRST(_id, _off, _w, _core, _len, _name) \
	{ .id = (_id), .offset = (_off), .width = (_w), .core = (_core), \
	  .len = (_len), .data = NULL, .name = (_name), .zero = true, \
	  .first_only = true }

/*
 * acphytbl_info_rev0 entries, in the order the vendor emits them (agcombo
 * attach, TABLE_ID sequence at #357..#3939). The zero-fills are interleaved
 * here rather than run as a separate pass: the vendor puts 0x04/0x03 between
 * tx_evm and phasetrack, and the per-core ones between est_pwr and
 * papd_comp_rfpwr. Order matters in this block -- est_pwr and papd_comp_rfpwr
 * target the same cells -- so keep the sequence, and note that the per-core
 * groups are id-major, not core-major.
 *
 * The per-core entries are gated on num_cores, NOT on coremask: the d6220 is a
 * 2x2 board (rxchain=3) and its vendor still loads the core-2 instances
 * (0x80/0x81/0x82/0x87/0x88, attach-to-bss-up capture). Same rule as the noise
 * shaping loop in phy_ac.c -- the tables are per channel PHY, not per active
 * chain.
 */
static const struct b43_phy_ac_table_desc b43_phy_ac_tables_rev0[] = {
	TBL_POPULATED_FIRST(0x01, 0, 16, TBL_SHARED, acphy_mcs_tbl_rev0,                "mcs"),
	TBL_POPULATED_FIRST(0x02, 0,  8, TBL_SHARED, acphy_tx_evm_tbl_rev0,             "tx_evm"),
	TBL_ZERO_FIRST(0x04, 0,  8, TBL_SHARED, 256,                               "zero_cal_0x04"),
	TBL_ZERO_FIRST(0x03, 0, 32, TBL_SHARED, 256,                               "zero_cal_0x03"),
	TBL_POPULATED_FIRST(0x05, 0, 32, TBL_SHARED, phasetrack_tbl_rev0,               "phasetrack"),
	TBL_POPULATED(0x40, 0, 16, 0,          est_pwr_lut_core0_rev0,            "est_pwr_lut_core0"),
	TBL_POPULATED(0x60, 0, 16, 1,          est_pwr_lut_core1_rev0,            "est_pwr_lut_core1"),
	TBL_POPULATED(0x80, 0, 16, 2,          est_pwr_lut_core2_rev0,            "est_pwr_lut_core2"),
	TBL_ZERO_FIRST(0x41, 0, 32, 0,          128,                               "zero_cal_core0_0x41"),
	TBL_ZERO_FIRST(0x61, 0, 32, 1,          128,                               "zero_cal_core1_0x61"),
	TBL_ZERO_FIRST(0x81, 0, 32, 2,          128,                               "zero_cal_core2_0x81"),
	TBL_ZERO     (0x42, 0, 16, 0,          128,                               "zero_cal_core0_0x42"),
	TBL_ZERO     (0x62, 0, 16, 1,          128,                               "zero_cal_core1_0x62"),
	TBL_ZERO     (0x82, 0, 16, 2,          128,                               "zero_cal_core2_0x82"),
	TBL_POPULATED(0x40, 0, 16, 0,          papd_comp_rfpwr_tbl_core0_rev0,    "papd_comp_rfpwr_tbl_core0"),
	TBL_POPULATED(0x60, 0, 16, 1,          papd_comp_rfpwr_tbl_core1_rev0,    "papd_comp_rfpwr_tbl_core1"),
	TBL_POPULATED(0x80, 0, 16, 2,          papd_comp_rfpwr_tbl_core2_rev0,    "papd_comp_rfpwr_tbl_core2"),
	TBL_POPULATED_FIRST(0x47, 0, 32, 0,          papd_comp_epsilon_tbl_core0_rev0,  "papd_comp_epsilon_tbl_core0"),
	TBL_POPULATED_FIRST(0x67, 0, 32, 1,          papd_comp_epsilon_tbl_core1_rev0,  "papd_comp_epsilon_tbl_core1"),
	TBL_POPULATED_FIRST(0x87, 0, 32, 2,          papd_comp_epsilon_tbl_core2_rev0,  "papd_comp_epsilon_tbl_core2"),
	TBL_POPULATED_FIRST(0x48, 0, 32, 0,          papd_cal_scalars_tbl_core0_rev0,   "papd_cal_scalars_tbl_core0"),
	TBL_POPULATED_FIRST(0x68, 0, 32, 1,          papd_cal_scalars_tbl_core1_rev0,   "papd_cal_scalars_tbl_core1"),
	TBL_POPULATED_FIRST(0x88, 0, 32, 2,          papd_cal_scalars_tbl_core2_rev0,   "papd_cal_scalars_tbl_core2"),
};

#undef TBL_POPULATED
#undef TBL_ZERO
#undef TBL_POPULATED_FIRST
#undef TBL_ZERO_FIRST

/* Init */

/* TODO: calibrate. */
void b43_phy_ac_tables_init(struct b43_wldev *dev)
{
	B43_AC_FN();
	const struct b43_phy_ac_table_desc *t;
	u16 saved;
	size_t i;

	saved = b43_phy_ac_tbl_write_lock(dev);
	for (i = 0; i < ARRAY_SIZE(b43_phy_ac_tables_rev0); i++) {
		t = &b43_phy_ac_tables_rev0[i];
		if (t->core != TBL_SHARED && t->core >= dev->phy.ac->num_cores)
			continue;
		/*
		 * do_full_init and not the latched
		 * B43_PHY_AC_STATE_FIRST_BRINGUP: op_init latches that flag
		 * after this call, so reading it here would give the previous
		 * cycle's phase.
		 */
		if (t->first_only && !dev->phy.do_full_init)
			continue;

		/*
		 * The _locked variants: the gate stays locked for the whole loop,
		 * so the individual bulks do not re-read it.
		 */
		if (t->zero)
			b43_actab_zerofill_locked(dev, t->id, t->offset,
						  t->width, t->len);
		else if (t->data)
			b43_actab_write_bulk_locked(dev, t->id, t->offset,
						    t->width, t->len, t->data);
	}

	b43_phy_ac_tbl_write_unlock(dev, saved);
}

/*
 * Alternate-port table write (id 0x11 style). The blob programs this table
 * one word at a time via the data register 0x0011 (not DATA_LO 0x00f /
 * DATA_HI 0x010), re-selecting id+offset for every word and with no address
 * auto-increment -- see trace. Reproduced faithfully here.
 */
void b43_actab_write_r11(struct b43_wldev *dev,
			 u16 id, u16 offset, size_t len, const u16 *data)
{
	size_t i;

	/*
	 * The vendor's pattern for table 0x11, 464 u16s: for each cell, peek
	 * 0x019e, write the table id, write the offset, write DATA_2 at 0x0011.
	 * It does not rely on the offset auto-incrementing, which is what
	 * actab_write_bulk() does; it reselects id and offset for every cell.
	 * Five ops per cell over 464 cells, so 2320 ops.
	 *
	 * The TBL.WR label comes from the tracer's wrap; see test/wrap.c.
	 */
	for (i = 0; i < len; i++) {
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
		b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, (u16)(offset + i));
		b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_2, data[i]);
	}
}

void b43_actab_fill_r11(struct b43_wldev *dev,
			u16 id, u16 offset, size_t len, u16 val)
{
	size_t i;

	for (i = 0; i < len; i++) {
		b43_phy_read_log(dev, B43_PHY_AC_REG_TBL_WRITE_GATE);
		b43_phy_write(dev, B43_PHY_AC_TABLE_ID, id);
		b43_phy_write(dev, B43_PHY_AC_TABLE_OFFSET, (u16)(offset + i));
		b43_phy_write(dev, B43_PHY_AC_TABLE_DATA_2, val);
	}
}
