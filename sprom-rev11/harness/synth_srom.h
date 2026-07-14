/*
 * synth_srom.h — encode a wl_nvram-style key/value table into a raw
 * rev-11 SROM image, using the byte offsets and bit-packing defined by
 * the patch's ssb_regs.h. Pair to the parser body in extract_r11.c.
 *
 * Used by test_synth.c to round-trip NVRAM-only vectors (e.g. the
 * BCM4360 USB defaults from asuswrt-merlin's bcmsrom.c) where no raw
 * `wl srdump` is available. The round-trip checks structural completeness
 * of extract_r11 (every NVRAM key it claims to handle is recoverable)
 * and self-consistency of the patch's offsets/encoding (synth and parse
 * agree). It does NOT cross-validate offsets against an external source;
 * that is the job of cross_check.md and would require importing
 * Broadcom's bcmsrom_tbl.h.
 *
 * Fields the NVRAM does not declare are written as zero. The caller is
 * responsible for skipping diff checks on those fields.
 */

#ifndef SYNTH_SROM_H
#define SYNTH_SROM_H

#include <stddef.h>
#include "kernel_shim.h"
#include "data_load.h"

/* Encode `nv` into the SROM word buffer `srom` of `words` u16 entries.
 * Returns 0 on success; -1 if `words` is too small for the rev-11
 * region the patch addresses (worst case: byte 0x190 ≈ 200 words).
 *
 * The synth touches only the bytes the patch's parser reads; bytes
 * outside that footprint stay at their incoming value (caller should
 * memset to 0 first if a clean slate is wanted). */
int synth_srom_from_nvram(const struct nvram *nv, u16 *srom, size_t words);

#endif /* SYNTH_SROM_H */
