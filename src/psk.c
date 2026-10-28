// Coherent BPSK/QPSK/OQPSK symbol recovery, ported from the SDR Workbench (usb/psk.js).
// Chain: front end to ~16 samples/symbol -> coarse carrier acquisition (M-th power FFT) ->
// NCO -> channel filter -> Gardner timing recovery -> Costas carrier loop -> AGC.
#include "demod.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPS_TARGET 16
#define ACQ_SECONDS 0.5
#define ACQ_FFT 4096
#define LOCK_SUSTAIN 300
#define LOCK_HOLDOFF 400   // Symbols after acquisition before lock can be declared.
#define COH_ALPHA 0.005    // Coherence averaging (~200 symbols).

typedef struct { double alpha, beta; } gains_t;
static gains_t loop_gains(double bn) {
	const double zeta = 0.707, theta = bn / (zeta + 0.25 / zeta), d = 1 + 2 * zeta * theta + theta * theta;
	return (gains_t){4 * zeta * theta / d, 4 * theta * theta / d};
}

struct psk {
	psk_mode_t mode; int order, offset_q, reacquire;
	double baud, rate, sps, lock_coherence;
	frontend_t *fe; fir_t *channel; gains_t carrier, timing;
	// State.
	double phase, freq; int acquired;
	float *acq; size_t acqcap; int acqlen;
	float *buf; size_t bufcap; int buflen;  // Filtered samples (frames) kept for interpolation.
	double t, omega, prev_re, prev_im, level, evm;
	double coh_re, coh_im, coh_mag; long symbol_count, coherent_run, unlocked_symbols;
	float *out; size_t outcap;
};

static void psk_reset(psk_t *p) {
	p->phase = 0; p->freq = 0; p->acquired = 0; p->acqlen = 0;
	p->buflen = 0; p->t = 2 * p->sps; p->omega = p->sps;
	p->prev_re = p->prev_im = 0; p->level = 1; p->evm = 1; p->unlocked_symbols = 0;
	p->coh_re = p->coh_im = 0; p->coh_mag = 1e-9; p->symbol_count = 0; p->coherent_run = 0;
}

psk_t *psk_new(psk_mode_t mode, double baud, double in_rate, double offset_hz, int reacquire) {
	psk_t *p = calloc(1, sizeof *p);
	p->mode = mode; p->baud = baud; p->reacquire = reacquire;
	p->order = mode == PSK_BPSK ? 2 : 4;
	// OQPSK: sample I half a symbol earlier than Q, which turns it back into QPSK for the loops.
	p->offset_q = mode == PSK_OQPSK;
	p->lock_coherence = mode == PSK_BPSK ? 0.45 : 0.55;
	double target = SPS_TARGET * baud, pass = 0.75 * baud + PSK_SEARCH_HZ + baud;
	p->fe = frontend_new(in_rate, offset_hz, target, pass, 0.75 * baud + PSK_SEARCH_HZ, baud);
	p->rate = p->fe->rate; p->sps = p->rate / baud;
	int n; float *h;
	// Channel filter: root-raised-cosine where the shaping is known (Aero OQPSK uses α = 1, as
	// JAERO does); otherwise a low-pass over the main lobe.
	if (p->offset_q) h = dsp_rrc(1.0, p->sps, 8, &n);
	else h = dsp_lowpass(0.75 * baud, p->rate, 0.5 * baud, 2047, &n);
	p->channel = fir_new(h, n);
	// OQPSK (Aero) needs a narrower carrier loop: on a live capture with an overlapping carrier,
	// 0.005 gave exact frame spacing where 0.02 slipped every frame.
	p->carrier = loop_gains(p->offset_q ? 0.005 : 0.02); p->timing = loop_gains(0.01);
	psk_reset(p);
	return p;
}

void psk_free(psk_t *p) {
	if (!p) return;
	frontend_free(p->fe); fir_free(p->channel);
	free(p->acq); free(p->buf); free(p->out); free(p);
}

// Coarse acquisition: |FFT(x^M)| peaks at M times the carrier offset.
static void acquire(psk_t *p, const float *iq, int n) {
	p->acq = dsp_reserve(p->acq, &p->acqcap, (size_t)2 * (p->acqlen + n), sizeof(float));
	memcpy(p->acq + 2 * p->acqlen, iq, (size_t)2 * n * sizeof(float));
	p->acqlen += n;
	const int N = ACQ_FFT;
	if (p->acqlen < fmax(p->rate * ACQ_SECONDS, 2 * N)) return;
	float *pw = malloc((size_t)2 * p->acqlen * sizeof(float));
	for (int i = 0; i < p->acqlen; i++) {
		double re = p->acq[2 * i], im = p->acq[2 * i + 1];
		for (int k = 1; k < p->order; k++) {
			double r = re * p->acq[2 * i] - im * p->acq[2 * i + 1];
			im = re * p->acq[2 * i + 1] + im * p->acq[2 * i]; re = r;
		}
		double m = hypot(re, im); if (m == 0) m = 1;  // Normalise so noise bursts do not dominate.
		pw[2 * i] = (float)(re / m); pw[2 * i + 1] = (float)(im / m);
	}
	double *power = calloc(N, sizeof *power), *seg = malloc(N * sizeof *seg);
	for (int start = 0; start + N <= p->acqlen; start += N / 2) {
		dsp_power_spectrum(pw + 2 * start, N, seg);
		for (int i = 0; i < N; i++) power[i] += seg[i];
	}
	int span = (int)floor(PSK_SEARCH_HZ * p->order / p->rate * N);
	if (span > N / 2 - 1) span = N / 2 - 1;
	int best = N / 2;
	for (int i = N / 2 - span; i <= N / 2 + span; i++) if (power[i] > power[best]) best = i;
	double offset = (double)(best - N / 2) / N * p->rate / p->order;
	p->freq = 2 * M_PI * offset / p->rate;
	p->acqlen = 0; p->acquired = 1; p->unlocked_symbols = 0;
	free(pw); free(power); free(seg);
}

// One symbol: Gardner timing, AGC, Costas carrier loop. The loop corrections act on the very
// next input sample (the NCO runs sample by sample), not a block later.
static void symbol(psk_t *p, float *out) {
	const float *buf = p->buf;
#define AT(t, k) (buf[2 * (int)floor(t) + (k)] + (buf[2 * (int)floor(t) + 2 + (k)] - buf[2 * (int)floor(t) + (k)]) * ((t) - floor(t)))
	// Gardner: on-time sample now, mid-point half a symbol earlier, previous on-time sample.
	double lag = p->offset_q ? p->omega / 2 : 0;
	double y_re = AT(p->t - lag, 0), y_im = AT(p->t, 1), mid = p->t - p->omega / 2;
	double m_re = AT(mid - lag, 0), m_im = AT(mid, 1);
#undef AT
	double te = (p->prev_re - y_re) * m_re + (p->prev_im - y_im) * m_im;
	p->prev_re = y_re; p->prev_im = y_im;
	// AGC on symbol magnitude.
	double mag = hypot(y_re, y_im);
	p->level += 0.01 * (mag - p->level);
	double g = 1 / fmax(p->level, 1e-9), s_re = y_re * g, s_im = y_im * g;
	out[0] = (float)s_re; out[1] = (float)s_im;
	double terr = fmax(-1, fmin(1, te * g * g));
	p->omega = fmin(p->sps * 1.01, fmax(p->sps * 0.99, p->omega + p->timing.beta * terr * p->sps));
	p->t += p->omega + p->timing.alpha * terr * p->sps;
	// Costas (decision-directed) phase error; QPSK points sit on the diagonals.
	double d_re, d_im;
	if (p->order == 2) { d_re = s_re >= 0 ? 1 : -1; d_im = 0; }
	else { d_re = s_re >= 0 ? M_SQRT1_2 : -M_SQRT1_2; d_im = s_im >= 0 ? M_SQRT1_2 : -M_SQRT1_2; }
	double cerr = fmax(-1, fmin(1, p->order == 2 ? s_im * d_re : s_im * d_re - s_re * d_im));
	p->phase += p->carrier.alpha * cerr;
	p->freq += p->carrier.beta * cerr / p->sps;
	double e_re = s_re - d_re, e_im = s_im - d_im;
	p->evm += 0.005 * ((e_re * e_re + e_im * e_im) - p->evm);
	// Phase coherence of s^M: steady when locked, spins towards zero when slipping.
	double p_re = s_re, p_im = s_im;
	for (int k = 1; k < p->order; k++) { double r = p_re * s_re - p_im * s_im; p_im = p_re * s_im + p_im * s_re; p_re = r; }
	p->coh_re += COH_ALPHA * (p_re - p->coh_re); p->coh_im += COH_ALPHA * (p_im - p->coh_im);
	p->coh_mag += COH_ALPHA * (hypot(p_re, p_im) - p->coh_mag);
	p->symbol_count++;
	int coherent = hypot(p->coh_re, p->coh_im) >= p->lock_coherence * p->coh_mag;
	p->coherent_run = coherent && p->symbol_count >= LOCK_HOLDOFF ? p->coherent_run + 1 : 0;
}

int psk_process(psk_t *p, const float *iq, int n, const float **symbols) {
	const float *x;
	int m = frontend_process(p->fe, iq, n, &x), count = 0;
	*symbols = p->out;
	if (!p->acquired) { acquire(p, x, m); return 0; }
	p->buf = dsp_reserve(p->buf, &p->bufcap, (size_t)2 * (p->buflen + m), sizeof(float));
	p->out = dsp_reserve(p->out, &p->outcap, (size_t)2 * (m / (p->sps * 0.98) + 4), sizeof(float));
	// OQPSK looks back an extra half symbol for the I sample.
	if (p->offset_q && p->t < 1.5 * p->omega + 1) p->t = 1.5 * p->omega + 1;
	for (int i = 0; i < m; i++) {
		// NCO mix to baseband, then the channel filter.
		double c = cos(p->phase), s = sin(p->phase), re = x[2 * i], im = x[2 * i + 1];
		p->phase += p->freq;
		if (p->phase > M_PI) p->phase -= 2 * M_PI; else if (p->phase < -M_PI) p->phase += 2 * M_PI;
		fir_push(p->channel, (float)(re * c + im * s), (float)(im * c - re * s), &p->buf[2 * p->buflen], &p->buf[2 * p->buflen + 1]);
		p->buflen++;
		while (p->t + 1 < p->buflen) {
			p->out = dsp_reserve(p->out, &p->outcap, (size_t)2 * count + 2, sizeof(float));
			symbol(p, p->out + 2 * count++);
		}
	}
	// Keep enough history for the next interpolation and the half-symbol look-back.
	int keep = (int)floor(p->t - (p->offset_q ? 1.5 : 1) * p->omega - 2);
	if (keep < 0) keep = 0;
	if (keep > p->buflen) keep = p->buflen;
	memmove(p->buf, p->buf + 2 * keep, (size_t)2 * (p->buflen - keep) * sizeof(float));
	p->buflen -= keep; p->t -= keep;
	*symbols = p->out;
	demod_status_t st = psk_status(p);
	// Lost lock for ~2 s: search for the carrier again.
	p->unlocked_symbols = st.locked ? 0 : p->unlocked_symbols + count;
	if (p->reacquire && p->unlocked_symbols > 2 * p->baud) psk_reset(p);
	return count;
}

demod_status_t psk_status(const psk_t *p) {
	return (demod_status_t){
		.acquired = p->acquired, .locked = p->acquired && p->coherent_run >= LOCK_SUSTAIN,
		.snr_db = 10 * log10(1 / fmax(p->evm, 1e-6)),
		.coherence = hypot(p->coh_re, p->coh_im) / fmax(p->coh_mag, 1e-9),
		.offset_hz = p->freq * p->rate / (2 * M_PI), .baud_hz = p->baud * p->sps / p->omega};
}
