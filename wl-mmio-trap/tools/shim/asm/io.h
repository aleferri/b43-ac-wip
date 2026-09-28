/* SPDX-License-Identifier: GPL-2.0 */
/* Host stand-ins for the bare bus accessors. Native byte order on both
 * sides, which is what the target's __raw_* do as well: they move the
 * bytes a bare lb/lh/lw/sb/sh/sw would move, with no swap. */
#ifndef SHIM_ASM_IO_H
#define SHIM_ASM_IO_H

#include <linux/types.h>

static inline u8  __raw_readb(const volatile void *a) { return *(const volatile u8 *)a; }
static inline u16 __raw_readw(const volatile void *a) { return *(const volatile u16 *)a; }
static inline u32 __raw_readl(const volatile void *a) { return *(const volatile u32 *)a; }
static inline void __raw_writeb(u8 v, volatile void *a)  { *(volatile u8 *)a = v; }
static inline void __raw_writew(u16 v, volatile void *a) { *(volatile u16 *)a = v; }
static inline void __raw_writel(u32 v, volatile void *a) { *(volatile u32 *)a = v; }

#endif
