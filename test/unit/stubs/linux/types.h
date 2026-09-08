/* SPDX-License-Identifier: GPL-2.0
 * Minimal stub of <linux/types.h> for userspace build of the b43 AC-PHY
 * scratch. Only the types actually used by the code under test are defined.
 */
#ifndef _STUB_LINUX_TYPES_H
#define _STUB_LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef u32 __le32;
typedef u16 __le16;
typedef u32 __be32;
typedef u16 __be16;

/* GFP flags -- semantically no-op in tests. */
typedef unsigned int gfp_t;
#define GFP_KERNEL 0

typedef unsigned long dma_addr_t;

#endif
