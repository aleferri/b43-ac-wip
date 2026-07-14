/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STUB_LINUX_SLAB_H
#define _STUB_LINUX_SLAB_H

#include <linux/kernel.h>

static inline void *kzalloc(size_t sz, gfp_t f __attribute__((unused)))
{
	return calloc(1, sz);
}
static inline void *kmalloc(size_t sz, gfp_t f __attribute__((unused)))
{
	return malloc(sz);
}
static inline void kfree(const void *p) { free((void *)p); }

/*
 * kzalloc_obj(*ptr): helper used by the scratch driver — allocates a
 * zero-initialised block sized to the pointee type. Expands to the
 * standard kzalloc(sizeof(*ptr), GFP_KERNEL) idiom.
 */
#define kzalloc_obj(obj) kzalloc(sizeof(obj), GFP_KERNEL)

#endif
