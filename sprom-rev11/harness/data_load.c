/*
 * data_load.c — see data_load.h.
 */

#include "data_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int srom_load(const char *path, u16 *buf, size_t buf_words)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "srom_load: cannot open %s\n", path);
		return -1;
	}

	memset(buf, 0, buf_words * sizeof(u16));

	int max_idx = -1;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		/* Find "srom[" — bail on anything that is not a data line. */
		const char *p = strstr(line, "srom[");
		if (!p)
			continue;
		p += 5;
		char *end;
		long start = strtol(p, &end, 10);
		if (end == p || *end != ']' || start < 0 || (size_t)start >= buf_words) {
			fprintf(stderr, "srom_load: bad index in line: %s", line);
			fclose(f);
			return -1;
		}
		p = end + 1;
		while (*p == ':' || isspace((unsigned char)*p))
			p++;

		/* Read consecutive 0xWWWW tokens until end of line. */
		size_t idx = (size_t)start;
		while (*p) {
			while (isspace((unsigned char)*p))
				p++;
			if (!*p)
				break;
			if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) {
				fprintf(stderr,
					"srom_load: expected 0xNNNN at offset %zd, got: %s",
					p - line, line);
				fclose(f);
				return -1;
			}
			char *tend;
			unsigned long w = strtoul(p, &tend, 16);
			if (tend == p) {
				fprintf(stderr, "srom_load: failed to parse hex word: %s", line);
				fclose(f);
				return -1;
			}
			if (idx >= buf_words) {
				fprintf(stderr, "srom_load: word index %zu past buffer\n", idx);
				fclose(f);
				return -1;
			}
			buf[idx] = (u16)w;
			if ((int)idx > max_idx)
				max_idx = (int)idx;
			idx++;
			p = tend;
		}
	}

	fclose(f);
	return max_idx + 1;
}

static char *strip(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		e--;
	*e = 0;
	return s;
}

int nvram_load(const char *path, struct nvram *out)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "nvram_load: cannot open %s\n", path);
		return -1;
	}
	out->n = 0;

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = strip(line);
		char *v = strip(eq + 1);
		if (!*k)
			continue;
		if (out->n >= sizeof(out->items) / sizeof(out->items[0])) {
			fprintf(stderr, "nvram_load: too many entries\n");
			fclose(f);
			return -1;
		}
		struct nvram_kv *kv = &out->items[out->n++];
		snprintf(kv->key, sizeof(kv->key), "%s", k);
		snprintf(kv->val, sizeof(kv->val), "%s", v);
	}
	fclose(f);
	return 0;
}

const char *nvram_get(const struct nvram *nv, const char *key)
{
	for (size_t i = 0; i < nv->n; i++)
		if (strcmp(nv->items[i].key, key) == 0)
			return nv->items[i].val;
	return NULL;
}
