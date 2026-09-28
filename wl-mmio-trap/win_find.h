/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Finds wl's register window without being told where it is.
 *
 * Two steps, because the two facts come from different places:
 *
 *  1. win_find_bar() asks the PCI layer for the device's BAR0 -- a
 *     physical address and a length, known as soon as the device is
 *     enumerated, whether or not wl is loaded.
 *  2. win_find_mapping() scans the kernel page tables over the vmalloc
 *     range for the virtual address whose pte carries that physical page,
 *     which only exists once wl has actually called ioremap on it.
 *
 * Step 2 is also the check that the window is TLB-mapped at all. On this
 * architecture __ioremap() short-circuits an uncached mapping of anything
 * in the low 512 MB of physical space to a bare CKSEG1 address, which has
 * no page table entry to invalidate -- so on a board whose BAR0 sits below
 * 512 MB this whole mechanism cannot work, and the honest outcome is
 * "no mapping found", not a half-armed trap. The distinction is reported
 * by win_find_mapping()'s return code.
 */
#ifndef WIN_FIND_H
#define WIN_FIND_H

#include <linux/types.h>

struct win_bar {
	phys_addr_t phys;
	size_t len;
	u16 vendor, device;
	u32 pci_class;
	unsigned int bus, devfn;
};

/* Physical addresses below this are reached through CKSEG1 by __ioremap()
 * and have no page table entry at all: see win_find.h's header comment and
 * DESIGN.md section 5. */
#define WIN_KSEG1_LIMIT 0x20000000UL

/* vendor/device may be PCI_ANY_ID; with an unspecified device id the
 * search is restricted to network-class functions, or the first match on
 * these SoCs is the PCIe host bridge core, which shares the vendor id with
 * the radio behind it and has a perfectly usable BAR of its own.
 *
 * verbose lists every function that carries the vendor, accepted or not,
 * so one line of dmesg shows the whole topology. It has to be off on the
 * retries, or the discovery poll floods the log.
 *
 * -ENODEV when nothing matches. */
int win_find_bar(unsigned int vendor, unsigned int device, int bar_index,
		 bool verbose, struct win_bar *out);

/* Every PCI function the kernel knows about, with the BAR in question.
 * Printed when the targeted search comes up empty, so a failure says what
 * IS there rather than only what is not. */
void win_find_list_all(int bar_index);

/* -EAGAIN: nothing maps that physical page yet (wl has not ioremapped it,
 *          or it is reached through CKSEG1 and has no pte).
 * -ERANGE: found, but fewer contiguous pages than asked for; *va_out and
 *          *len_out still describe what was found. */
int win_find_mapping(phys_addr_t phys, size_t len,
		     unsigned long *va_out, size_t *len_out);

#endif /* WIN_FIND_H */
