// Inmarsat Aero P-channel decoders, ported from the SDR Workbench (usb/aero.js).
//
// The scrambler, interleaver, CRC, frame layouts, ISU/SSU reassembly and ACARS parsing follow
// JAERO by Jonathan Olds (https://github.com/jontio/JAERO, MIT licence, Copyright (c)
// 2015-2019 Jonathan Olds). Bit ordering, code taps and alignment were confirmed on live
// I-3 F5 and I-4 F3 captures.
#include "aero.h"
#include "fec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREAMBLE 0xe15ae893u
#define PREAMBLE_BITS 32
#define SYNC_TOLERANCE 3                 // Preamble bit errors allowed.
#define ROWS 64
#define SU_BYTES 12
// Aero-H+: 2625 symbols per frame (preamble included), 16 header + 178 dummy bits, then a
// 4992-bit block in a 64 x 78 interleaver; 26 signal units.
#define H_FRAME_SYMBOLS 2625
#define H_START (16 + 178)
#define H_CODED 4992
#define H_COLS 78
// Aero-L: 1200 bits per frame: preamble, 16 header bits, 1152 coded bits in 64-row blocks
// (6 columns at 600 bit/s, 9 at 1200); 6 signal units.
#define L_FRAME_BITS 1200
#define L_CODED 1152
#define MAX_ISUS 64
#define MAX_FRAGMENTS 64
#define ISU_DATA_MAX 520

typedef struct {
	uint32_t aes; uint8_t ges, qno, refno, seqno, last_count; int age;
	uint8_t data[ISU_DATA_MAX]; int len;
} isu_t;

typedef struct { aero_acars_t msg; size_t textcap; int age; } fragment_t;

struct aero {
	int bit_rate, cols;
	aero_acars_cb on_acars; aero_data_cb on_data; void *user;
	float *s1, *s2; size_t cap1, cap2; int len;  // H+: I and Q streams. L: s1 = bits.
	long base, next;                             // Absolute index of s1[0]; next frame start (-1: search).
	float prev; int parity;
	aero_stats_t stats;
	isu_t isus[MAX_ISUS]; int nisus;
	int have_last; uint32_t last_aes; uint8_t last_ges;
	fragment_t frags[MAX_FRAGMENTS]; int nfrags;
};

static uint8_t PN[H_CODED / 2];
static int PRE[PREAMBLE_BITS];
static int tables_ready;

static void init_tables(void) {
	if (tables_ready) return;
	// Descrambling sequence: 15-bit LFSR, output = s[0] ^ s[14], restarted every frame.
	int st[15] = {1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1};
	for (int a = 0; a < H_CODED / 2; a++) {
		int v = st[0] ^ st[14];
		PN[a] = (uint8_t)v;
		memmove(st + 1, st, 14 * sizeof(int)); st[0] = v;
	}
	for (int i = 0; i < PREAMBLE_BITS; i++) PRE[i] = (PREAMBLE >> (31 - i)) & 1;
	tables_ready = 1;
}

// CRC-16 (reflected 0x1021, init 0xFFFF, inverted) over the first 10 bytes of a signal unit.
uint16_t aero_su_crc(const uint8_t *bytes) {
	uint16_t crc = 0xffff;
	for (int i = 0; i < 10; i++) {
		uint8_t b = bytes[i];
		for (int k = 0; k < 8; k++) { int c = (crc ^ b) & 1; crc >>= 1; b >>= 1; if (c) crc ^= 0x8408; }
	}
	return (uint16_t)~crc;
}

aero_t *aero_new(int bit_rate, aero_acars_cb on_acars, aero_data_cb on_data, void *user) {
	init_tables();
	aero_t *a = calloc(1, sizeof *a);
	a->bit_rate = bit_rate; a->cols = bit_rate >= 1200 ? 9 : 6;
	a->on_acars = on_acars; a->on_data = on_data; a->user = user;
	a->next = -1; a->stats.ber = -1;
	return a;
}

void aero_free(aero_t *a) {
	if (!a) return;
	for (int i = 0; i < a->nfrags; i++) free(a->frags[i].msg.text);
	free(a->s1); free(a->s2); free(a);
}

aero_stats_t aero_stats(const aero_t *a) { return a->stats; }

static int odd_parity(int b) { b ^= b >> 4; b ^= b >> 2; b ^= b >> 1; return b & 1; }

static void push(float **buf, size_t *cap, int len, float v) {
	if ((size_t)len + 1 > *cap) { *cap = *cap ? *cap * 2 : 16384; *buf = realloc(*buf, *cap * sizeof(float)); }
	(*buf)[len] = v;
}

static void trim(aero_t *a, int count) {
	if (count <= 0) return;
	if (count > a->len) count = a->len;
	memmove(a->s1, a->s1 + count, (size_t)(a->len - count) * sizeof(float));
	if (a->s2) memmove(a->s2, a->s2 + count, (size_t)(a->len - count) * sizeof(float));
	a->len -= count; a->base += count;
	if (a->next >= 0 && a->next < a->base) a->next = -1;
}

// +1 / -1 when the preamble (or its inverse) starts at p, else 0.
static int polarity(const float *s, int p) {
	int m = 0;
	for (int i = 0; i < PREAMBLE_BITS; i++) if ((s[p + i] > 0 ? 1 : 0) == PRE[i]) m++;
	return m >= PREAMBLE_BITS - SYNC_TOLERANCE ? 1 : m <= SYNC_TOLERANCE ? -1 : 0;
}

// ---- ACARS reassembly, shared by both rates ----

static char next_block(char b) { return (char)(((b + 1 - 65) % 26 + 26) % 26 + 65); }

static void emit(aero_t *a, aero_acars_t *msg) {
	a->stats.messages++;
	if (a->on_acars) a->on_acars(msg, a->user);
}

// Multi-block messages (ETB): join blocks with consecutive block IDs.
static void defragment(aero_t *a, aero_acars_t *msg) {
	for (int i = 0; i < a->nfrags;) {
		if (++a->frags[i].age > 30) { free(a->frags[i].msg.text); a->frags[i] = a->frags[--a->nfrags]; }
		else i++;
	}
	for (int i = 0; i < a->nfrags; i++) {
		aero_acars_t *f = &a->frags[i].msg;
		if (strcmp(f->reg, msg->reg) || strcmp(f->label, msg->label) || f->mode != msg->mode ||
		    strcmp(f->aes, msg->aes) || strcmp(f->ges, msg->ges) || f->tak != msg->tak || next_block(f->block) != msg->block) continue;
		size_t n = strlen(f->text), m = strlen(msg->text);
		if (n + m + 1 > a->frags[i].textcap) { a->frags[i].textcap = n + m + 1; f->text = realloc(f->text, n + m + 1); }
		memcpy(f->text + n, msg->text, m + 1);
		f->block = msg->block; f->more = msg->more; a->frags[i].age = 0;
		if (msg->more) return;
		aero_acars_t done = *f;
		a->frags[i] = a->frags[--a->nfrags];
		emit(a, &done);
		free(done.text);
		return;
	}
	if (msg->more) {
		if (a->nfrags == MAX_FRAGMENTS) { free(a->frags[0].msg.text); a->frags[0] = a->frags[--a->nfrags]; }
		fragment_t *f = &a->frags[a->nfrags++];
		f->msg = *msg; f->msg.text = strdup(msg->text); f->textcap = strlen(msg->text) + 1; f->age = 0;
		return;
	}
	emit(a, msg);
}

static void complete(aero_t *a, const isu_t *isu) {
	const uint8_t *d = isu->data;
	const int len = isu->len;
	if (!isu->aes) return;
	char aes[12], ges[4];
	snprintf(aes, sizeof aes, "%06X", (unsigned)(isu->aes & 0xffffff)); snprintf(ges, sizeof ges, "%02X", isu->ges);
	// ACARS: FF FF SOH mode reg[7] TAK label[2] BI STX text… ETX|ETB BCS BCS DEL (odd parity per byte).
	if (len > 16 && d[0] == 0xff && d[1] == 0xff && (d[15] == 0x83 || d[15] == 0x02)) {
		for (int k = 4; k < 11; k++) if (!odd_parity(d[k])) return;
		aero_acars_t msg = {0};
		memcpy(msg.aes, aes, 7); memcpy(msg.ges, ges, 3);  // 6 and 2 hex digits.
		msg.mode = (char)(d[3] & 0x7f);
		for (int k = 0; k < 7; k++) msg.reg[k] = (char)(d[4 + k] & 0x7f);
		msg.tak = (char)(d[11] & 0x7f);
		// ACARS writes a DEL (0x7F) in a label as 'd', e.g. label "_d".
		for (int k = 0; k < 2; k++) { int c = d[12 + k] & 0x7f; msg.label[k] = c == 0x7f ? 'd' : (char)c; }
		msg.block = (char)(d[14] & 0x7f);
		msg.more = d[len - 4] == 0x97;
		char *text = malloc((size_t)len * 5 + 1); int t = 0;
		if (d[15] == 0x02) {
			for (int k = 16; k < len - 4; k++) {
				if (!odd_parity(d[k])) { free(text); return; }
				int c = d[k] & 0x7f;
				if (c == 0x7f) { memcpy(text + t, "<DEL>", 5); t += 5; } else text[t++] = (char)c;
			}
		}
		text[t] = 0; msg.text = text;
		defragment(a, &msg);
		free(text);
		return;
	}
	if (a->on_data) a->on_data(aes, ges, d, len, a->user);
}

static void isu_append(isu_t *isu, const uint8_t *b, int n) {
	if (isu->len + n > ISU_DATA_MAX) n = ISU_DATA_MAX - isu->len;
	memcpy(isu->data + isu->len, b, n); isu->len += n;
}

// ISU (0x71) opens a message for an aircraft; SSUs (0xC0..0xFF) carry the rest, counting down.
static void signal_unit(aero_t *a, const uint8_t *su) {
	int type = su[0];
	if (type == 0x71) {
		isu_t isu = {0};
		isu.aes = ((uint32_t)su[1] << 16) | (su[2] << 8) | su[3]; isu.ges = su[4];
		isu.qno = su[5] >> 4; isu.refno = su[5] & 15; isu.seqno = su[6] & 0x3f; isu.last_count = su[7] >> 4;
		isu.data[0] = su[8]; isu.data[1] = su[9]; isu.len = 2;
		if (isu.last_count > 8) return;
		for (int i = 0; i < a->nisus;) {
			isu_t *x = &a->isus[i];
			x->age++;
			if (x->age > 10 || (x->aes == isu.aes && x->ges == isu.ges && x->qno == isu.qno && x->refno == isu.refno))
				a->isus[i] = a->isus[--a->nisus];
			else i++;
		}
		if (a->nisus == MAX_ISUS) a->isus[0] = a->isus[--a->nisus];
		a->isus[a->nisus++] = isu;
		// SSUs carry no aircraft ID; like JAERO, match them to the latest ISU's AES/GES.
		a->have_last = 1; a->last_aes = isu.aes; a->last_ges = isu.ges;
	} else if ((type & 0xc0) == 0xc0) {
		int seqno = type & 0x3f, qno = su[1] >> 4, refno = su[1] & 15;
		if (!a->have_last) return;
		for (int i = 0; i < a->nisus; i++) {
			isu_t *x = &a->isus[i];
			if (x->aes != a->last_aes || x->ges != a->last_ges || x->seqno != seqno + 1 || x->qno != qno || x->refno != refno) continue;
			x->seqno--;
			if (x->seqno == 0) {
				isu_t done = *x;
				isu_append(&done, su + 2, done.last_count);
				a->isus[i] = a->isus[--a->nisus];
				complete(a, &done);
			} else isu_append(x, su + 2, 8);
			return;
		}
	}
}

// Descramble the decoded bits (LSB first into bytes) and check each 12-byte signal unit.
static void signal_units(aero_t *a, const uint8_t *info, int nbits) {
	uint8_t bytes[H_CODED / 16] = {0};
	for (int i = 0; i < nbits; i++) if (info[i] ^ PN[i]) bytes[i >> 3] |= (uint8_t)(1 << (i & 7));
	for (int su = 0; su < nbits / 8 / SU_BYTES; su++) {
		const uint8_t *o = bytes + su * SU_BYTES;
		uint16_t rec = (uint16_t)((o[11] << 8) | o[10]);
		int ok = rec == aero_su_crc(o);
		if (!ok && rec == 0) { ok = 1; for (int k = 0; k < 10; k++) if (o[k]) ok = 0; }  // Some units are all zero.
		if (!ok) { a->stats.su_bad++; continue; }
		a->stats.su_ok++;
		signal_unit(a, o);
	}
}

static void decode_block(aero_t *a, const float *coded, int n) {
	uint8_t *info = malloc(n / 2);
	int start = fec_viterbi(coded, n, info);
	a->stats.ber = fec_ber(coded, n, info, start);
	a->stats.frames++; a->stats.synced = 1;
	signal_units(a, info, n / 2);
	free(info);
}

static float clip2(float v) { return v > 2 ? 2 : v < -2 ? -2 : v; }

// ---- Aero-H+ ----

// QPSK carrier recovery can lock in any of four phases. Normally the preamble starts at the same
// symbol on I and Q and bits run I, Q. After a 90° rotation the true I stream arrives on Q and the
// true Q stream on I one symbol later, so bits run Q[k], I[k+1].
typedef struct { const float *first, *second; int lag, pf, ps; } arrangement_t;
static int arrangement(const aero_t *a, int p, arrangement_t *r) {
	int pi = polarity(a->s1, p), pq = polarity(a->s2, p);
	if (pi && pq) { *r = (arrangement_t){a->s1, a->s2, 0, pi, pq}; return 1; }
	int pi1 = polarity(a->s1, p + 1);
	if (pq && pi1) { *r = (arrangement_t){a->s2, a->s1, 1, pq, pi1}; return 1; }
	return 0;
}

static void frame_h(aero_t *a, int p, const arrangement_t *r) {
	// Bits after the preamble alternate between the two streams; polarity comes from the preamble.
	float bits[2 * H_FRAME_SYMBOLS], coded[H_CODED];
	const int o = p + PREAMBLE_BITS;
	for (int k = 0; k < H_FRAME_SYMBOLS; k++) {
		bits[2 * k] = r->pf * r->first[o + k]; bits[2 * k + 1] = r->ps * r->second[o + k + r->lag];
	}
	for (int j = 0; j < H_COLS; j++) for (int i = 0; i < ROWS; i++)
		coded[j * ROWS + i] = clip2(bits[H_START + ((i * 27) % ROWS) * H_COLS + j]);
	decode_block(a, coded, H_CODED);
}

void aero_feed_symbols(aero_t *a, const float *iq, int n) {
	for (int k = 0; k < n; k++) {
		push(&a->s1, &a->cap1, a->len, iq[2 * k]);
		push(&a->s2, &a->cap2, a->len, iq[2 * k + 1]);
		a->len++;
	}
	arrangement_t r;
	for (;;) {
		int len = a->len;
		if (a->next < 0) {
			int found = -1;
			for (int p = 0; p + PREAMBLE_BITS + 1 <= len; p++) if (arrangement(a, p, &r)) { found = p; break; }
			if (found < 0) { trim(a, len - PREAMBLE_BITS); return; }
			a->next = a->base + found;
		}
		int p = (int)(a->next - a->base);
		if (p + PREAMBLE_BITS + H_FRAME_SYMBOLS + 2 > len) { trim(a, p - 1); return; }
		// Allow a symbol of timing slip either side of the expected position.
		int at = -1;
		for (int d = 0; d < 3 && at < 0; d++) {
			int q = p + (d == 0 ? 0 : d == 1 ? -1 : 1);
			if (q >= 0 && arrangement(a, q, &r)) at = q;
		}
		if (at < 0) { a->next = -1; a->stats.synced = 0; trim(a, p + 1); continue; }
		frame_h(a, at, &r);
		a->next = a->base + at + H_FRAME_SYMBOLS;
	}
}

// ---- Aero-L ----

static void frame_l(aero_t *a, int p, int pol) {
	float coded[L_CODED];
	const float *d = a->s1 + p + PREAMBLE_BITS + 16;
	const int block = a->cols * ROWS;
	for (int b = 0; b < L_CODED; b += block)
		for (int j = 0; j < a->cols; j++) for (int i = 0; i < ROWS; i++)
			coded[b + j * ROWS + i] = pol * d[b + ((i * 27) % ROWS) * a->cols + j];
	decode_block(a, coded, L_CODED);
}

void aero_feed_bits(aero_t *a, const float *bits, int n) {
	for (int k = 0; k < n; k++) push(&a->s1, &a->cap1, a->len++, bits[k]);
	for (;;) {
		int len = a->len;
		if (a->next < 0) {
			int found = -1;
			for (int p = 0; p + PREAMBLE_BITS <= len; p++) if (polarity(a->s1, p)) { found = p; break; }
			if (found < 0) { trim(a, len - PREAMBLE_BITS); return; }
			a->next = a->base + found;
		}
		int p = (int)(a->next - a->base);
		if (p + L_FRAME_BITS + 2 > len) { trim(a, p - 2); return; }
		static const int slips[5] = {0, -1, 1, -2, 2};
		int at = -1, pol = 0;
		for (int i = 0; i < 5 && at < 0; i++) {
			int q = p + slips[i];
			if (q >= 0 && (pol = polarity(a->s1, q))) at = q;
		}
		if (at < 0) { a->next = -1; a->stats.synced = 0; trim(a, p + 1); continue; }
		frame_l(a, at, pol);
		a->next = a->base + at + L_FRAME_BITS;
	}
}
