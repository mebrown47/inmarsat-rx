// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// A decoded channel: one carrier in the input band, its demodulator and decoder, and the JSON
// records it emits.
#ifndef CHANNEL_H
#define CHANNEL_H

#include "events.h"

typedef enum { CH_STDC, CH_AERO_H, CH_AERO_L600, CH_AERO_L1200 } channel_kind_t;

typedef struct {
	const char *source_kind, *source_name;   // Record "source" field.
	int hide_acks, emit_data, emit_bulletins, raw_blocks;   // hide_acks: stdout/UDP records only
} channel_opts_t;

typedef struct channel channel_t;
// Parses "stdc", "aero" (Aero-H+ 10.5k), "aerol600", "aerol1200"; -1 if unknown.
int channel_kind_parse(const char *name);
const char *channel_kind_name(channel_kind_t k);

// sat: satellite id (plans.h) or a free name; found: added by --scan.
channel_t *channel_new(channel_kind_t kind, double freq_hz, const char *sat, int found, double center_hz, double rate,
	const channel_opts_t *opts);
void channel_free(channel_t *c);
// iq: n interleaved I/Q frames at the input rate; now_ms: time of the first sample.
void channel_process(channel_t *c, const float *iq, int n, double now_ms);
// Emits anything still being reassembled (end of input).
void channel_flush(channel_t *c);
// One-line status for stderr.
void channel_status(const channel_t *c, char *buf, int size);
void channel_info(const channel_t *c, channel_info_t *info);
double channel_freq(const channel_t *c);
channel_kind_t channel_kind(const channel_t *c);
// STD-C: the packet tally (count and bytes by descriptor byte) since the last call, then zeroed.
// Returns 0 for other channels or when there is nothing to report. Call between blocks.
int channel_take_tally(channel_t *c, unsigned long counts[256], unsigned long bytes[256]);
// The secret key for hashing ships' mobile numbers (HMAC-SHA256). Set once, before decoding.
void channel_set_mobile_key(const unsigned char *key, int len);

#endif
