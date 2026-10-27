/*
 * nvram_backend.h — a `wl nvram_dump` file served to bcm47xx_sprom.c as a
 * router's NVRAM, every variable under the given device prefix ("0:", as on
 * a board whose radios have no SROM).
 */

#ifndef NVRAM_BACKEND_H
#define NVRAM_BACKEND_H

int nvram_backend_load(const char *path, const char *prefix);

/* The dump's value of an unprefixed variable, NULL if absent. */
const char *nvram_backend_value(const char *name);

#endif /* NVRAM_BACKEND_H */
