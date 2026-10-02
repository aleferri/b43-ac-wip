/*
 * snapshot.h - write a canonical dump of the core's host-visible state.
 *
 * One line per non-zero cell:
 *     <REGION> <key> <value>
 * REGION in {REG, SHM, SCR, HW, RCMTA, UCODE}. key and value are 0x-hex.
 * REG/SHM keys are byte offsets (REG 32-bit words, SHM 16-bit words); the
 * index-addressed spaces use the word index. Zero cells are omitted so the
 * dump is sparse and the diff is about what each driver actually set.
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdio.h>
#include "d11_core.h"

void snapshot_write(const struct d11_core *c, FILE *out, int include_ucode);

#endif /* SNAPSHOT_H */
