/*
 * data_load.h — load test vectors from `wl srdump` and `wl nvram_dump`
 * text files into in-memory representations the test driver consumes.
 */

#ifndef DATA_LOAD_H
#define DATA_LOAD_H

#include <stddef.h>
#include "kernel_shim.h"

/* SROM is at most 1024 u16 words (2 KB). The DSL-3580L dump is
 * shorter; the buffer is sized to cover any plausible rev-11 SROM. */
#define SROM_MAX_WORDS 1024

/* Parse a `wl srdump` text file into a u16 buffer.
 * Lines have the shape "  srom[NNN]:  0xWWWW 0xWWWW ...".
 * Returns the highest word index touched + 1 (i.e. effective length),
 * or -1 on read/parse error. Words not covered by the file remain 0. */
int srom_load(const char *path, u16 *buf, size_t buf_words);

/* Opaque nvram_dump key-value table. Backed by a fixed-size array; the
 * DSL-3580L dump has ~160 entries, so the cap is comfortable. */
struct nvram_kv {
	char key[64];
	char val[256];
};
struct nvram {
	struct nvram_kv items[512];
	size_t n;
};

/* Parse a `wl nvram_dump` text file. Lines have the shape "key=value".
 * Empty values and trailing whitespace are preserved. Returns 0 on
 * success, -1 on read error. */
int nvram_load(const char *path, struct nvram *out);

/* Look up a key. Returns NULL if absent. */
const char *nvram_get(const struct nvram *nv, const char *key);

#endif /* DATA_LOAD_H */
