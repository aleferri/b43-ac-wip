/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STUB_LINUX_KERNEL_H
#define _STUB_LINUX_KERNEL_H

#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n)        (1UL << (n))
#define BIT_ULL(n)    (1ULL << (n))

#ifndef min
#define min(a, b)  ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b)  ((a) > (b) ? (a) : (b))
#endif
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define clamp_t(t, v, lo, hi) ((t)clamp((v), (lo), (hi)))
#define abs(x) ((x) < 0 ? -(x) : (x))

#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1) / (d))
#define round_up(n, m)       (((n) + (m) - 1) & ~((m) - 1))
#define round_down(n, m)     ((n) & ~((m) - 1))

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define __always_unused __attribute__((unused))
#define __maybe_unused  __attribute__((unused))
#define __must_check    __attribute__((warn_unused_result))

#define IS_ENABLED(cfg) 0

/* Bit-manipulation helpers used by the driver. */
static inline int __ffs(unsigned long x)  { return __builtin_ctzl(x); }
static inline int fls(int x)              { return x ? 32 - __builtin_clz((unsigned)x) : 0; }
static inline int hweight32(u32 x)        { return __builtin_popcount(x); }

/*
 * lib/int_sqrt.c bit-by-bit integer square root. Kernel version handles
 * the same integer domain and produces the same result; no floating
 * point involved. Verbatim from the well-known "digit-by-digit" method.
 */
static inline unsigned long int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;
	if (x <= 1)
		return x;
	m = 1UL << ((__builtin_clzl(1) - __builtin_clzl(x)) & ~1UL);
	while (m) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}

/* Console I/O routed to stderr so it doesn't pollute the trace on stdout. */
#define pr_err(fmt, ...)     fprintf(stderr, "err: "  fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    fprintf(stderr, "warn: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    fprintf(stderr, "info: " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   fprintf(stderr, "dbg: "  fmt, ##__VA_ARGS__)
#define printk(fmt, ...)     fprintf(stderr, fmt, ##__VA_ARGS__)

#define KERN_ERR   ""
#define KERN_WARN  ""
#define KERN_INFO  ""
#define KERN_DEBUG ""

#define WARN_ON(x)     ({ if (x) fprintf(stderr, "WARN_ON at %s:%d\n", __FILE__, __LINE__); (int)!!(x); })
#define WARN_ON_ONCE(x) WARN_ON(x)
#define BUG_ON(x)      do { if (x) { fprintf(stderr, "BUG_ON at %s:%d\n", __FILE__, __LINE__); abort(); } } while (0)

/* Kernel string formatting: fall back to sprintf. */
#define scnprintf snprintf

#endif
