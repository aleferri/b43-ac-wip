/*
 * kernel_shim.h — userspace shim of the kernel symbols used by
 * bcma_sprom_extract_r11().
 *
 * The goal of this harness is to compile drivers/bcma/sprom.c's
 * extract_r11 routine **verbatim** in userspace and exercise it
 * against the DSL-3580L test vector. To do that we must reproduce
 * the kernel-side names for fixed-width integer types, the byte-
 * order helper, BUILD_BUG_ON, ARRAY_SIZE, and the SPROM extraction
 * macros (SPOFF / SPEX / SPEX32) — copied here byte-for-byte from
 * drivers/bcma/sprom.c upstream.
 *
 * If those macros change shape upstream, this shim must be updated
 * to match before rerunning the test, or the test will silently
 * exercise a different parser than the one going to mainline.
 */

#ifndef KERNEL_SHIM_H
#define KERNEL_SHIM_H

#define _DEFAULT_SOURCE  /* expose htobe16() from <endian.h> */

#include <stdint.h>
#include <endian.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* The kernel uses __be16 for the BE-stored portion of il0mac inside
 * struct ssb_sprom. We mirror only the storage shape, not the
 * sparse/__bitwise annotation. */
typedef uint16_t __be16;

/* drivers/bcma/sprom.c uses cpu_to_be16() to byteswap each u16 word
 * of the MAC address into the il0mac u8[6] slot. Userspace htobe16()
 * has identical semantics. */
#define cpu_to_be16(x) htobe16(x)

/* ARRAY_SIZE: drivers/bcma/sprom.c relies on the kernel-wide
 * definition. The compile-time-constant version below is sufficient
 * for the call sites we exercise (BUILD_BUG_ON on r11_pwr_info_offset
 * vs core_pwr_info, plus the loop bound). */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* BUILD_BUG_ON in the kernel triggers a compile error if the
 * expression is non-zero. _Static_assert has the same semantics for
 * compile-time-constant expressions, which is what the parser uses
 * it for here (sizing invariants between r11_pwr_info_offset and
 * core_pwr_info). */
#define BUILD_BUG_ON(cond) _Static_assert(!(cond), #cond)

/* --- SPROM extraction macros -----------------------------------------
 * Verbatim copy of drivers/bcma/sprom.c upstream, lines defining
 * SPOFF, SPEX, SPEX32. These rely on `bus` being in scope as a
 * `struct bcma_bus *` and on `sprom` being in scope as a `const u16 *`,
 * which is exactly the calling convention bcma_sprom_extract_r11()
 * uses, identical to bcma_sprom_extract_r8().
 */
#define SPOFF(offset)	((offset) / sizeof(u16))

#define SPEX(_field, _offset, _mask, _shift)	\
	bus->sprom._field = ((sprom[SPOFF(_offset)] & (_mask)) >> (_shift))

#define SPEX32(_field, _offset, _mask, _shift)	\
	bus->sprom._field = ((((u32)sprom[SPOFF((_offset)+2)] << 16 | \
				sprom[SPOFF(_offset)]) & (_mask)) >> (_shift))

#endif /* KERNEL_SHIM_H */
