// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared first stage: channels close together in frequency form a cluster, and the cluster
// narrows the input once (mix to its centre, decimate) for all of them. At 10 MS/s with channels
// in five groups this replaces ~30 full-rate front ends with five.
#ifndef CLUSTER_H
#define CLUSTER_H

#include "demod.h"

#define CLUSTER_MAX_SPAN 250e3   // Channels further apart than this go in separate clusters.

#define CLUSTER_STAGES 6

typedef struct {
	double center, half;         // Centre and half-span of the channels it was made for.
	double rate;                 // Output rate (the input rate when it passes the input through).
	int passthrough;             // Too little to gain: channels take the input as it is.
	double ph_re, ph_im, step_re, step_im;          // Mixer phasor.
	decim_t *stage[CLUSTER_STAGES]; int nstages;   // ÷4 stages, then one to the output rate.
	float *mixed; size_t mixedcap;
	const float *out; int n;     // This block's output.
} cluster_t;

// Groups the sorted frequencies into clusters (at most max); returns how many.
int cluster_plan(const double *sorted_freqs, int n, cluster_t *out, int max);
// Builds the cluster's filters for an input at in_rate around in_center.
void cluster_init(cluster_t *c, double in_rate, double in_center);
void cluster_free(cluster_t *c);
// Index of a cluster whose passband holds a channel at freq, or -1.
int cluster_find(const cluster_t *c, int n, double freq);
// Narrows this block; afterwards c->out / c->n hold the result (the input itself when passthrough).
void cluster_process(cluster_t *c, const float *iq, int n);

#endif
