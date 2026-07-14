// SPDX-License-Identifier: GPL-2.0
/*
 * mempeek - legge registri MMIO via mmap di /dev/mem (userspace, niente modulo).
 *
 * Nato come fallback a cc-dump quando sul device manca devmem/dd e caricare un
 * .ko crasha per mismatch di build: essendo userspace non ha vermagic, ne'
 * dipendenza dal layout di struct module o dall'albero kernel vendor.
 *
 * Build (con la tua toolchain MIPS, statico cosi' non serve libc sul device):
 *     mips-linux-gnu-gcc -O2 -static -o mempeek mempeek.c
 * Trasferiscilo (tftp/nc) in /tmp sul device, chmod +x, ed esegui.
 *
 * Uso:
 *     ./mempeek <phys_hex> [count]      # legge 'count' word da 32 bit (def. 1)
 * Esempi (BAR0 agcombo = 0x11000000):
 *     ./mempeek 0x11000000              # chipid  (atteso ~0x4360)
 *     ./mempeek 0x1100061c              # max_res_mask  <-- confronto con 0x1ff
 *     ./mempeek 0x11000600 8            # blocco PMU: ctl,cap,stat,res_state,
 *                                       #   res_pending,timer,minres,maxres
 * Stampa valore raw e byte-swapped: se il chipid esce ribaltato usa il bswap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "uso: %s <phys_hex> [count]\n", argv[0]);
		return 2;
	}

	unsigned long phys = strtoul(argv[1], NULL, 16);
	int n = (argc > 2) ? atoi(argv[2]) : 1;
	if (n < 1)
		n = 1;

	unsigned long pg = sysconf(_SC_PAGESIZE);
	unsigned long base = phys & ~(pg - 1);
	unsigned long off = phys - base;
	unsigned long span = off + (unsigned long)n * 4;

	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}

	volatile unsigned char *m = mmap(NULL, span, PROT_READ, MAP_SHARED, fd, base);
	if (m == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	for (int i = 0; i < n; i++) {
		volatile uint32_t *p = (volatile uint32_t *)(m + off + (unsigned long)i * 4);
		uint32_t v = *p;
		uint32_t s = __builtin_bswap32(v);
		printf("0x%08lx: 0x%08x  (bswap 0x%08x)\n", phys + (unsigned long)i * 4, v, s);
	}

	munmap((void *)m, span);
	close(fd);
	return 0;
}
