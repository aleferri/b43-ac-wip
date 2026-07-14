/* SPDX-License-Identifier: GPL-2.0
 * Stub linux/delay.h — tracing wrap.c decides whether to log delays.
 */
#ifndef _STUB_LINUX_DELAY_H
#define _STUB_LINUX_DELAY_H

#include <linux/types.h>

extern void udelay(unsigned long us);
extern void mdelay(unsigned long ms);
extern void msleep(unsigned int ms);
extern void usleep_range(unsigned long min_us, unsigned long max_us);

#endif
