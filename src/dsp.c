// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// DSP building blocks, ported from the SDR Workbench (usb/demod.js, usb/psk.js, usb/spectrum.js).
#include "dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void *dsp_reserve(void *buf, size_t *cap, size_t need, size_t elem) {
	if (need <= *cap) return buf;
	size_t n = *cap ? *cap : 1024;
	while (n < need) n *= 2;
	void *p = realloc(buf, n * elem);
	if (!p) abort();
	*cap = n;
	return p;
}

float *dsp_lowpass(double cutoff, double rate, double transition, int max_taps, int *ntaps) {
	int n = (int)ceil(5.5 * rate / transition) | 1;
	if (n > max_taps) n = max_taps | 1;
	float *h = malloc(n * sizeof *h);
	double m = (n - 1) / 2.0, fc = cutoff / rate, sum = 0;
	for (int i = 0; i < n; i++) {
		double x = i - m, sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
		double w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));
		h[i] = (float)(sinc * w); sum += sinc * w;
	}
	for (int i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
	*ntaps = n;
	return h;
}

float *dsp_rrc(double alpha, double sps, double span, int *ntaps) {
	int n = 2 * (int)lround(span * sps) + 1;
	float *h = malloc(n * sizeof *h);
	double m = (n - 1) / 2.0, sum = 0;
	for (int i = 0; i < n; i++) {
		double t = (i - m) / sps, v;
		if (fabs(t) < 1e-9) v = 1 - alpha + 4 * alpha / M_PI;
		else if (fabs(fabs(t) - 1 / (4 * alpha)) < 1e-9)
			v = alpha / M_SQRT2 * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
		else v = (sin(M_PI * t * (1 - alpha)) + 4 * alpha * t * cos(M_PI * t * (1 + alpha))) /
			(M_PI * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
		h[i] = (float)v; sum += v;
	}
	for (int i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
	*ntaps = n;
	return h;
}

decim_t *decim_new(float *taps, int ntaps, int factor, int channels) {
	decim_t *d = calloc(1, sizeof *d);
	d->h = taps; d->ntaps = ntaps; d->factor = factor; d->ch = channels;
	d->buf = dsp_reserve(NULL, &d->bufcap, (size_t)(ntaps - 1) * channels + 1, sizeof(float));
	memset(d->buf, 0, (size_t)(ntaps - 1) * channels * sizeof(float));
	return d;
}

void decim_free(decim_t *d) {
	if (!d) return;
	free(d->h); free(d->buf); free(d->out); free(d);
}

int decim_process(decim_t *d, const float *in, int n) {
	const int L = d->ntaps - 1, ch = d->ch, factor = d->factor;
	const size_t hist = (size_t)L * ch;
	d->buf = dsp_reserve(d->buf, &d->bufcap, hist + (size_t)n * ch, sizeof(float));
	memcpy(d->buf + hist, in, (size_t)n * ch * sizeof(float));
	int count = n - d->offset > 0 ? (n - d->offset + factor - 1) / factor : 0;
	d->out = dsp_reserve(d->out, &d->outcap, (size_t)count * ch + 1, sizeof(float));
	const float *h = d->h, *buf = d->buf;
	float *out = d->out;
	int p = L + d->offset;
	for (int o = 0; o < count; o++, p += factor) {
		if (ch == 2) {
			// Newest sample at p; taps run backwards in time. The buffer is read oldest first so
			// the compiler can vectorise: y = sum h[k] * x[p-k] = sum h[L-j] * x[p-L+j].
			const float *x = buf + 2 * (p - L);
			float re = 0, im = 0;
			for (int j = 0; j <= L; j++) { float c = h[L - j]; re += c * x[2 * j]; im += c * x[2 * j + 1]; }
			out[2 * o] = re; out[2 * o + 1] = im;
		} else {
			const float *x = buf + (p - L);
			float y = 0;
			for (int j = 0; j <= L; j++) y += h[L - j] * x[j];
			out[o] = y;
		}
	}
	d->offset = p - (L + n);
	memmove(d->buf, d->buf + (size_t)n * ch, hist * sizeof(float));
	return count;
}

fir_t *fir_new(float *taps, int ntaps) {
	fir_t *f = calloc(1, sizeof *f);
	f->h = taps; f->ntaps = ntaps;
	f->line = calloc((size_t)4 * ntaps, sizeof(float));
	return f;
}

void fir_free(fir_t *f) { if (f) { free(f->h); free(f->line); free(f); } }

void dsp_fft(double *re, double *im, int n) {
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
	}
	for (int len = 2; len <= n; len <<= 1) {
		double ang = -2 * M_PI / len;
		for (int j = 0; j < len / 2; j++) {
			double c = cos(ang * j), s = sin(ang * j);
			for (int start = 0; start < n; start += len) {
				int a = start + j, b = a + len / 2;
				double r = re[b] * c - im[b] * s, q = re[b] * s + im[b] * c;
				re[b] = re[a] - r; im[b] = im[a] - q; re[a] += r; im[a] += q;
			}
		}
	}
}

void dsp_power_spectrum(const float *iq, int n, double *power) {
	double *re = malloc(n * sizeof *re), *im = malloc(n * sizeof *im);
	for (int i = 0; i < n; i++) {
		double w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
		re[i] = iq[2 * i] * w; im[i] = iq[2 * i + 1] * w;
	}
	dsp_fft(re, im, n);
	for (int i = 0; i < n; i++) { int k = (i + n / 2) % n; power[i] = re[k] * re[k] + im[k] * im[k]; }
	free(re); free(im);
}
