// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Inmarsat-C (STD-C) NCS TDM decoder: frame sync, de-permutation, de-interleaving, soft Viterbi,
// descrambling, packet framing/checksum, bulletin boards and EGC message reassembly.
#ifndef STDC_H
#define STDC_H

typedef struct {
	int id, service, repetition, complete, parts;
	const char *service_name, *priority;
	const char *text;
} stdc_egc_t;

typedef struct {
	int frame_number, les, version;
	double utc_seconds;
	const char *satellite;
} stdc_bulletin_t;

typedef struct {
	int synced, sync_score; long frames, dropped, packets_ok, packets_bad; double ber;  // ber < 0: none yet.
} stdc_stats_t;

typedef void (*stdc_egc_cb)(const stdc_egc_t *msg, void *user);
typedef void (*stdc_bulletin_cb)(const stdc_bulletin_t *b, void *user);

typedef struct stdc stdc_t;
stdc_t *stdc_new(stdc_egc_cb on_egc, stdc_bulletin_cb on_bulletin, void *user);
void stdc_free(stdc_t *s);
// soft: one value per 1200 Bd symbol (positive = 1). now_ms: current time, for EGC expiry.
void stdc_feed(stdc_t *s, const float *soft, int n, double now_ms);
// Emits EGC messages still waiting for parts (end of input).
void stdc_flush(stdc_t *s);
stdc_stats_t stdc_stats(const stdc_t *s);
// Signalling metadata from one packet: which kind, which ship (its 24-bit mobile number, for the
// caller to hash), which LES and logical channel, and sizes. Never message contents.
typedef struct {
	int desc;                  // Descriptor byte.
	int has_mes; unsigned mes; // Mobile earth station (ship) number, when the packet carries one.
	int les, sat;              // Target LES and its satellite code, or -1.
	int lc;                    // Logical channel, or -1.
	int pres;                  // Announcement: presentation (0 IA5, else other), or -1.
	int pkt_no, payload;       // Message packet: sequence number and payload bytes; else -1, 0.
} stdc_sig_t;
#define STDC_MAX_SIGS 64

// Packet tally and signalling per frame: after each decoded frame, the number of packets with a
// good checksum and their bytes by descriptor byte (multiframe contents counted too), and the
// signalling metadata. Metadata only.
typedef void (*stdc_frame_cb)(const unsigned *counts, const unsigned *bytes, const stdc_sig_t *sigs, int nsigs,
	void *user);
void stdc_set_frame_cb(stdc_t *s, stdc_frame_cb cb);

#endif
