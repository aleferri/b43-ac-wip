// SPDX-License-Identifier: GPL-2.0
/*
 * The vchipd process (userspace code, like trace_out.c). One line per
 * request, one reply each, as vchipd.py documents; a missing or malformed
 * reply aborts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "chip_link.h"

static const char *board_args = "";
static FILE *to_chip, *from_chip;
static pid_t chip_pid;
static int chip_state;			/* 0 to start, 1 running, -1 absent */
static unsigned int chip_irq;

static void chip_stop(void)
{
	char line[64];

	if (chip_state != 1)
		return;
	fputs("Q\n", to_chip);
	fflush(to_chip);
	if (!fgets(line, sizeof(line), from_chip))
		fprintf(stderr, "vchipd: no reply to Q\n");
	fclose(to_chip);
	fclose(from_chip);
	waitpid(chip_pid, NULL, 0);
	chip_state = -1;
}

void b43_chip_board(const char *args)
{
	board_args = args;
}

static void chip_start(void)
{
	const char *vchip = getenv("B43_VCHIP");
	const char *extra = getenv("B43_VCHIP_ARGS");
	static char cmd[1024];
	int down[2], up[2];

	chip_state = -1;
	if (!vchip || !*vchip)
		return;
	if (snprintf(cmd, sizeof(cmd), "%s %s %s", vchip, board_args,
		     extra ? extra : "") >= (int)sizeof(cmd)) {
		fprintf(stderr, "vchipd: command too long\n");
		exit(1);
	}
	if (pipe(down) || pipe(up)) {
		perror("vchipd: pipe");
		exit(1);
	}
	chip_pid = fork();
	if (chip_pid < 0) {
		perror("vchipd: fork");
		exit(1);
	}
	if (chip_pid == 0) {
		dup2(down[0], 0);
		dup2(up[1], 1);
		close(down[0]);
		close(down[1]);
		close(up[0]);
		close(up[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		perror("vchipd: exec");
		_exit(127);
	}
	close(down[0]);
	close(up[1]);
	to_chip = fdopen(down[1], "w");
	from_chip = fdopen(up[0], "r");
	chip_state = 1;
	atexit(chip_stop);
}

int b43_chip_active(void)
{
	if (!chip_state)
		chip_start();
	return chip_state == 1;
}

/* A request and its reply; returns the value of a V. */
static unsigned int chip_request(const char *req)
{
	char line[64], kind;
	unsigned int a = 0, b = 0;
	int n;

	fputs(req, to_chip);
	fflush(to_chip);
	if (!fgets(line, sizeof(line), from_chip)) {
		fprintf(stderr, "vchipd: no reply to %s", req);
		exit(1);
	}
	n = sscanf(line, "%c %x %x", &kind, &a, &b);
	if (kind == 'V' && n == 3) {
		chip_irq = b;
		return a;
	}
	if (kind == 'K' && n == 2) {
		chip_irq = a;
		return 0;
	}
	fprintf(stderr, "vchipd: unexpected reply to %s: %s", req, line);
	exit(1);
}

unsigned int b43_chip_read(unsigned short off, int width)
{
	char req[48];

	snprintf(req, sizeof(req), "R %x %x\n", off, width);
	return chip_request(req);
}

void b43_chip_write(unsigned short off, unsigned int val, int width)
{
	char req[48];

	snprintf(req, sizeof(req), "W %x %x %x\n", off, val, width);
	chip_request(req);
}

void b43_chip_time(unsigned long us)
{
	char req[48];

	if (!us)
		return;
	snprintf(req, sizeof(req), "T %lx\n", us);
	chip_request(req);
}

unsigned int b43_chip_irq(void)
{
	return chip_irq;
}
