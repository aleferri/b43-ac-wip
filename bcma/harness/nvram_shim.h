/*
 * nvram_shim.h — userspace shim of the kernel symbols that
 * drivers/firmware/broadcom/bcm47xx_sprom.c uses outside its bcma and ssb
 * fallback handlers, which test_nvram compiles out (CONFIG_BCMA and
 * CONFIG_SSB not built in). The headers under nvram_shim/ stand in for the
 * kernel ones the file includes and all come here, except
 * linux/bcm47xx_sprom.h, which is the tree's own.
 */

#ifndef NVRAM_SHIM_H
#define NVRAM_SHIM_H

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef uint32_t u32;

#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif

#define __aligned(x)		__attribute__((aligned(x)))
#define BIT(n)			(1UL << (n))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define ETH_ALEN		6
#define IS_BUILTIN(option)	0
#define IS_ENABLED(option)	0
#define fs_initcall(fn) \
	static int (*const fs_initcall_ ## fn)(void) __attribute__((unused)) = fn

#define pr_warn(...)		fprintf(stderr, "warn: " __VA_ARGS__)
#define pr_err(...)		fprintf(stderr, "err: " __VA_ARGS__)

/* Provided by nvram_backend.c over the loaded dump. */
int bcm47xx_nvram_getenv(const char *name, char *val, size_t val_len);

/* The kernel's: the whole string, one trailing newline allowed. */
static inline int shim_kstrtoull(const char *s, unsigned int base,
				 unsigned long long max,
				 unsigned long long *res)
{
	unsigned long long v;
	char *end;

	if (*s == '-')
		return -EINVAL;
	errno = 0;
	v = strtoull(s, &end, base);
	if (end == s)
		return -EINVAL;
	if (*end == '\n')
		end++;
	if (*end)
		return -EINVAL;
	if (errno || v > max)
		return -ERANGE;
	*res = v;
	return 0;
}

static inline int shim_kstrtoll(const char *s, unsigned int base,
				long long min, long long max, long long *res)
{
	long long v;
	char *end;

	errno = 0;
	v = strtoll(s, &end, base);
	if (end == s)
		return -EINVAL;
	if (*end == '\n')
		end++;
	if (*end)
		return -EINVAL;
	if (errno || v < min || v > max)
		return -ERANGE;
	*res = v;
	return 0;
}

#define SHIM_KSTRTOU(type, max)						\
static inline int kstrto ## type(const char *s, unsigned int base,	\
				 type *res)				\
{									\
	unsigned long long v;						\
	int err = shim_kstrtoull(s, base, max, &v);			\
									\
	if (!err)							\
		*res = v;						\
	return err;							\
}
SHIM_KSTRTOU(u8, UINT8_MAX)
SHIM_KSTRTOU(u16, UINT16_MAX)
SHIM_KSTRTOU(u32, UINT32_MAX)
#undef SHIM_KSTRTOU

static inline int kstrtos8(const char *s, unsigned int base, s8 *res)
{
	long long v;
	int err = shim_kstrtoll(s, base, INT8_MIN, INT8_MAX, &v);

	if (!err)
		*res = v;
	return err;
}

static inline char *strim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		e--;
	*e = 0;
	return s;
}

static inline char *strreplace(char *s, char old, char new)
{
	for (; *s; s++)
		if (*s == old)
			*s = new;
	return s;
}

static inline bool mac_pton(const char *s, u8 *mac)
{
	unsigned int m[ETH_ALEN];
	int n, i;

	if (strlen(s) != 3 * ETH_ALEN - 1)
		return false;
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%n", &m[0], &m[1], &m[2],
		   &m[3], &m[4], &m[5], &n) != ETH_ALEN || n != 3 * ETH_ALEN - 1)
		return false;
	for (i = 0; i < ETH_ALEN; i++)
		mac[i] = m[i];
	return true;
}

static inline void ether_addr_copy(u8 *dst, const u8 *src)
{
	memcpy(dst, src, ETH_ALEN);
}

#endif /* NVRAM_SHIM_H */
