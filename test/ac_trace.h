/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Trace points of the AC-PHY, for the two harnesses. Force-included
 * (-include) ahead of the driver, whose phy_ac.h has no-op defaults.
 *
 * B43_AC_FN() at the top of a function brackets the ops that follow with the
 * function name, so fn_map.py can segment the trace by exact boundaries. The
 * exit marker is emitted on any return through GCC's cleanup attribute, so
 * nested calls nest.
 *
 * B43_AC_BLOCK(dev, "name") names a section of a long function worth locating on
 * its own: a function-level capture marker collapses every stretch a function
 * accounts for into one interval, and op_switch_channel() covers about
 * forty. A point marker with no closing counterpart, since the sections are
 * stretches of straight-line code, not braced blocks: a block runs until the
 * next marker or the end of the function, which anchors.py closes.
 */
#ifndef B43_TEST_AC_TRACE_H_
#define B43_TEST_AC_TRACE_H_

void b43_ac_fn_enter(const char *fn);
void b43_ac_fn_leave(const char *fn);
void b43_ac_block_mark(const char *name);

static inline void b43_ac_fn_cleanup(const char *const *fn)
{
	b43_ac_fn_leave(*fn);
}

#define B43_AC_FN() \
	const char *const __b43_fn __attribute__((cleanup(b43_ac_fn_cleanup))) = \
		__func__; \
	b43_ac_fn_enter(__func__)

#define B43_AC_BLOCK(dev, name) b43_ac_block_mark(name)

#endif /* B43_TEST_AC_TRACE_H_ */
