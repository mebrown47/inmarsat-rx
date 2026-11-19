// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// File and stdin input, with SigMF metadata when present.
#define _GNU_SOURCE
#include "input.h"
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef struct { FILE *f; int format, bytes; uint8_t *raw; size_t rawcap; } file_priv_t;

// Sample format names: ours, the usual aliases, and SigMF's (ci16_le and so on; little-endian only).
int iq_format_parse(const char *name) {
	char s[16]; size_t n = 0;
	for (; name[n] && n < sizeof s - 1; n++) s[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
	s[n] = 0;
	if (n > 3 && !strcmp(s + n - 3, "_le")) s[n - 3] = 0;
	if (!strcmp(s, "sc16")) return FMT_CS16;
	if (!strcmp(s, "sc8")) return FMT_CS8;
	if (!strcmp(s, "cu8") || !strcmp(s, "u8")) return FMT_CU8;
	if (!strcmp(s, "cs8") || !strcmp(s, "ci8") || !strcmp(s, "s8")) return FMT_CS8;
	if (!strcmp(s, "cs16") || !strcmp(s, "ci16") || !strcmp(s, "s16")) return FMT_CS16;
	if (!strcmp(s, "cf32") || !strcmp(s, "fc32") || !strcmp(s, "f32")) return FMT_CF32;
	return -1;
}

static const int BYTES[] = {2, 2, 4, 8};

static int file_read(input_t *in, float *iq, int max) {
	file_priv_t *p = in->priv;
	size_t need = (size_t)max * p->bytes;
	if (need > p->rawcap) { p->raw = realloc(p->raw, need); p->rawcap = need; }
	size_t got = fread(p->raw, 1, need, p->f);
	int n = (int)(got / p->bytes);
	switch (p->format) {
	case FMT_CU8: for (int i = 0; i < 2 * n; i++) iq[i] = (p->raw[i] - 127.5f) / 128.f; break;
	case FMT_CS8: for (int i = 0; i < 2 * n; i++) iq[i] = ((int8_t *)p->raw)[i] / 128.f; break;
	case FMT_CS16: for (int i = 0; i < 2 * n; i++) { int16_t v; memcpy(&v, p->raw + 2 * i, 2); iq[i] = v / 32768.f; } break;
	case FMT_CF32: memcpy(iq, p->raw, (size_t)n * 8); break;
	}
	if (n == 0 && ferror(p->f)) return -1;
	return n;
}

static void file_close(input_t *in) {
	file_priv_t *p = in->priv;
	if (p->f && p->f != stdin) fclose(p->f);
	free(p->raw); free(p); free(in);
}

static char *slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *s = malloc(n + 1);
	if (fread(s, 1, n, f) != (size_t)n) n = 0;
	s[n] = 0; fclose(f);
	return s;
}

// Value of "key": in a JSON text (string or number), copied into out.
static int meta_value(const char *json, const char *key, char *out, int size) {
	char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
	const char *p = strstr(json, k);
	if (!p) return 0;
	p += strlen(k);
	while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
	int quoted = *p == '"'; if (quoted) p++;
	int n = 0;
	while (*p && n < size - 1 && (quoted ? *p != '"' : (*p != ',' && *p != '}' && *p != ' ' && *p != '\n'))) out[n++] = *p++;
	out[n] = 0;
	return n > 0;
}

// Rate and centre from a capture's name, as the workbench and the capture scripts write it:
// "i4f3-1546.04M-2.5Msps.ci16" is 1546.04 MHz at 2.5 MS/s. Only what SigMF did not give.
static void name_rate_centre(input_t *in, const char *base) {
	double rate = 0, centre = 0;
	for (const char *s = base; *s; s++) {
		if (!(*s >= '0' && *s <= '9') || (s > base && ((s[-1] >= '0' && s[-1] <= '9') || s[-1] == '.'))) continue;
		char *end; double v = strtod(s, &end);
		if (end == s) continue;
		if (!strncasecmp(end, "Msps", 4) || !strncasecmp(end, "MS/s", 4)) rate = v * 1e6;
		else if (!strncasecmp(end, "ksps", 4)) rate = v * 1e3;
		else if ((end[0] == 'M' || end[0] == 'm') && (end[1] != 's' && end[1] != 'S') && v >= 30) centre = v * 1e6;  // 1546.04M or 1546.04MHz
		s = end - 1;
	}
	if (!in->rate && rate) in->rate = rate;
	if (!in->center_hz && centre) in->center_hz = centre;
	if (rate || centre) fprintf(stderr, "inmarsat-rx: %s: %s%s%s from the file name (-s and -F override)\n", base,
		rate ? "sample rate" : "", rate && centre ? " and " : "", centre ? "centre frequency" : "");
}

static void apply_sigmf(input_t *in, file_priv_t *p, const char *meta_path) {
	char *json = slurp(meta_path), v[64];
	if (!json) return;
	if (meta_value(json, "core:sample_rate", v, sizeof v)) in->rate = atof(v);
	if (meta_value(json, "core:frequency", v, sizeof v)) in->center_hz = atof(v);
	if (p->format < 0 && meta_value(json, "core:datatype", v, sizeof v)) {
		char base[16]; snprintf(base, sizeof base, "%.*s", (int)strcspn(v, "_"), v);
		p->format = iq_format_parse(base);
	}
	if (meta_value(json, "core:datetime", v, sizeof v)) {
		struct tm tm = {0}; double frac = 0;
		char *rest = strptime(v, "%Y-%m-%dT%H:%M:%S", &tm);
		if (rest) {
			if (*rest == '.') frac = atof(rest);
			in->start_ms = ((double)timegm(&tm) + frac) * 1000;
		}
	}
	free(json);
}

input_t *input_open_file(const char *path, int format) {
	input_t *in = calloc(1, sizeof *in);
	file_priv_t *p = calloc(1, sizeof *p);
	in->priv = p; in->read = file_read; in->close = file_close;
	p->format = format;
	char data[1024], meta[1024];
	snprintf(data, sizeof data, "%s", path);
	if (strcmp(path, "-")) {
		size_t n = strlen(path);
		// The folder part ends at the last / (or \ on Windows); an extension is a dot after it.
		const char *slash = strrchr(path, '/');
#ifdef _WIN32
		const char *bs = strrchr(path, '\\');
		if (bs && (!slash || bs > slash)) slash = bs;
#endif
		const char *dot = strrchr(path, '.');
		if (dot && slash && dot < slash) dot = NULL;
		size_t stem = dot ? (size_t)(dot - path) : n;
		if (dot && !strcmp(dot, ".sigmf-meta")) snprintf(data, sizeof data, "%.*s.sigmf-data", (int)stem, path);
		snprintf(meta, sizeof meta, "%.*s.sigmf-meta", (int)stem, path);
		apply_sigmf(in, p, meta);
		// SigMF names the dataset when it is not <stem>.sigmf-data.
		char *json = slurp(meta), ds[512];
		if (json && meta_value(json, "core:dataset", ds, sizeof ds) && dot && !strcmp(dot, ".sigmf-meta")) {
			snprintf(data, sizeof data, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, ds);
		}
		free(json);
		// No SigMF format: the extension (.ci16, .cu8, .cf32 ...); no SigMF rate or centre: the name.
		const char *base = slash ? slash + 1 : path;
		if (p->format < 0 && dot) p->format = iq_format_parse(dot + 1);
		if (!in->rate || !in->center_hz) name_rate_centre(in, base);
		p->f = fopen(data, "rb");
		if (!p->f) { perror(data); free(p); free(in); return NULL; }
	} else { compat_stdin_binary(); p->f = stdin; }
	if (p->format < 0) { fprintf(stderr, "%s: give the sample format with -f (cu8, cs8, cs16 or ci16, cf32)\n", data); fclose(p->f); free(p); free(in); return NULL; }
	p->bytes = BYTES[p->format];
	snprintf(in->name, sizeof in->name, "%.255s", data);
	return in;
}
