// Viterbi decoder and encoder, ported from the SDR Workbench (usb/stdc.js).
#include "fec.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define K 7
#define STATES (1 << (K - 1))
static const int POLY[2] = {0x5b, 0x79};
static uint8_t ENC[STATES * 2];
static int ready;

static int parity(int x) { x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return x & 1; }
static int next_state(int s, int u) { return (s >> 1) | (u << (K - 2)); }

static void init(void) {
	if (ready) return;
	for (int s = 0; s < STATES; s++) for (int u = 0; u < 2; u++) {
		int reg = (u << (K - 1)) | s;
		ENC[s * 2 + u] = (uint8_t)((parity(reg & POLY[0]) << 1) | parity(reg & POLY[1]));
	}
	ready = 1;
}

int fec_viterbi(const float *soft, int n, uint8_t *out) {
	init();
	int T = n >> 1;
	double metric[STATES], next[STATES];
	for (int s = 0; s < STATES; s++) metric[s] = 0;
	uint8_t *from = malloc((size_t)T * STATES), *bit = malloc((size_t)T * STATES);
	for (int t = 0; t < T; t++) {
		double a = soft[2 * t], b = soft[2 * t + 1];
		for (int s = 0; s < STATES; s++) next[s] = -INFINITY;
		for (int s = 0; s < STATES; s++) {
			double m = metric[s];
			if (m == -INFINITY) continue;
			for (int u = 0; u < 2; u++) {
				int e = ENC[s * 2 + u], ns = next_state(s, u);
				// Correlation metric: soft values agree in sign with the expected bits.
				double v = m + (e & 2 ? a : -a) + (e & 1 ? b : -b);
				if (v > next[ns]) { next[ns] = v; from[t * STATES + ns] = (uint8_t)s; bit[t * STATES + ns] = (uint8_t)u; }
			}
		}
		memcpy(metric, next, sizeof metric);
	}
	int best = 0;
	for (int s = 1; s < STATES; s++) if (metric[s] > metric[best]) best = s;
	int s = best;
	for (int t = T - 1; t >= 0; t--) { out[t] = bit[t * STATES + s]; s = from[t * STATES + s]; }
	free(from); free(bit);
	return s;
}

void fec_encode(const uint8_t *bits, int nbits, int start_state, uint8_t *out) {
	init();
	for (int t = 0, s = start_state; t < nbits; t++) {
		int e = ENC[s * 2 + bits[t]];
		out[2 * t] = (uint8_t)(e >> 1); out[2 * t + 1] = e & 1; s = next_state(s, bits[t]);
	}
}

double fec_ber(const float *soft, int n, const uint8_t *bits, int start_state) {
	uint8_t *re = malloc(n);
	fec_encode(bits, n / 2, start_state, re);
	int errors = 0;
	for (int i = 0; i < n; i++) if ((soft[i] > 0 ? 1 : 0) != re[i]) errors++;
	free(re);
	return (double)errors / n;
}
