/* SPDX-License-Identifier: GPL-2.0 */
/* The kernel's SMP barriers as full host barriers. */
#ifndef SHIM_ASM_BARRIER_H
#define SHIM_ASM_BARRIER_H

#define smp_mb()	__sync_synchronize()
#define smp_rmb()	__sync_synchronize()
#define smp_wmb()	__sync_synchronize()

#endif
