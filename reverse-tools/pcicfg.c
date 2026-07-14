// SPDX-License-Identifier: GPL-2.0
/*
 * pcicfg - legge/scrive il config space PCI via /proc/bus/pci/<bus>/<dev.fn>.
 *
 * Serve per ripuntare la finestra di backplane del BAR0 (registro BAR0_WIN,
 * config offset 0x80) sul core ChipCommon (backplane base 0x18000000), cosi'
 * che mempeek possa leggere i registri PMU a BAR0+0x600...
 *
 * Build (statico, con la tua toolchain MIPS):
 *     mips-linux-gnu-gcc -O2 -static -o pcicfg pcicfg.c
 *
 * Uso:
 *     pcicfg <path> <off_hex>            # legge una word da 32 bit
 *     pcicfg <path> <off_hex> <val_hex>  # scrive una word da 32 bit
 * dove <path> e' es. /proc/bus/pci/02/00.0  (bus 02, dev 00, fn 0).
 *
 * Il config space PCI e' little-endian: i byte vengono assemblati/scritti a
 * mano in LE, quindi il risultato e' corretto anche su host big-endian (MIPS).
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "uso: %s <path> <off_hex> [val_hex]\n", argv[0]);
		return 2;
	}

	const char *path = argv[1];
	unsigned long off = strtoul(argv[2], NULL, 16);
	int writing = (argc > 3);

	int fd = open(path, writing ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	if (writing) {
		uint32_t v = (uint32_t)strtoul(argv[3], NULL, 16);
		unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff,
				       (v >> 16) & 0xff, (v >> 24) & 0xff };
		if (pwrite(fd, b, 4, off) != 4) {
			perror("pwrite");
			close(fd);
			return 1;
		}
		printf("scritto 0x%08x @ %s+0x%lx\n", v, path, off);
	} else {
		unsigned char b[4];
		if (pread(fd, b, 4, off) != 4) {
			perror("pread");
			close(fd);
			return 1;
		}
		uint32_t v = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
			     (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
		printf("%s+0x%lx = 0x%08x\n", path, off, v);
	}

	close(fd);
	return 0;
}
