// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Inmarsat-C (STD-C) NCS TDM decoder, ported from the SDR Workbench (usb/stdc.js).
//
// Written from the published frame structure. Protocol constants (unique word, permutation,
// interleaver shape, code polynomials, scrambling sequence, packet descriptors, checksum, EGC
// header layout) were checked against the inmarsat-sniffer project (GPL-3.0, used only as a
// reference; no code copied), which in turn credits the inmarsatc library and SatDump.
#include "stdc.h"
#include "fec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 64
#define COLS 162
#define FRAME_SYMBOLS (ROWS * COLS)   // 10368 symbols, 8.64 s
#define DATA_COLS 160
#define CODED (ROWS * DATA_COLS)
#define FRAME_BYTES (CODED / 16)      // 640 bytes
#define SYNC_ACQUIRE 112              // Of 128 unique-word symbol matches.
#define SYNC_HOLD 96
#define MAX_BER 0.15                  // Frames with more channel errors are dropped.
#define EGC_TIMEOUT_MS (10 * 60 * 1000.0)

static const uint8_t UNIQUE_WORD[64] = {
	0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0,
	0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0};
// One entry per 4-byte group of the decoded frame: 1 = invert the group.
static const uint8_t SCRAMBLE[160] = {
	0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1,
	1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1,
	0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1,
	1, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1,
	1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0};

static const char *SATELLITES[4] = {"AOR-W", "AOR-E", "POR", "IOR"};
static const char *PRIORITIES[4] = {"Routine", "Safety", "Urgency", "Distress"};

static const char *service_name(int s) {
	switch (s) {
	case 0x00: return "All ships";
	case 0x02: return "FleetNET group call";
	case 0x04: return "SafetyNET rectangular area";
	case 0x11: return "Inmarsat system message";
	case 0x13: return "Coastal warning";
	case 0x14: return "Distress alert (circular area)";
	case 0x23: return "EGC system message";
	case 0x24: return "SafetyNET circular area";
	case 0x31: return "NAVAREA/METAREA warning";
	case 0x33: return "Download group identity";
	case 0x34: return "SAR coordination (rectangular)";
	case 0x44: return "SAR coordination (circular)";
	case 0x72: return "FleetNET chart correction";
	case 0x73: return "SafetyNET chart correction";
	default: return NULL;
	}
}

static int address_length(int s) {
	switch (s) {
	case 0x11: case 0x31: return 4;
	case 0x02: case 0x72: return 5;
	case 0x13: case 0x23: case 0x33: case 0x73: return 6;
	case 0x04: case 0x14: case 0x24: case 0x34: case 0x44: return 7;
	default: return 3;
	}
}

typedef struct { int index; uint8_t *data; int len; } part_t;
typedef struct {
	int id, service, priority, repetition, presentation, complete, emitted;
	double first, last;
	part_t *parts; int nparts;
} egc_t;

struct stdc {
	stdc_egc_cb on_egc; stdc_bulletin_cb on_bulletin; void *user;
	stdc_frame_cb on_frame; unsigned frame_counts[256], frame_bytes[256];
	float ring[FRAME_SYMBOLS]; int head, filled, since_frame;
	stdc_stats_t stats;
	egc_t *egc; int negc, egccap;
	uint8_t *multi; int multi_len, multi_total, multi_active;
	double now;
};

static int reverse_byte(int b) {
	b = ((b >> 1) & 0x55) | ((b & 0x55) << 1); b = ((b >> 2) & 0x33) | ((b & 0x33) << 2);
	return ((b >> 4) & 0x0f) | ((b & 0x0f) << 4);
}

// Packet checksum (two running sums); the last two bytes are excluded.
static int checksum(const uint8_t *bytes, int len) {
	int c0 = 0, c1 = 0;
	for (int i = 0; i < len; i++) { int b = i < len - 2 ? bytes[i] : 0; c0 = (c0 + b) & 0xffff; c1 = (c1 + c0) & 0xffff; }
	return (((c0 - c1) & 0xff) << 8) | ((c1 - 2 * c0) & 0xff);
}

// Packet length in bytes (including descriptor and checksum) from its descriptor.
static int packet_length(const uint8_t *frame, int pos, int size) {
	int d = frame[pos];
	if ((d & 0x80) == 0) return (d & 0x0f) + 1;
	if ((d & 0xc0) == 0x80) return pos + 1 < size ? frame[pos + 1] + 2 : size - pos;
	return size - pos;
}

void stdc_set_frame_cb(stdc_t *s, stdc_frame_cb cb) { s->on_frame = cb; }

stdc_t *stdc_new(stdc_egc_cb on_egc, stdc_bulletin_cb on_bulletin, void *user) {
	stdc_t *s = calloc(1, sizeof *s);
	s->on_egc = on_egc; s->on_bulletin = on_bulletin; s->user = user;
	s->stats.ber = -1;
	return s;
}

static void egc_free(egc_t *m) {
	for (int i = 0; i < m->nparts; i++) free(m->parts[i].data);
	free(m->parts);
}

void stdc_free(stdc_t *s) {
	if (!s) return;
	for (int i = 0; i < s->negc; i++) egc_free(&s->egc[i]);
	free(s->egc); free(s->multi); free(s);
}

stdc_stats_t stdc_stats(const stdc_t *s) { return s->stats; }

static float at(const stdc_t *s, int i) { return s->ring[(s->head + i) % FRAME_SYMBOLS]; }

static int sync_score(const stdc_t *s) {
	int match = 0;
	for (int r = 0; r < ROWS; r++) {
		int w = UNIQUE_WORD[r];
		if ((at(s, r * COLS) > 0 ? 1 : 0) == w) match++;
		if ((at(s, r * COLS + 1) > 0 ? 1 : 0) == w) match++;
	}
	return match;  // 0..128; low values mean an inverted frame.
}

static int cmp_part(const void *a, const void *b) { return ((const part_t *)a)->index - ((const part_t *)b)->index; }

static void egc_emit(stdc_t *s, egc_t *m) {
	qsort(m->parts, m->nparts, sizeof *m->parts, cmp_part);
	size_t cap = 64; int len = 0;
	for (int i = 0; i < m->nparts; i++) cap += (size_t)m->parts[i].len * 3 + 8;
	char *text = malloc(cap);
	for (int i = 0; i < m->nparts; i++) {
		const part_t *p = &m->parts[i];
		if (m->presentation == 0) {
			// IA5 text per part; a jump in part numbers means a part was not received, shown as […].
			if (i && p->index != m->parts[i - 1].index + 1) len += sprintf(text + len, " […] ");
			for (int k = 0; k < p->len; k++) {
				int c = p->data[k] & 0x7f;
				if (c == 0x03) break;
				if (c >= 0x20 || c == 10 || c == 13 || c == 9) text[len++] = (char)c;
			}
		} else {
			for (int k = 0; k < p->len; k++) len += sprintf(text + len, "%s%02x", len ? " " : "", p->data[k]);
		}
	}
	text[len] = 0;
	char fallback[32];
	const char *name = service_name(m->service);
	if (!name) { snprintf(fallback, sizeof fallback, "Service 0x%x", m->service); name = fallback; }
	stdc_egc_t e = {.id = m->id, .service = m->service, .repetition = m->repetition, .complete = m->complete,
		.parts = m->nparts, .service_name = name, .priority = PRIORITIES[m->priority & 3], .text = text};
	if (s->on_egc) s->on_egc(&e, s->user);
	m->emitted = 1;
	free(text);
}

static void egc_packet(stdc_t *s, const uint8_t *f, int len, int second_half) {
	int service = f[2], flags = f[3], id = (f[4] << 8) | f[5], packet_no = f[6], presentation = f[7];
	int header = 8 + address_length(service);
	if (header > len - 2) header = len - 2;
	egc_t *m = NULL;
	for (int i = 0; i < s->negc; i++) if (s->egc[i].service == service && s->egc[i].id == id) { m = &s->egc[i]; break; }
	if (!m) {
		if (s->negc == s->egccap) { s->egccap = s->egccap ? 2 * s->egccap : 16; s->egc = realloc(s->egc, s->egccap * sizeof *s->egc); }
		m = &s->egc[s->negc++];
		memset(m, 0, sizeof *m);
		m->id = id; m->service = service; m->first = s->now;
	}
	m->priority = (flags >> 5) & 3; m->repetition = flags & 0x1f; m->presentation = presentation; m->last = s->now;
	// Parts arrive as packet 1 (B1, B2), packet 2 (B1, B2) … ; continuation 0 on a B2 ends the message.
	int index = (packet_no - 1) * 2 + (second_half ? 1 : 0);
	if (index < 0) index = 0;
	part_t *p = NULL;
	for (int i = 0; i < m->nparts; i++) if (m->parts[i].index == index) { p = &m->parts[i]; free(p->data); break; }
	if (!p) { m->parts = realloc(m->parts, (m->nparts + 1) * sizeof *m->parts); p = &m->parts[m->nparts++]; }
	p->index = index; p->len = len - 2 - header;
	p->data = malloc(p->len > 0 ? p->len : 1);
	if (p->len > 0) memcpy(p->data, f + header, p->len); else p->len = 0;
	if (second_half && !(flags & 0x80) && !m->complete) { m->complete = 1; egc_emit(s, m); }
}

static void parse(stdc_t *s, const uint8_t *frame, int size);

static void packet(stdc_t *s, const uint8_t *f, int len) {
	int d = f[0];
	if (d == 0x7d && len >= 14) {
		int frame_number = (f[2] << 8) | f[3], sat = (f[7] >> 6) & 3;
		stdc_bulletin_t b = {.frame_number = frame_number, .utc_seconds = frame_number * 8.64,
			.satellite = SATELLITES[sat], .les = f[7] & 0x3f, .version = f[1]};
		if (s->on_bulletin) s->on_bulletin(&b, s->user);
	} else if ((d == 0xb1 || d == 0xb2) && len >= 10) {
		egc_packet(s, f, len, d == 0xb2);
	} else if (d == 0xbd && len >= 5) {
		// Multiframe start: an encapsulated packet continues in later frames.
		const uint8_t *inner = f + 2; int n = len - 4;
		s->multi_total = packet_length(inner, 0, n);
		s->multi = realloc(s->multi, n); memcpy(s->multi, inner, n);
		s->multi_len = n; s->multi_active = 1;
	} else if (d == 0xbe && s->multi_active && len >= 4) {
		int n = len - 4;
		s->multi = realloc(s->multi, s->multi_len + n);
		memcpy(s->multi + s->multi_len, f + 2, n); s->multi_len += n;
		if (s->multi_len >= s->multi_total - 2) {
			int total = s->multi_total < s->multi_len ? s->multi_total : s->multi_len;
			uint8_t *inner = s->multi; s->multi = NULL; s->multi_active = 0;
			parse(s, inner, total);
			free(inner);
		}
	}
}

static void parse(stdc_t *s, const uint8_t *frame, int size) {
	for (int pos = 0; pos < size - 2;) {
		int d = frame[pos];
		if (d == 0x00) break;
		if (d == 0xff) { pos++; continue; }
		int len = packet_length(frame, pos, size);
		if (len < 3 || pos + len > size) break;
		int sum = (frame[pos + len - 2] << 8) | frame[pos + len - 1];
		if (sum != 0 && sum != checksum(frame + pos, len)) { s->stats.packets_bad++; pos += len; continue; }
		s->stats.packets_ok++;
		s->frame_counts[d]++; s->frame_bytes[d] += (unsigned)len;
		packet(s, frame + pos, len);
		pos += len;
	}
}

// Drops completed messages; emits and drops ones that have waited too long for missing parts.
static void expire(stdc_t *s, int all) {
	for (int i = 0; i < s->negc;) {
		egc_t *m = &s->egc[i];
		if (m->complete || all || s->now - m->last > EGC_TIMEOUT_MS) {
			if (!m->emitted) egc_emit(s, m);
			egc_free(m);
			s->egc[i] = s->egc[--s->negc];
		} else i++;
	}
}

void stdc_flush(stdc_t *s) { expire(s, 1); }

static void process_frame(stdc_t *s, int inverted) {
	float *coded = malloc(CODED * sizeof *coded);
	const float sign = inverted ? -1.f : 1.f;
	// Undo the row permutation and the column-wise interleaver; skip the two sync symbols per row.
	for (int r = 0; r < ROWS; r++) {
		int src = ((r * 23) % ROWS) * COLS + 2;
		for (int c = 0; c < DATA_COLS; c++) {
			float v = sign * at(s, src + c);
			coded[c * ROWS + r] = v > 2 ? 2 : v < -2 ? -2 : v;  // Clip so outliers cannot dominate.
		}
	}
	uint8_t *bits = malloc(CODED / 2);
	int start = fec_viterbi(coded, CODED, bits);
	double ber = fec_ber(coded, CODED, bits, start);
	s->stats.ber = ber;
	free(coded);
	if (ber > MAX_BER) { s->stats.dropped++; free(bits); return; }
	uint8_t frame[FRAME_BYTES] = {0};
	for (int i = 0; i < CODED / 2; i++) if (bits[i]) frame[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
	free(bits);
	for (int g = 0; g < DATA_COLS; g++) for (int j = 0; j < 4; j++) {
		int k = g * 4 + j;
		frame[k] = (uint8_t)(reverse_byte(frame[k]) ^ (SCRAMBLE[g] ? 0xff : 0));
	}
	s->stats.frames++;
	memset(s->frame_counts, 0, sizeof s->frame_counts); memset(s->frame_bytes, 0, sizeof s->frame_bytes);
	parse(s, frame, FRAME_BYTES);
	if (s->on_frame) s->on_frame(s->frame_counts, s->frame_bytes, s->user);
	expire(s, 0);
}

void stdc_feed(stdc_t *s, const float *soft, int n, double now_ms) {
	s->now = now_ms;
	for (int i = 0; i < n; i++) {
		s->ring[s->head] = soft[i]; s->head = (s->head + 1) % FRAME_SYMBOLS;
		if (s->filled < FRAME_SYMBOLS) { s->filled++; if (s->filled < FRAME_SYMBOLS) continue; }
		s->since_frame++;
		// Searching: test every alignment. Synced: test once per frame.
		if (s->stats.synced && s->since_frame < FRAME_SYMBOLS) continue;
		int score = sync_score(s), best = score > 128 - score ? score : 128 - score;
		if (best >= (s->stats.synced ? SYNC_HOLD : SYNC_ACQUIRE)) {
			s->stats.synced = 1; s->since_frame = 0; s->stats.sync_score = best;
			process_frame(s, score < 64);
		} else if (s->stats.synced) {
			s->stats.synced = 0; s->stats.sync_score = best;
		}
	}
}
