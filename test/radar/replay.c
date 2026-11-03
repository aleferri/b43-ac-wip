// SPDX-License-Identifier: GPL-2.0
/*
 * Replay radar FIFO reads through b43/radar_ac.c.
 *
 *   replay FILE...
 *
 * A file holds polls: a line that does not start with "0 " or "1 " opens a
 * poll (the runs of router-data/rt-ac66u-b1/radar-fifo.zip have the two fill
 * levels there), and each "<fifo> <word>" line after it is one word read from
 * that FIFO, in hex. Prints, per file, the type matched and the poll it was
 * matched in, or "none".
 */
#include <stdio.h>
#include <string.h>

#include "radar_ac.h"

static struct b43_radar radar;

struct fifo_words {
	u16 w[4];
	unsigned int n;
};

static int replay(const char *path)
{
	struct fifo_words fw[2] = { 0 };
	const struct b43_radar_type *seen = NULL;
	unsigned int poll = 0, hits = 0;
	char line[128];
	bool open = false;
	FILE *f = fopen(path, "r");

	if (!f) {
		perror(path);
		return -1;
	}
	b43_radar_reset(&radar);

	for (;;) {
		bool eof = !fgets(line, sizeof(line), f);
		unsigned int fifo, word;

		if (!eof && (line[0] == '0' || line[0] == '1') &&
		    line[1] == ' ' &&
		    sscanf(line, "%u %x", &fifo, &word) == 2) {
			fw[fifo].w[fw[fifo].n++] = word;
			if (fw[fifo].n == 4) {
				b43_radar_entry(&radar, fifo, fw[fifo].w);
				fw[fifo].n = 0;
			}
			continue;
		}
		if (open) {
			const struct b43_radar_type *t = b43_radar_match(&radar);

			if (t) {
				if (!hits++)
					seen = t;
				printf("  poll %u: %s\n", poll,
				       b43_radar_type_name(t));
			}
			poll++;
		}
		if (eof)
			break;
		open = true;
		fw[0].n = 0;
		fw[1].n = 0;
	}
	fclose(f);
	printf("%s: %s (%u in %u polls)\n", path,
	       seen ? b43_radar_type_name(seen) : "none", hits, poll);
	return hits;
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (replay(argv[i]) < 0)
			return 2;
	return 0;
}
