/*
 * nvram_backend.c — bcm47xx_nvram_getenv() over a `wl nvram_dump` file.
 */

#include "data_load.h"
#include "nvram_backend.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct nvram nv;
static const char *nv_prefix;

/*
 * Strip @prefix off every key that carries it and drop the rest, so nv holds
 * the radio's bare variables whatever prefix the dump used ("0:", "pci/1/1/",
 * or "" for an already-bare dump). bcm47xx_nvram_getenv() and the test's own
 * reads then work on bare keys, and the backend's prefix is "".
 */
int nvram_backend_load(const char *path, const char *prefix)
{
	size_t plen = strlen(prefix);
	size_t i, kept = 0;

	nv_prefix = "";
	if (nvram_load(path, &nv) < 0)
		return -1;
	if (!plen)
		return 0;

	for (i = 0; i < nv.n; i++) {
		if (strncmp(nv.items[i].key, prefix, plen))
			continue;	/* not this radio's variable */
		if (kept != i)
			nv.items[kept] = nv.items[i];
		memmove(nv.items[kept].key, nv.items[kept].key + plen,
			strlen(nv.items[kept].key + plen) + 1);
		kept++;
	}
	nv.n = kept;
	if (!kept)
		fprintf(stderr,
			"nvram_backend: no key under prefix '%s'; is the prefix right?\n",
			prefix);
	return 0;
}

const char *nvram_backend_value(const char *name)
{
	return nvram_get(&nv, name);
}

int bcm47xx_nvram_getenv(const char *name, char *val, size_t val_len)
{
	size_t len = strlen(nv_prefix);
	const char *v;

	if (strncmp(name, nv_prefix, len))
		return -ENOENT;
	v = nvram_get(&nv, name + len);
	if (!v)
		return -ENOENT;
	return snprintf(val, val_len, "%s", v);
}
