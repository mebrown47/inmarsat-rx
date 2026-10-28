// Channel front end: mix the channel to 0 Hz, then two decimation stages.
#include "demod.h"
#include <math.h>
#include <stdlib.h>

frontend_t *frontend_new(double in_rate, double offset_hz, double target, double pass, double cut2, double tw2) {
	frontend_t *f = calloc(1, sizeof *f);
	f->ph_re = 1; f->ph_im = 0;
	double w = -2 * M_PI * offset_hz / in_rate;
	f->step_re = cos(w); f->step_im = sin(w);
	double rate = in_rate;
	int n, d1 = (int)floor(in_rate / (8 * target));
	// Stage 1 only has to stop aliases landing inside ±pass, so its transition band is wide.
	if (d1 >= 2) {
		rate = in_rate / d1;
		float *h = dsp_lowpass(rate / 2, in_rate, rate - 2 * pass, 2047, &n);
		f->stage1 = decim_new(h, n, d1, 2);
	}
	int d2 = (int)floor(rate / target);
	if (d2 < 1) d2 = 1;
	double c = fmin(0.4 * rate / d2, cut2), t = fmax(tw2, 0.1 * rate / d2);
	float *h = dsp_lowpass(c, rate, t, 2047, &n);
	f->stage2 = decim_new(h, n, d2, 2);
	f->rate = rate / d2;
	return f;
}

void frontend_free(frontend_t *f) {
	if (!f) return;
	decim_free(f->stage1); decim_free(f->stage2); free(f->mixed); free(f);
}

int frontend_process(frontend_t *f, const float *iq, int n, const float **out) {
	f->mixed = dsp_reserve(f->mixed, &f->mixedcap, (size_t)2 * n, sizeof(float));
	double pr = f->ph_re, pi = f->ph_im;
	const double sr = f->step_re, si = f->step_im;
	for (int i = 0; i < n; i++) {
		double re = iq[2 * i], im = iq[2 * i + 1];
		f->mixed[2 * i] = (float)(re * pr - im * pi);
		f->mixed[2 * i + 1] = (float)(re * pi + im * pr);
		double t = pr * sr - pi * si; pi = pr * si + pi * sr; pr = t;
	}
	double m = 1 / hypot(pr, pi);  // Keep the phasor on the unit circle.
	f->ph_re = pr * m; f->ph_im = pi * m;
	const float *x = f->mixed;
	if (f->stage1) { n = decim_process(f->stage1, x, n); x = f->stage1->out; }
	n = decim_process(f->stage2, x, n);
	*out = f->stage2->out;
	return n;
}
