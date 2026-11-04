// SPDX-License-Identifier: GPL-2.0
/*
 * Replay radar FIFO reads through b43/radar_ac.c.
 *
 *   replay [-w 20|40|80] [-r all|fcc|etsi] FILE...
 *
 * A file holds polls: a line that does not start with "0 " or "1 " opens a
 * poll (the runs of router-data/rt-ac66u-b1/radar-fifo.zip have the two fill
 * levels there), and each "<fifo> <word>" line after it is one word read from
 * that FIFO, in hex. Prints, per file, the type matched and the poll it was
 * matched in, or "none". A line "radar <fifo> <w0> <w1> <w2> <w3> ..." (the
 * test log of radar-widths.zip) is one entry of the open poll, a poll opened
 * by the first. -r gives the DFS region whose types are matched, all by
 * default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radar_ac.h"

static struct b43_radar radar;

struct fifo_words {
	u16 w[4];
	unsigned int n;
};

static enum b43_radar_region region = B43_RADAR_ALL;

static int replay(const char *path, unsigned int shift)
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
	radar.shift = shift;
	radar.region = region;

	for (;;) {
		bool eof = !fgets(line, sizeof(line), f);
		unsigned int fifo, word, e[4];

		/* "radar <fifo> <w0> <w1> <w2> <w3> ...": one entry a line */
		if (!eof && !strncmp(line, "radar ", 6) &&
		    sscanf(line + 6, "%u %x %x %x %x", &fifo, &e[0], &e[1],
			   &e[2], &e[3]) == 5 && fifo < 2) {
			u16 w[4] = { e[0], e[1], e[2], e[3] };

			if (!open) {
				open = true;
				fw[0].n = fw[1].n = 0;
			}
			b43_radar_entry(&radar, fifo, w);
			continue;
		}

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
	unsigned int shift = 0;
	int i;

	for (i = 1; i < argc; i++) {
		/* -w 20|40|80: the channel width the files were read at */
		if (!strcmp(argv[i], "-w") && i + 1 < argc) {
			unsigned int mhz = atoi(argv[++i]);

			shift = mhz == 20 ? 2 : mhz == 40 ? 1 : 0;
			continue;
		}
		/* -r all|fcc|etsi: the DFS region */
		if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			i++;
			region = !strcmp(argv[i], "fcc") ? B43_RADAR_FCC :
				 !strcmp(argv[i], "etsi") ? B43_RADAR_ETSI :
				 B43_RADAR_ALL;
			continue;
		}
		if (replay(argv[i], shift) < 0)
			return 2;
	}
	return 0;
}
