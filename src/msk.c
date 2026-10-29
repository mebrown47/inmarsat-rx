// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Non-coherent MSK demodulator for Inmarsat Aero-L (600/1200 bit/s), ported from the SDR
// Workbench (usb/msk.js). Chain: front end to ~16 samples/bit -> coarse carrier acquisition
// (squared signal: two lines one bit rate apart) -> NCO -> channel filter -> FM discriminator ->
// integrate over each bit with Gardner timing recovery -> AFC from the integrated phase.
//
// MSK advances the carrier phase by ±π/2 per bit, so each bit is the sign of the phase change
// across it (positive = 1, the higher tone). This needs no carrier phase lock: a coherent
// OQPSK-style loop at 300 Bd slipped on oscillator phase noise in tests on a live I-4 F3
// capture, while this decoded every frame with no channel bit errors.
#include "demod.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPB_TARGET 16
#define ACQ_FFT 4096
#define ACQ_SECONDS 1.0
#define LOCK_EVM 0.3       // Mean squared distance of soft bits from ±1 while locked.
#define LOCK_SUSTAIN 200   // Bits below LOCK_EVM before lock is reported.

struct msk {
	double baud, rate, spb; int reacquire;
	frontend_t *fe; fir_t *channel;
	double t_alpha, t_beta;
	double phase, freq; int acquired;
	float *acq; size_t acqcap; int acqlen;
	double *cum; size_t cumcap; int cumlen;  // Running sum of the discriminator: unwrapped phase.
	double t, omega, prev_re, prev_im, phase_sum, prev_y, evm;
	long good_run, unlocked_bits;
	float *out; size_t outcap;
};

static void msk_reset(msk_t *m) {
	m->phase = 0; m->freq = 0; m->acquired = 0; m->acqlen = 0;
	m->cumlen = 0; m->t = 2 * m->spb; m->omega = m->spb;
	m->prev_re = 1; m->prev_im = 0; m->phase_sum = 0; m->prev_y = 0;
	m->evm = 1; m->good_run = 0; m->unlocked_bits = 0;
}

void msk_restart(msk_t *m) { msk_reset(m); }

msk_t *msk_new(double bit_rate, double in_rate, double offset_hz, int reacquire) {
	msk_t *m = calloc(1, sizeof *m);
	m->baud = bit_rate; m->reacquire = reacquire;
	double target = SPB_TARGET * bit_rate, pass = MSK_SEARCH_HZ + bit_rate;
	m->fe = frontend_new(in_rate, offset_hz, target, pass, pass, bit_rate);
	m->rate = m->fe->rate; m->spb = m->rate / bit_rate;
	int n;
	// Main lobe of the MSK spectrum is ±0.75 bit rate.
	float *h = dsp_lowpass(0.8 * bit_rate, m->rate, 0.4 * bit_rate, 2047, &n);
	m->channel = fir_new(h, n);
	const double zeta = 0.707, bn = 0.01, theta = bn / (zeta + 0.25 / zeta), d = 1 + 2 * zeta * theta + theta * theta;
	m->t_alpha = 4 * zeta * theta / d; m->t_beta = 4 * theta * theta / d;
	msk_reset(m);
	return m;
}

void msk_free(msk_t *m) {
	if (!m) return;
	frontend_free(m->fe); fir_free(m->channel);
	free(m->acq); free(m->cum); free(m->out); free(m);
}

// Squaring MSK leaves lines at twice the offset ± half the bit rate: find the strongest pair.
static void acquire(msk_t *m, const float *iq, int n) {
	m->acq = dsp_reserve(m->acq, &m->acqcap, (size_t)2 * (m->acqlen + n), sizeof(float));
	memcpy(m->acq + 2 * m->acqlen, iq, (size_t)2 * n * sizeof(float));
	m->acqlen += n;
	const int N = ACQ_FFT;
	if (m->acqlen < fmax(m->rate * ACQ_SECONDS, 2 * N)) return;
	float *p = malloc((size_t)2 * m->acqlen * sizeof(float));
	for (int i = 0; i < m->acqlen; i++) {
		double re = m->acq[2 * i], im = m->acq[2 * i + 1], mag = re * re + im * im;
		if (mag == 0) mag = 1;
		p[2 * i] = (float)((re * re - im * im) / mag); p[2 * i + 1] = (float)(2 * re * im / mag);
	}
	double *power = calloc(N, sizeof *power), *seg = malloc(N * sizeof *seg);
	for (int start = 0; start + N <= m->acqlen; start += N / 2) {
		dsp_power_spectrum(p + 2 * start, N, seg);
		for (int i = 0; i < N; i++) power[i] += seg[i];
	}
	int pair = (int)lround(m->baud / 2 / m->rate * N);
	int span = (int)floor(MSK_SEARCH_HZ * 2 / m->rate * N);
	if (span > N / 2 - 1 - pair) span = N / 2 - 1 - pair;
	int best = N / 2;
#define SCORE(i) (power[(i) - pair] + power[(i) + pair])
	for (int i = N / 2 - span; i <= N / 2 + span; i++) if (SCORE(i) > SCORE(best)) best = i;
#undef SCORE
	double offset = (double)(best - N / 2) / N * m->rate / 2;
	m->freq = 2 * M_PI * offset / m->rate;
	m->acquired = 1; m->unlocked_bits = 0;  // acq keeps the samples: process() demodulates them next.
	free(p); free(power); free(seg);
}

// One bit: Gardner timing on the integrated phase, decision, AFC.
static float bit(msk_t *m) {
	const double *cum = m->cum, q = M_PI / 2;
#define AT(t) (cum[(int)floor(t)] + (cum[(int)floor(t) + 1] - cum[(int)floor(t)]) * ((t) - floor(t)))
	// Phase change across the bit ending now, and across the one ending half a bit ago.
	double y = AT(m->t) - AT(m->t - m->omega);
	double mid = AT(m->t - m->omega / 2) - AT(m->t - 1.5 * m->omega);
#undef AT
	double terr = fmax(-1, fmin(1, (m->prev_y - y) * mid / (q * q)));
	m->prev_y = y;
	m->omega = fmin(m->spb * 1.01, fmax(m->spb * 0.99, m->omega + m->t_beta * terr * m->spb));
	m->t += m->omega + m->t_alpha * terr * m->spb;
	double v = y / q, d = v >= 0 ? 1 : -1;
	// AFC: a frequency offset adds a constant phase drift to every bit.
	m->freq += 0.002 * (y - d * q) / m->spb;
	m->evm += 0.01 * ((v - d) * (v - d) - m->evm);
	m->good_run = m->evm < LOCK_EVM ? m->good_run + 1 : 0;
	return (float)v;
}

int msk_process(msk_t *m, const float *iq, int n, const float **bits) {
	const float *x;
	int k = frontend_process(m->fe, iq, n, &x), count = 0;
	*bits = m->out;
	if (!m->acquired) {
		acquire(m, x, k);
		if (!m->acquired) return 0;
		x = m->acq; k = m->acqlen; m->acqlen = 0;  // Demodulate everything the search looked at.
	}
	m->cum = dsp_reserve(m->cum, &m->cumcap, (size_t)m->cumlen + k + 1, sizeof(double));
	if (m->t < 1.5 * m->omega + 1) m->t = 1.5 * m->omega + 1;
	for (int i = 0; i < k; i++) {
		double c = cos(m->phase), s = sin(m->phase), re = x[2 * i], im = x[2 * i + 1];
		m->phase += m->freq;
		if (m->phase > M_PI) m->phase -= 2 * M_PI; else if (m->phase < -M_PI) m->phase += 2 * M_PI;
		float yr, yi;
		fir_push(m->channel, (float)(re * c + im * s), (float)(im * c - re * s), &yr, &yi);
		// Discriminator, summed: the unwrapped phase.
		m->phase_sum += atan2(yi * m->prev_re - yr * m->prev_im, yr * m->prev_re + yi * m->prev_im);
		m->prev_re = yr; m->prev_im = yi;
		m->cum[m->cumlen++] = m->phase_sum;
		while (m->t + 1 < m->cumlen) {
			m->out = dsp_reserve(m->out, &m->outcap, (size_t)count + 1, sizeof(float));
			m->out[count++] = bit(m);
		}
	}
	int keep = (int)floor(m->t - 1.5 * m->omega - 2);
	if (keep < 0) keep = 0;
	if (keep > m->cumlen) keep = m->cumlen;
	memmove(m->cum, m->cum + keep, (size_t)(m->cumlen - keep) * sizeof(double));
	m->cumlen -= keep; m->t -= keep;
	*bits = m->out;
	demod_status_t st = msk_status(m);
	m->unlocked_bits = st.locked ? 0 : m->unlocked_bits + count;
	if (m->reacquire && m->unlocked_bits > 4 * m->baud) msk_reset(m);
	return count;
}

demod_status_t msk_status(const msk_t *m) {
	return (demod_status_t){
		.acquired = m->acquired, .locked = m->acquired && m->good_run >= LOCK_SUSTAIN,
		.snr_db = 10 * log10(1 / fmax(m->evm, 1e-6)), .coherence = fmax(0, 1 - m->evm),
		.offset_hz = m->freq * m->rate / (2 * M_PI), .baud_hz = m->baud * m->spb / m->omega};
}
