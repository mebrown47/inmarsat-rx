// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// JSONL archive, as vdl2-sniffer's and iridium-sniffer's: same directory layout and time rules.
#include "archive.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char dir[512];
static FILE *f;
static int day;

int archive_open(const char *d) {
	if (mkdir(d, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "archive: cannot create directory %s: %s\n", d, strerror(errno));
		return -1;
	}
	snprintf(dir, sizeof dir, "%s", d);
	return 0;
}

int archive_enabled(void) { return dir[0] != 0; }

void archive_write(double t_ms, const char *line) {
	if (!dir[0]) return;
	time_t t = (time_t)(t_ms / 1000);
	struct tm g;
	gmtime_r(&t, &g);
	int d = (g.tm_year + 1900) * 10000 + (g.tm_mon + 1) * 100 + g.tm_mday;
	pthread_mutex_lock(&lock);
	if (d != day || !f) {
		if (f) fclose(f);
		char path[700];
		snprintf(path, sizeof path, "%s/inmarsat-%08d.jsonl", dir, d);
		f = fopen(path, "a");
		if (!f) fprintf(stderr, "archive: cannot open %s: %s\n", path, strerror(errno));
		day = d;
	}
	if (f) { fputs(line, f); fputc('\n', f); fflush(f); }
	pthread_mutex_unlock(&lock);
}

void archive_close(void) {
	pthread_mutex_lock(&lock);
	if (f) fclose(f);
	f = NULL;
	pthread_mutex_unlock(&lock);
}
