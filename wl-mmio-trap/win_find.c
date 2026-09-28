// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/fixmap.h>
#include <asm/highmem.h>	/* VMALLOC_END, same reason fault.c includes it */

#include "win_find.h"
#include "mmio_pte.h"

void win_find_list_all(int bar_index)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL)
		pr_info("wl_mmio_trap:   %s [%04x:%04x] class %06x BAR%d = %08llx +%llu flags %lx\n",
			pci_name(pdev), pdev->vendor, pdev->device,
			pdev->class, bar_index,
			(unsigned long long)pci_resource_start(pdev, bar_index),
			(unsigned long long)pci_resource_len(pdev, bar_index),
			pci_resource_flags(pdev, bar_index));
}

int win_find_bar(unsigned int vendor, unsigned int device, int bar_index,
		 bool verbose, struct win_bar *out)
{
	struct pci_dev *pdev = NULL;
	bool got = false;

	while ((pdev = pci_get_device(vendor, device, pdev)) != NULL) {
		resource_size_t start = pci_resource_start(pdev, bar_index);
		resource_size_t len = pci_resource_len(pdev, bar_index);
		unsigned long flags = pci_resource_flags(pdev, bar_index);
		const char *verdict;
		bool take;

		if (device == PCI_ANY_ID &&
		    (pdev->class >> 16) != PCI_BASE_CLASS_NETWORK) {
			take = false;
			verdict = "skipped: not a network function";
		} else if (!(flags & IORESOURCE_MEM) || !start ||
			   len < PAGE_SIZE) {
			take = false;
			verdict = "skipped: BAR unusable";
		} else if (got) {
			take = false;
			verdict = "ignored: already have one";
		} else {
			take = true;
			verdict = "taken";
		}

		if (verbose)
			pr_info("wl_mmio_trap: %s [%04x:%04x] class %06x BAR%d = %08llx +%llu -- %s\n",
				pci_name(pdev), pdev->vendor, pdev->device,
				pdev->class, bar_index,
				(unsigned long long)start,
				(unsigned long long)len, verdict);

		if (!take)
			continue;

		out->phys = (phys_addr_t)start;
		out->len = (size_t)len;
		out->vendor = pdev->vendor;
		out->device = pdev->device;
		out->pci_class = pdev->class;
		out->bus = pdev->bus->number;
		out->devfn = pdev->devfn;
		got = true;
		/* No break: the loop keeps going so the verbose listing is
		 * complete, and pci_get_device drops the reference it took
		 * on the previous device for us. */
	}

	if (!got && verbose) {
		pr_err("wl_mmio_trap: no %04x:%04x with a usable BAR%d. Everything the PCI layer knows about follows; if the radio is not in it, it is not enumerated as a PCI function and win_phys= is the way in.\n",
		       vendor, device, bar_index);
		win_find_list_all(bar_index);
	}
	return got ? 0 : -ENODEV;
}

int win_find_mapping(phys_addr_t phys, size_t len,
		     unsigned long *va_out, size_t *len_out)
{
	unsigned long target_pfn = (unsigned long)(phys >> PAGE_SHIFT);
	unsigned int want = len ? (unsigned int)DIV_ROUND_UP(len, PAGE_SIZE) : 1;
	unsigned long start = (unsigned long)VMALLOC_START & PGDIR_MASK;
	unsigned long end = (unsigned long)VMALLOC_END;
	unsigned long ndirs = (end - start + PGDIR_SIZE - 1) / PGDIR_SIZE;
	unsigned long d;

	*va_out = 0;
	*len_out = 0;

	/* Stepping a PGDIR at a time and descending only where the top level
	 * is populated keeps this to a few thousand pte reads on a 32-bit
	 * vmalloc range: cheap enough to re-run from a poll. The loop counts
	 * directories rather than comparing addresses, so a vmalloc range
	 * ending at the top of the address space cannot wrap it. */
	for (d = 0; d < ndirs; d++) {
		unsigned long chunk = start + d * PGDIR_SIZE;
		unsigned long off;

		if (!mmio_pte_walk(chunk))
			continue;

		for (off = 0; off < PGDIR_SIZE; off += PAGE_SIZE) {
			unsigned long va = chunk + off;
			pte_t *ptep;
			unsigned int n;

			if (va < (unsigned long)VMALLOC_START || va >= end)
				continue;
			ptep = mmio_pte_walk(va);
			if (!ptep || !pte_present(*ptep))
				continue;
			if (pte_pfn(*ptep) != target_pfn)
				continue;

			/* Found the first page. Walk forward for as long as
			 * the physical pages stay consecutive: a shorter run
			 * means wl mapped less than the whole BAR, and the
			 * caller gets to decide what to do about it. */
			for (n = 1; n < want; n++) {
				pte_t *p = mmio_pte_walk(va + n * PAGE_SIZE);

				if (!p || !pte_present(*p) ||
				    pte_pfn(*p) != target_pfn + n)
					break;
			}

			*va_out = va;
			*len_out = (size_t)n * PAGE_SIZE;
			pr_info("wl_mmio_trap: phys %08llx is mapped at %p, %u/%u pages contiguous\n",
				(unsigned long long)phys, (void *)va, n, want);
			return n == want ? 0 : -ERANGE;
		}
	}
	return -EAGAIN;
}
