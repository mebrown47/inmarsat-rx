// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Channel front end and symbol demodulators: coherent PSK (STD-C BPSK, Aero-H+ OQPSK) and
// non-coherent MSK (Aero-L). Ported from the SDR Workbench (usb/psk.js, usb/msk.js).
#ifndef DEMOD_H
#define DEMOD_H

#include <stddef.h>
#include "dsp.h"

// Moves a channel at offset_hz to 0 Hz and decimates it in two stages.
typedef struct {
	double ph_re, ph_im, step_re, step_im; int renorm;  // Input mixer phasor.
	decim_t *stage1, *stage2;
	double rate;                                        // Output rate.
	float *mixed; size_t mixedcap;
} frontend_t;
// pass: highest frequency later stages need; cut2/tw2: stage-2 cutoff and minimum transition.
frontend_t *frontend_new(double in_rate, double offset_hz, double target, double pass, double cut2, double tw2);
void frontend_free(frontend_t *f);
// Returns the number of output frames; *out points into the front end's buffer.
int frontend_process(frontend_t *f, const float *iq, int n, const float **out);

typedef struct {
	int acquired, locked;
	double snr_db, coherence, offset_hz, baud_hz;
} demod_status_t;

typedef enum { PSK_BPSK, PSK_QPSK, PSK_OQPSK } psk_mode_t;

typedef struct psk psk_t;
// reacquire: search for the carrier again after ~2 s without lock (off for Aero, whose frame
// sync judges lock itself).
psk_t *psk_new(psk_mode_t mode, double baud, double in_rate, double offset_hz, int reacquire);
// Searches for the carrier again (the decoder found no frames where the search settled).
void psk_restart(psk_t *p);
void psk_free(psk_t *p);
// Returns the number of symbols; *symbols holds interleaved I/Q (BPSK ±1, QPSK (±1±j)/√2).
int psk_process(psk_t *p, const float *iq, int n, const float **symbols);
demod_status_t psk_status(const psk_t *p);

typedef struct msk msk_t;
msk_t *msk_new(double bit_rate, double in_rate, double offset_hz, int reacquire);
void msk_restart(msk_t *m);
void msk_free(msk_t *m);
// Returns the number of soft bits (positive = 1, the higher tone).
int msk_process(msk_t *m, const float *iq, int n, const float **bits);
demod_status_t msk_status(const msk_t *m);

#define PSK_SEARCH_HZ 2500.0
#define MSK_SEARCH_HZ 1200.0  // Aero-L carriers sit 5 kHz apart; stay off the neighbours.

#endif
