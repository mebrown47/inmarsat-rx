// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Carrier discovery. One Hann-windowed FFT (~40 Hz bins) per second of input, averaged over the
// period; the floor is the 20th percentile of each 10 kHz segment, interpolated; a carrier is a run
// of bins more than 5 dB above it, ignoring the band edges and the centre (a DC spike on many SDRs).
#include "scan.h"
#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLOOR_SEG_HZ 10000.0
#define ABOVE_DB 5.0
#define EDGE 0.4          // Usable band: ±EDGE × rate.
#define DC_HZ 2000.0

struct scan {
	double rate, centre, period; int n;
	float *buf; int fill; double since;   // Samples gathered for the next FFT; seconds since the last.
	double *acc, *re, *im, *win; int ffts, ready;
	double elapsed;
};

scan_t *scan_new(double rate, double centre, double period_s) {
	scan_t *s = calloc(1, sizeof *s);
	s->rate = rate; s->centre = centre; s->period = period_s;
	s->n = 1; while (s->n < rate / 40) s->n <<= 1;
	s->buf = malloc((size_t)2 * s->n * sizeof(float));
	s->acc = calloc(s->n, sizeof(double)); s->re = malloc(s->n * sizeof(double)); s->im = malloc(s->n * sizeof(double));
	s->win = malloc(s->n * sizeof(double));
	for (int i = 0; i < s->n; i++) s->win[i] = 0.5 - 0.5 * cos(2 * M_PI * i / (s->n - 1));
	s->since = 1;  // First FFT right away.
	return s;
}

void scan_free(scan_t *s) {
	if (!s) return;
	free(s->buf); free(s->acc); free(s->re); free(s->im); free(s->win); free(s);
}

void scan_feed(scan_t *s, const float *iq, int n) {
	double dt = n / s->rate;
	s->elapsed += dt;
	if (s->since >= 1 || s->fill) {
		int take = s->n - s->fill < n ? s->n - s->fill : n;
		memcpy(s->buf + 2 * s->fill, iq, (size_t)2 * take * sizeof(float));
		s->fill += take;
		if (s->fill == s->n) {
			for (int i = 0; i < s->n; i++) { s->re[i] = s->buf[2 * i] * s->win[i]; s->im[i] = s->buf[2 * i + 1] * s->win[i]; }
			dsp_fft(s->re, s->im, s->n);
			for (int i = 0; i < s->n; i++) { int k = (i + s->n / 2) % s->n; s->acc[i] += s->re[k] * s->re[k] + s->im[k] * s->im[k]; }
			s->ffts++; s->fill = 0; s->since = 0;
		}
	} else s->since += dt;
	if (s->elapsed >= s->period && s->ffts) s->ready = 1;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int cmp_snr(const void *a, const void *b) { double x = ((const carrier_t *)a)->snr_db, y = ((const carrier_t *)b)->snr_db; return x > y ? -1 : x < y; }

int scan_report(scan_t *s, carrier_t *out, int max) {
	if (!s->ready) return -1;
	const int n = s->n;
	const double bin = s->rate / n;
	double *db = malloc(n * sizeof *db), *sm = malloc(n * sizeof *sm), *fl = malloc(n * sizeof *fl);
	for (int i = 0; i < n; i++) db[i] = 10 * log10(s->acc[i] / s->ffts + 1e-30);
	// Smooth over ~200 Hz so a carrier's spectrum reads as one run.
	const int w = (int)(100 / bin) > 0 ? (int)(100 / bin) : 1;
	for (int i = 0; i < n; i++) {
		double a = 0; int c = 0;
		for (int k = i - w; k <= i + w; k++) if (k >= 0 && k < n) { a += db[k]; c++; }
		sm[i] = a / c;
	}
	// Floor: 20th percentile per segment, linearly interpolated between segment centres.
	const int seg = (int)(FLOOR_SEG_HZ / bin), nseg = n / seg;
	double *sf = malloc(nseg * sizeof *sf), *tmp = malloc(seg * sizeof *tmp);
	for (int g = 0; g < nseg; g++) { memcpy(tmp, sm + g * seg, seg * sizeof *tmp); qsort(tmp, seg, sizeof *tmp, cmp_d); sf[g] = tmp[seg / 5]; }
	for (int i = 0; i < n; i++) {
		double x = (double)i / seg - 0.5; int g = (int)floor(x);
		if (g < 0) fl[i] = sf[0]; else if (g >= nseg - 1) fl[i] = sf[nseg - 1];
		else fl[i] = sf[g] + (sf[g + 1] - sf[g]) * (x - g);
	}
	int found = 0;
	const int lo = (int)(n / 2 - EDGE * s->rate / bin), hi = (int)(n / 2 + EDGE * s->rate / bin);
	for (int i = lo > 0 ? lo : 0; i < hi && i < n && found < max;) {
		if (sm[i] - fl[i] < ABOVE_DB) { i++; continue; }
		int j = i, gap = 0;
		double p = 0, pf = 0, peak = 0;
		while (j < hi && j < n && gap <= 2) {
			if (sm[j] - fl[j] >= ABOVE_DB) gap = 0; else gap++;
			double lin = pow(10, db[j] / 10) - pow(10, fl[j] / 10);
			if (lin > 0) { p += lin; pf += lin * j; }
			if (sm[j] - fl[j] > peak) peak = sm[j] - fl[j];
			j++;
		}
		int end = j - gap;
		double f = s->centre + ((p > 0 ? pf / p : (i + end) / 2.0) - n / 2) * bin;
		if (fabs(f - s->centre) > DC_HZ) out[found++] = (carrier_t){f, (end - i) * bin, peak};
		i = j;
	}
	qsort(out, found, sizeof *out, cmp_snr);
	memset(s->acc, 0, n * sizeof *s->acc);
	s->ffts = 0; s->ready = 0; s->elapsed = 0;
	free(db); free(sm); free(fl); free(sf); free(tmp);
	return found;
}
