/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STUB_LINUX_ERRNO_H
#define _STUB_LINUX_ERRNO_H

/* Standard errno.h has most of these; add the kernel-specific ones. */
#include <errno.h>

#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef ESRCH
#define ESRCH 3
#endif

#endif
