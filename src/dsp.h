// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// DSP building blocks: filter design, decimating FIR, FFT. Samples are interleaved I/Q floats.
#ifndef DSP_H
#define DSP_H

#include <stddef.h>

// Blackman-windowed sinc low-pass, unit DC gain; transition width is about 5.5*rate/taps.
float *dsp_lowpass(double cutoff, double rate, double transition, int max_taps, int *ntaps);
// Root-raised-cosine (roll-off alpha, sps samples per symbol, span symbols each side), unit DC gain.
float *dsp_rrc(double alpha, double sps, double span, int *ntaps);

// FIR filter that computes only every factor-th output; channels: 2 for complex, 1 for real.
typedef struct {
	float *h; int ntaps, factor, ch;
	float *buf; size_t bufcap;       // History (ntaps-1 frames) followed by the current input.
	int offset;
	float *out; size_t outcap;
} decim_t;
decim_t *decim_new(float *taps, int ntaps, int factor, int channels);  // Takes ownership of taps.
void decim_free(decim_t *d);
// Filters n frames; returns the number of output frames, written to d->out.
int decim_process(decim_t *d, const float *in, int n);

// Streaming complex FIR, one sample in, one out (for filters inside a tracking loop).
typedef struct { float *h, *line; int ntaps, pos; } fir_t;
fir_t *fir_new(float *taps, int ntaps);  // Takes ownership of taps.
void fir_free(fir_t *f);
static inline void fir_push(fir_t *f, float re, float im, float *ore, float *oim) {
	// The delay line is stored twice so the newest ntaps samples are always contiguous.
	const int n = f->ntaps;
	f->pos = f->pos == 0 ? n - 1 : f->pos - 1;
	f->line[2 * f->pos] = f->line[2 * (f->pos + n)] = re;
	f->line[2 * f->pos + 1] = f->line[2 * (f->pos + n) + 1] = im;
	const float *x = f->line + 2 * f->pos, *h = f->h;
	float a = 0, b = 0;
	for (int k = 0; k < n; k++) { a += h[k] * x[2 * k]; b += h[k] * x[2 * k + 1]; }
	*ore = a; *oim = b;
}

// In-place complex FFT of n = 2^k points (re, im arrays).
void dsp_fft(double *re, double *im, int n);
// Power spectrum of n complex samples with a Hann window, FFT-shifted (bin n/2 = 0 Hz).
void dsp_power_spectrum(const float *iq, int n, double *power);

// Grows *buf to hold at least need elements of size elem.
void *dsp_reserve(void *buf, size_t *cap, size_t need, size_t elem);

#endif
