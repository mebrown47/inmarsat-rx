// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// IQ sources. Every input delivers interleaved float I/Q (full scale ±1).
#ifndef INPUT_H
#define INPUT_H

typedef struct input input_t;
struct input {
	double rate, center_hz;   // Filled by the opener (0 = unknown; the caller may set them).
	double start_ms;          // Wall-clock time of the first sample (epoch ms), 0 if unknown.
	int live;
	// Reads up to max frames; returns frames read, 0 at end of input, -1 on error.
	int (*read)(input_t *in, float *iq, int max);
	void (*close)(input_t *in);
	void *priv;
	char name[256];
};

typedef enum { FMT_CU8, FMT_CS8, FMT_CS16, FMT_CF32 } iq_format_t;
int iq_format_parse(const char *s);   // -1 if unknown.

// path: a file, a .sigmf-meta/.sigmf-data pair, or "-" for stdin. format < 0: from SigMF.
input_t *input_open_file(const char *path, int format);

// Live sources: "airspy[:SERIAL]", "hackrf[:SERIAL]", "rtl_tcp:HOST:PORT", "spyserver:HOST:PORT".
typedef struct {
	double center_hz;     // Required.
	double rate;          // 0: the lowest rate the device offers whose usable band covers half_span.
	double half_span;     // Farthest channel from the centre, Hz.
	const char *gain;     // Device-specific; NULL for the default. See the usage text.
	int bias, amp;        // Antenna power (bias-tee) and HackRF RF amp.
	double ppm;
} live_opts_t;
// Returns NULL (with a message on stderr) if spec is not a live source or it cannot be opened.
input_t *input_open_live(const char *spec, const live_opts_t *opts);
int input_is_live(const char *spec);
// Live inputs count samples lost to overruns here.
extern volatile long long input_dropped;

#endif
