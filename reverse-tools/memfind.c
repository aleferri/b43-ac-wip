// SPDX-License-Identifier: GPL-2.0
/*
 * memfind - cerca word a 32 bit con un dato valore in un intervallo di RAM
 * fisica, via mmap di /dev/mem (userspace, niente modulo).
 *
 * Serve a localizzare variabili del kernel di cui si conosce il valore ma non
 * il simbolo: con KALLSYMS senza KALLSYMS_ALL i simboli dato sono invisibili.
 * Il caso che lo ha motivato e' il cursore dell'allocatore riservato dei
 * moduli wireless del TG789vac v2: dopo il boot vale la fine del blocco di
 * `wl`, allineata a pagina se l'allocatore allinea dopo ogni allocazione o
 * grezza se allinea prima della successiva (i soli indirizzi di inizio nel
 * log non distinguono i due casi), e si cerca in tutte le forme in cui
 * potrebbe essere salvato (KSEG0, fisico, offset dall'inizio della regione).
 *
 * Build (statico, cosi' non serve libc sul device):
 *     mips-linux-gnu-gcc -O2 -static -o memfind memfind.c
 *
 * Uso:
 *     ./memfind <phys_start_hex> <len_hex> <val_hex> [val_hex ...]
 *
 * Esempio (RAM sotto la regione riservata; fine allineata e grezza del
 * blocco 0x80b9a000 + 4101315, nelle tre forme):
 *     ./memfind 0x2000 0xb8c000 0x80f84000 0x00f84000 0x003f6000 \
 *                               0x80f83443 0x00f83443 0x003f5443
 *
 * Stampa una riga per hit con indirizzo fisico, indirizzo KSEG0 e valore
 * trovato. Esce 0 se c'e' almeno un hit, 1 se nessuno, 2 su errore d'uso.
 *
 * L'intervallo deve essere RAM: mappare I/O qui non ha senso e su alcune
 * periferiche una lettura ha effetti collaterali. Il file e' aperto SENZA
 * O_SYNC apposta: con O_SYNC il kernel MIPS mappa /dev/mem non cached, e una
 * variabile appena scritta potrebbe stare ancora in una linea sporca della
 * D-cache, invisibile dalla DRAM. Con la mappatura cached resta il rischio di
 * alias della cache tra indirizzi virtuali diversi: un'assenza totale di hit
 * va confermata da una scansione fatta in kernel space prima di concludere
 * che la variabile non esiste in quella forma.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define CHUNK (1UL << 20)
#define KSEG0 0x80000000UL
#define MAX_VALS 16

int main(int argc, char **argv)
{
	unsigned long start, len, end, pg, pos;
	uint32_t vals[MAX_VALS];
	int nvals, i, fd, hits = 0;

	if (argc < 4) {
		fprintf(stderr, "uso: %s <phys_start_hex> <len_hex> <val_hex> [val_hex ...]\n",
			argv[0]);
		return 2;
	}

	start = strtoul(argv[1], NULL, 16);
	len = strtoul(argv[2], NULL, 16);
	nvals = argc - 3;
	if (nvals > MAX_VALS) {
		fprintf(stderr, "al massimo %d valori\n", MAX_VALS);
		return 2;
	}
	for (i = 0; i < nvals; i++)
		vals[i] = (uint32_t)strtoul(argv[3 + i], NULL, 16);

	pg = sysconf(_SC_PAGESIZE);
	if (start & 3 || len & 3 || start & (pg - 1)) {
		fprintf(stderr, "inizio allineato a pagina e lunghezza multipla di 4\n");
		return 2;
	}
	end = start + len;

	fd = open("/dev/mem", O_RDONLY);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}

	for (pos = start; pos < end; pos += CHUNK) {
		unsigned long span = end - pos < CHUNK ? end - pos : CHUNK;
		const uint32_t *m;
		unsigned long w, nw = span / 4;

		m = mmap(NULL, span, PROT_READ, MAP_SHARED, fd, pos);
		if (m == MAP_FAILED) {
			fprintf(stderr, "mmap 0x%08lx+0x%lx: ", pos, span);
			perror("");
			close(fd);
			return 1;
		}
		for (w = 0; w < nw; w++) {
			uint32_t v = m[w];

			for (i = 0; i < nvals; i++) {
				if (v != vals[i])
					continue;
				printf("phys 0x%08lx  kseg0 0x%08lx  val 0x%08x\n",
				       pos + w * 4, KSEG0 + pos + w * 4, v);
				hits++;
			}
		}
		munmap((void *)m, span);
	}

	close(fd);
	fprintf(stderr, "%d hit in [0x%08lx, 0x%08lx)\n", hits, start, end);
	return hits ? 0 : 1;
}
