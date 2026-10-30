// SPDX-License-Identifier: GPL-2.0
/*
 * wlx: the wl ioctls DD-WRT's cut-down `wl` lacks, for the capture scripts (see README.md).
 * usage: wlx IF up|down|isup|revinfo|ap [0|1]|ssid [NAME]|bss up|down|iovar NAME [INT]
 * Output mimics Broadcom's wl where the scripts parse it (revinfo "deviceid 0x....", isup "0"/"1").
 * Ioctl numbers/structs from DD-WRT src/include.v24/wlioctl.h (stable across wl versions).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/sockios.h>

#define WLC_UP          2
#define WLC_DOWN        3
#define WLC_SET_SSID    26
#define WLC_GET_REVINFO 98
#define WLC_GET_AP      117
#define WLC_SET_AP      118
#define WLC_GET_UP      162
#define WLC_GET_VAR     262
#define WLC_SET_VAR     263

typedef struct {
	unsigned int cmd;
	void *buf;
	unsigned int len;
	unsigned char set;
	unsigned int used;
	unsigned int needed;
} wl_ioctl_t;

static const char *ifn;

static int wl_ioctl(int cmd, void *buf, int len, int set)
{
	struct ifreq ifr;
	wl_ioctl_t ioc = { .cmd = cmd, .buf = buf, .len = len, .set = set };
	int s = socket(AF_INET, SOCK_DGRAM, 0), r;

	if (s < 0) { perror("socket"); exit(2); }
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
	ifr.ifr_data = (void *)&ioc;
	r = ioctl(s, SIOCDEVPRIVATE, &ifr);
	close(s);
	return r;
}

static void die(const char *what) { perror(what); exit(1); }

/* stdout through write(2): the static libc of this toolchain drops printf output on stdout */
#include <stdarg.h>
static void out(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		write(1, b, n < (int)sizeof(b) ? n : (int)sizeof(b) - 1);
}

static int iovar_int(const char *name, int set, int val, int bsscfg, int *out)
{
	char buf[64];
	int n = strlen(name) + 1, off = n;

	if (n > 40) return -1;
	memset(buf, 0, sizeof(buf));
	memcpy(buf, name, n);
	if (bsscfg >= 0) { memcpy(buf + off, &bsscfg, 4); off += 4; }
	if (set) {
		memcpy(buf + off, &val, 4);
		return wl_ioctl(WLC_SET_VAR, buf, off + 4, 1);
	}
	if (wl_ioctl(WLC_GET_VAR, buf, sizeof(buf), 0) < 0) return -1;
	memcpy(out, buf, 4);
	return 0;
}

int main(int argc, char **argv)
{
	int v;

	if (argc < 3) { fprintf(stderr, "usage: wlx IF up|down|isup|revinfo|ap [v]|ssid [s]|bss up|down|iovar N [v]\n"); return 2; }
	ifn = argv[1];
	const char *c = argv[2];
	if (!strcmp(c, "up")) { if (wl_ioctl(WLC_UP, NULL, 0, 1) < 0) die("WLC_UP"); }
	else if (!strcmp(c, "down")) { if (wl_ioctl(WLC_DOWN, NULL, 0, 1) < 0) die("WLC_DOWN"); }
	else if (!strcmp(c, "isup")) { v = 0; if (wl_ioctl(WLC_GET_UP, &v, 4, 0) < 0) die("WLC_GET_UP"); out("%d\n", v); }
	else if (!strcmp(c, "revinfo")) {
		unsigned int r[16] = { 0 };
		if (wl_ioctl(WLC_GET_REVINFO, r, sizeof(r), 0) < 0) die("WLC_GET_REVINFO");
		out("vendorid 0x%x\ndeviceid 0x%x\nradiorev 0x%x\nchipnum 0x%x\nchiprev 0x%x\ncorerev 0x%x\n"
		       "boardid 0x%x\nboardvendor 0x%x\nboardrev 0x%x\ndriverrev 0x%x\nucoderev 0x%x\nbus 0x%x\n",
		       r[0], r[1], r[2], r[11], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10]);
	}
	else if (!strcmp(c, "ap")) {
		if (argc > 3) { v = atoi(argv[3]); if (wl_ioctl(WLC_SET_AP, &v, 4, 1) < 0) die("WLC_SET_AP"); }
		else { v = 0; if (wl_ioctl(WLC_GET_AP, &v, 4, 0) < 0) die("WLC_GET_AP"); out("%d\n", v); }
	}
	else if (!strcmp(c, "ssid") && argc > 3) {
		struct { unsigned int len; unsigned char ssid[32]; } s = { 0 };
		s.len = strlen(argv[3]); if (s.len > 32) s.len = 32;
		memcpy(s.ssid, argv[3], s.len);
		if (wl_ioctl(WLC_SET_SSID, &s, sizeof(s), 1) < 0) die("WLC_SET_SSID");
	}
	else if (!strcmp(c, "bss") && argc > 3) {
		/* "bss" iovar on bsscfg 0: buf = "bss\0" + int32 bsscfg idx + int32 val (as DD-WRT's wlconf.c) */
		v = !strcmp(argv[3], "up");
		char buf[16] = "bss"; int idx = 0;
		memcpy(buf + 4, &idx, 4); memcpy(buf + 8, &v, 4);
		if (wl_ioctl(WLC_SET_VAR, buf, 12, 1) < 0) die("bss");
	}
	else if (!strcmp(c, "iovar") && argc > 3) {
		if (argc > 4) { if (iovar_int(argv[3], 1, atoi(argv[4]), -1, NULL) < 0) die(argv[3]); }
		else { if (iovar_int(argv[3], 0, 0, -1, &v) < 0) die(argv[3]); out("%d\n", v); }
	}
	else { fprintf(stderr, "wlx: unknown command %s\n", c); return 2; }
	return 0;
}
