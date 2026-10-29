// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Decoded messages and channel health, fanned out to the archive (archive.c, read by the
// Observatory) and the live page (web.c). Thread-safe: channels call in from worker threads.
#ifndef EVENTS_H
#define EVENTS_H

typedef struct {
	double t_ms, freq;
	const char *sat, *type;            // satellite id, channel type (aero, aerol600, ...)
	const char *aes, *ges, *reg, *label, *text;
	char block, mode;
	int more, ack;
	const char *app_kind, *app_summary, *tree_json;  // NULL when libacars found nothing
	int app_error;
} ev_acars_t;

typedef struct {
	double t_ms, freq;
	const char *sat;
	int les; const char *station;      // les < 0: no bulletin board yet
	int service, id, parts, complete, repetition;
	const char *service_name, *priority, *text;
} ev_egc_t;

// One channel's state, for the live page and the per-minute health archive.
typedef struct {
	double freq; const char *sat, *type;
	int acquired, locked, synced, found;   // found: added by --scan
	int resting;                           // Aero: no frames for several windows, resting a while
	double snr_db, offset_hz, ber;         // ber < 0: none yet
	long frames, units_ok, units_bad, messages;  // units: Aero signal units or STD-C packets
	int les; char station[48];              // STD-C: the NCS bulletin board's station
} channel_info_t;

// STD-C signalling metadata (see stdc.h): ship is the keyed hash of its mobile number, or NULL.
typedef struct {
	double t_ms, freq; const char *sat;
	int chan_les;              // The LES of the channel it was heard on (from its bulletin), or -1.
	int desc; const char *ship;
	int les, lc, pres, pkt_no, payload;
} ev_stdc_sig_t;

void events_acars(const ev_acars_t *e);
void events_stdc_sig(const ev_stdc_sig_t *e);
void events_egc(const ev_egc_t *e);

#endif
