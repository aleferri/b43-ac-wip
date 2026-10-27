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

int nvram_backend_load(const char *path, const char *prefix)
{
	nv_prefix = prefix;
	return nvram_load(path, &nv);
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
