/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SHIM_LINUX_COMPILER_H
#define SHIM_LINUX_COMPILER_H

#define ACCESS_ONCE(x)	(*(volatile __typeof__(x) *)&(x))

#endif
