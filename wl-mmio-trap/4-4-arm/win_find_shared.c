// SPDX-License-Identifier: GPL-2.0
/* win_find.c is shared with the MIPS build and lives one level up; building
 * it here through a one-line include keeps a single copy. Its only arch
 * dependency is VMALLOC_START/END and the page-table walk, both of which
 * asm/pgtable.h provides on ARM too. */
#include "../win_find.c"
