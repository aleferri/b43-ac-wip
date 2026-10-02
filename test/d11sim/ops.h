/*
 * ops.h - replay a decoded bus op stream into the core + fake PHY.
 *
 * The vocabulary is the one shared by reverse-tools/mmio2ops.py (wl captures)
 * and test/integration/trace_out.c (b43), so the same replayer turns either
 * driver's op stream into a final core state. Run the ops through --no-bulk so
 * every OBJ write still carries its value; a folded OBJ.BULKW has no values to
 * replay and is counted as skipped.
 */
#ifndef OPS_H
#define OPS_H

#include <stdio.h>
#include "d11_core.h"
#include "fakephy.h"

struct replay_stats {
	unsigned long lines, applied, ignored;
	unsigned long reg_wr, reg_rd, obj_wr, obj_rd;
	unsigned long phy_wr, phy_rd, rad_wr, rad_rd;
	unsigned long mac_mctrl, mac_mcmd;
	unsigned long bulk_skipped;   /* OBJ.BULK* lines with no values        */
	unsigned long parse_errors;
	int ucode_booted;             /* handshake fired during replay         */
};

void replay_stream(FILE *in, struct d11_core *core, struct fakephy *phy,
		   struct replay_stats *st);

#endif /* OPS_H */
