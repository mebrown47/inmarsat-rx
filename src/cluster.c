// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared first stage for channels close together in frequency: mix the cluster to 0 Hz, then a
// cascade of decimators. Early stages divide by 4 with a wide transition band (only aliases that
// would land in the passband matter), so they need few taps; the last stage sets the edge.
#include "cluster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Room each side of the outermost channels: the widest signal (Aero-H+, ±5.25 kHz) plus its
// carrier search (±2.5 kHz), with some to spare. The passband ends here.
#define CLUSTER_MARGIN 12e3
#define CLUSTER_MIN_DECIMATION 4

int cluster_plan(const double *f, int n, cluster_t *out, int max) {
	int k = 0;
	for (int i = 0; i < n && k < max;) {
		int j = i;
		while (j + 1 < n && f[j + 1] - f[i] <= CLUSTER_MAX_SPAN) j++;
		out[k++] = (cluster_t){.center = (f[i] + f[j]) / 2, .half = (f[j] - f[i]) / 2};
		i = j + 1;
	}
	return k;
}

void cluster_init(cluster_t *c, double in_rate, double in_center) {
	const double edge = c->half + CLUSTER_MARGIN;   // Passband edge.
	const double target = 3 * edge;                 // Output rate: room for the last stage's transition.
	if (in_rate / target < CLUSTER_MIN_DECIMATION) { c->passthrough = 1; c->rate = in_rate; c->center = in_center; return; }
	double w = -2 * M_PI * (c->center - in_center) / in_rate;
	c->ph_re = 1; c->ph_im = 0; c->step_re = cos(w); c->step_im = sin(w);
	double rate = in_rate;
	c->nstages = 0;
	while (rate / target >= 8 && c->nstages < CLUSTER_STAGES - 1) {
		// Divide by 4. Everything that folds onto the passband lies beyond rate/4 - edge.
		int n; float *h = dsp_lowpass(rate / 8, rate, rate / 4 - 2 * edge, 2047, &n);
		c->stage[c->nstages++] = decim_new(h, n, 4, 2);
		rate /= 4;
	}
	int d = (int)floor(rate / target);
	if (d < 1) d = 1;
	// Last stage: flat to the edge, stopband from rate/d - edge (what folds onto the passband).
	const double stop = rate / d - edge;
	int n; float *h = dsp_lowpass((edge + stop) / 2, rate, stop - edge, 2047, &n);
	c->stage[c->nstages++] = decim_new(h, n, d, 2);
	c->rate = rate / d;
}

void cluster_free(cluster_t *c) {
	for (int i = 0; i < c->nstages; i++) decim_free(c->stage[i]);
	free(c->mixed);
	memset(c->stage, 0, sizeof c->stage); c->nstages = 0; c->mixed = NULL; c->mixedcap = 0;
}

int cluster_find(const cluster_t *c, int n, double freq) {
	for (int i = 0; i < n; i++) {
		if (c[i].passthrough) return i;  // The whole input.
		if (fabs(freq - c[i].center) <= c[i].half + 2e3) return i;
	}
	return -1;
}

void cluster_process(cluster_t *c, const float *iq, int n) {
	if (c->passthrough) { c->out = iq; c->n = n; return; }
	c->mixed = dsp_reserve(c->mixed, &c->mixedcap, (size_t)2 * n, sizeof(float));
	double pr = c->ph_re, pi = c->ph_im;
	const double sr = c->step_re, si = c->step_im;
	for (int i = 0; i < n; i++) {
		double re = iq[2 * i], im = iq[2 * i + 1];
		c->mixed[2 * i] = (float)(re * pr - im * pi);
		c->mixed[2 * i + 1] = (float)(re * pi + im * pr);
		double t = pr * sr - pi * si; pi = pr * si + pi * sr; pr = t;
	}
	double m = 1 / hypot(pr, pi);  // Keep the phasor on the unit circle.
	c->ph_re = pr * m; c->ph_im = pi * m;
	const float *x = c->mixed;
	for (int s = 0; s < c->nstages; s++) { n = decim_process(c->stage[s], x, n); x = c->stage[s]->out; }
	c->out = x; c->n = n;
}
