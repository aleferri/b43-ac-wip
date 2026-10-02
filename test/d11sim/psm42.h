/*
 * psm42.h - PSM (programmable state machine / microcode engine) layer for a
 * corerev-42 d11 core.
 *
 * SCOPE: this C layer only loads the extracted blob into the core's ucode
 * memory and inventories it. Executing the microcode is the job of the arch15
 * interpreter in b43-tools (interpreter/psm.py), driven by ucode_init.py: that
 * runs the real instructions and produces the ucode-authored shared-memory
 * state (revinfo and the rest of the version-stamp prologue). The host-visible
 * MAC_SUSPENDED handshake b43 polls for is raised in d11_core.c; nothing here
 * fabricates ucode output.
 */
#ifndef PSM42_H
#define PSM42_H

#include <stdint.h>
#include <stddef.h>
#include "d11_core.h"

struct psm42 {
	struct d11_core *core;
};

void psm42_attach(struct psm42 *p, struct d11_core *core);

/* Load a raw big-endian ucode blob (as extracted from the wl object) straight
 * into ucode memory, bypassing the OBJADDR/OBJDATA upload path. Returns the
 * number of 32-bit words loaded. */
size_t psm42_load_blob(struct psm42 *p, const uint8_t *blob, size_t nbytes);

/* Apply the modelled init handshake. Returns 1 if it fired this call. */
int psm42_boot(struct psm42 *p);

/* Static inventory of the loaded ucode: word count and the frequency of the
 * most-significant byte of each word. This is an opaque statistic for sanity
 * checking a blob, not a disassembly: the field is not claimed to be an
 * opcode. Writes a short report to the FILE. */
void psm42_report_inventory(const struct psm42 *p, void *FILEp);

#endif /* PSM42_H */
