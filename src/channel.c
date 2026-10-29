// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Channels and their JSON records. The record shape is the SDR Workbench message log's
// (log/records.js, v1), so the same tools read both.
#include "channel.h"
#include "content.h"
#include "aero.h"
#include "apps.h"
#include "demod.h"
#include "out.h"
#include "sha256.h"
#include "stdc.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECENT 256
#define RECENT_MS (60 * 60 * 1000.0)

struct channel {
	channel_kind_t kind; double freq_hz; channel_opts_t opts;
	char sat[24]; int found;
	psk_t *psk; msk_t *msk;
	aero_t *aero;
	stdc_t *stdc_diff, *stdc_coh; float prev_re, prev_im;
	float *tmp; size_t tmpcap;
	double now;
	int have_station, les; char station[48];
	// EGC messages already emitted: both STD-C paths report the same message.
	struct { char key[96]; double at; } recent[RECENT]; int recent_next;
	long egc_count;
	long last_frames; double progress_ms;   // Aero: frames at the last change, and when (sample time).
	int fails; double rest_until;            // Aero: listening windows without a frame; resting until.
	unsigned long tally_n[256], tally_b[256]; double tally_frame_ms;  // STD-C packet tally (metadata).
	// Satellite votes for a channel found by --scan (sat "?"): aircraft also heard on a known one.
	struct { char sat[24]; int n; } votes[4]; int nvotes, total_votes; char last_voter[8];
};

static pthread_mutex_t apps_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- Naming the satellite of a channel --scan found ----
// STD-C: every NCS and LES carrier announces its ocean region in its bulletin board. A plan channel
// (satellite known) teaches which satellite reports which region; a found channel reporting the
// same region is on that satellite.
// Aero: an aircraft is logged on to one satellite at a time, so a found carrier is on the satellite
// its aircraft were recently heard on through channels whose satellite is known. It is named once
// at least SAT_VOTES different aircraft say so and at least 80 % of all votes agree.
#define SAT_VOTES 3
#define AES_RECENT_MS (60 * 60 * 1000.0)
#define AES_SLOTS 4096
static pthread_mutex_t sat_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char region[16], sat[24]; } regions[16]; static int nregions;
static struct { char aes[8], sat[24]; double t; } aes_sat[AES_SLOTS];

static int unknown_sat(const channel_t *c) { return !strcmp(c->sat, "?"); }

static void set_sat(channel_t *c, const char *sat, const char *why) {
	char tmp[24]; snprintf(tmp, sizeof tmp, "%s", sat);
	memcpy(c->sat, tmp, sizeof tmp);
	fprintf(stderr, "satellite: %.4f MHz (%s) is on %s — %s\n", c->freq_hz / 1e6, channel_kind_name(c->kind), tmp, why);
}

static void learn_region(channel_t *c, const char *region) {
	pthread_mutex_lock(&sat_lock);
	int i;
	for (i = 0; i < nregions; i++) if (!strcmp(regions[i].region, region)) break;
	if (!unknown_sat(c) && !c->found) {
		if (i == nregions && nregions < 16) nregions++;
		if (i < 16) { snprintf(regions[i].region, sizeof regions[i].region, "%s", region); snprintf(regions[i].sat, sizeof regions[i].sat, "%s", c->sat); }
	} else if (unknown_sat(c) && i < nregions) {
		char why[64]; snprintf(why, sizeof why, "its bulletin board says %s, as %s's NCS does", region, regions[i].sat);
		set_sat(c, regions[i].sat, why);
	}
	pthread_mutex_unlock(&sat_lock);
}

static unsigned aes_slot(const char *aes) { unsigned h = 2166136261u; for (; *aes; aes++) h = (h ^ (unsigned char)*aes) * 16777619u; return h % AES_SLOTS; }

static void learn_aircraft(channel_t *c, const char *aes) {
	pthread_mutex_lock(&sat_lock);
	unsigned k = aes_slot(aes);
	if (!unknown_sat(c) && !c->found) {
		// One slot per hash: a collision only forgets an older aircraft.
		snprintf(aes_sat[k].aes, sizeof aes_sat[k].aes, "%s", aes); snprintf(aes_sat[k].sat, sizeof aes_sat[k].sat, "%s", c->sat);
		aes_sat[k].t = c->now;
	} else if (unknown_sat(c) && !strcmp(aes_sat[k].aes, aes) && c->now - aes_sat[k].t < AES_RECENT_MS && strcmp(c->last_voter, aes)) {
		snprintf(c->last_voter, sizeof c->last_voter, "%s", aes);
		int i;
		for (i = 0; i < c->nvotes; i++) if (!strcmp(c->votes[i].sat, aes_sat[k].sat)) break;
		if (i == c->nvotes && c->nvotes < 4) { snprintf(c->votes[i].sat, sizeof c->votes[i].sat, "%s", aes_sat[k].sat); c->votes[i].n = 0; c->nvotes++; }
		if (i < 4) c->votes[i].n++;
		c->total_votes++;
		for (i = 0; i < c->nvotes; i++)
			if (c->votes[i].n >= SAT_VOTES && c->votes[i].n * 5 >= c->total_votes * 4) {
				char why[80]; snprintf(why, sizeof why, "%d of %d aircraft checked were last heard on %s", c->votes[i].n, c->total_votes, c->votes[i].sat);
				set_sat(c, c->votes[i].sat, why);
				break;
			}
	}
	pthread_mutex_unlock(&sat_lock);
}

int channel_kind_parse(const char *s) {
	if (!strcmp(s, "stdc")) return CH_STDC;
	if (!strcmp(s, "aero") || !strcmp(s, "aeroh") || !strcmp(s, "aero10500")) return CH_AERO_H;
	if (!strcmp(s, "aerol600") || !strcmp(s, "aerol")) return CH_AERO_L600;
	if (!strcmp(s, "aerol1200")) return CH_AERO_L1200;
	return -1;
}

const char *channel_kind_name(channel_kind_t k) {
	static const char *names[] = {"stdc", "aero", "aerol600", "aerol1200"};
	return names[k];
}

static int aero_rate(channel_kind_t k) { return k == CH_AERO_L600 ? 600 : k == CH_AERO_L1200 ? 1200 : 10500; }

static void record_head(jbuf_t *b, const channel_t *c, const char *key, const char *decoder, const char *type, int link) {
	jb_raw(b, "{\"v\":1");
	jb_key(b, "key"); jb_str(b, key);
	jb_key(b, "decoder"); jb_str(b, decoder);
	jb_key(b, "type"); jb_str(b, type);
	jb_key(b, "link"); jb_raw(b, link ? "true" : "false");
	jb_key(b, "firstSeen"); jb_printf(b, "%.0f", c->now);
	jb_key(b, "lastSeen"); jb_printf(b, "%.0f", c->now);
	jb_key(b, "count"); jb_raw(b, "1");
	jb_key(b, "source"); jb_raw(b, "{"); jb_key(b, "kind"); jb_str(b, c->opts.source_kind);
	jb_key(b, "name"); jb_str(b, c->opts.source_name); jb_raw(b, "}");
	jb_key(b, "freqHz"); jb_printf(b, "%.0f", c->freq_hz);
}

static void on_acars(const aero_acars_t *m, void *user) {
	channel_t *c = user;
	int ack = !strcmp(m->label, "_d");
	learn_aircraft(c, m->aes);
	apps_t apps; int has_app = 0;
	if (m->text[0]) {
		pthread_mutex_lock(&apps_lock);  // libacars is not documented as thread-safe.
		has_app = apps_decode(m->label, m->text, 1, &apps);
		pthread_mutex_unlock(&apps_lock);
	}
	const char *reg = m->reg;
	while (*reg == '.') reg++;
	char block[2] = {m->block, 0};
	events_acars(&(ev_acars_t){.t_ms = c->now, .freq = c->freq_hz, .sat = c->sat, .type = channel_kind_name(c->kind),
		.aes = m->aes, .ges = m->ges, .reg = reg, .label = m->label, .text = m->text, .block = m->block, .mode = m->mode,
		.more = m->more, .ack = ack, .app_kind = has_app ? apps.kind : NULL, .app_summary = has_app ? apps.summary : NULL,
		.tree_json = has_app ? apps.json : NULL, .app_error = has_app && apps.error});
	if (ack && c->opts.hide_acks) { if (has_app) apps_free(&apps); return; }
	char key[96], h[8], to[32], from[16];
	fnv36(m->text, h);
	snprintf(key, sizeof key, "aero:%s:%s:%c:%s", m->aes, m->label, m->block, h);
	snprintf(from, sizeof from, "GES %s", m->ges);
	if (*reg) snprintf(to, sizeof to, "%s", reg); else snprintf(to, sizeof to, "AES %s", m->aes);
	jbuf_t b = {0};
	record_head(&b, c, key, "aero", "acars", ack || (!m->text[0] && !has_app));
	jb_key(&b, "aircraft"); jb_raw(&b, "{"); jb_key(&b, "reg"); jb_str(&b, *reg ? reg : NULL);
	jb_key(&b, "flight"); jb_raw(&b, "null"); jb_key(&b, "address"); jb_str(&b, m->aes); jb_raw(&b, "}");
	jb_key(&b, "ground"); jb_raw(&b, "{"); jb_key(&b, "id"); jb_str(&b, m->ges); jb_key(&b, "name"); jb_raw(&b, "null}");
	// P-channel traffic is ground to air.
	jb_key(&b, "from"); jb_str(&b, from);
	jb_key(&b, "to"); jb_str(&b, to);
	jb_key(&b, "label"); jb_str(&b, m->label);
	jb_key(&b, "block"); jb_str(&b, block);
	jb_key(&b, "app");
	if (has_app) {
		jb_raw(&b, "{"); jb_key(&b, "kind"); jb_str(&b, apps.kind); jb_key(&b, "summary"); jb_str(&b, apps.summary);
		jb_key(&b, "error"); jb_raw(&b, apps.error ? "true}" : "false}");
	} else jb_raw(&b, "null");
	jb_key(&b, "position");
	if (has_app && apps.has_pos) {
		jb_printf(&b, "{\"lat\":%.6f,\"lon\":%.6f", apps.lat, apps.lon);
		if (apps.has_alt) jb_printf(&b, ",\"alt\":%.0f", apps.alt);
		jb_raw(&b, "}");
	} else jb_raw(&b, "null");
	jb_key(&b, "text"); jb_str(&b, m->text);
	jb_key(&b, "decode");
	if (has_app) { jb_raw(&b, "{"); jb_key(&b, "tree"); jb_raw(&b, apps.json); jb_key(&b, "text"); jb_str(&b, apps.text); jb_raw(&b, "}"); }
	else jb_raw(&b, "null");
	char mode[2] = {m->mode, 0}, tak[2] = {m->tak, 0};
	jb_key(&b, "meta"); jb_raw(&b, "{"); jb_key(&b, "mode"); jb_str(&b, mode); jb_key(&b, "tak"); jb_str(&b, tak);
	jb_key(&b, "rate"); jb_printf(&b, "%d", aero_rate(c->kind));
	jb_key(&b, "sat"); jb_str(&b, c->sat); jb_key(&b, "channel"); jb_str(&b, channel_kind_name(c->kind));
	jb_key(&b, "more"); jb_raw(&b, m->more ? "true" : "false");
	jb_raw(&b, "}}");
	out_emit(b.s);
	jb_free(&b);
	if (has_app) apps_free(&apps);
}

static void on_aero_data(const char *aes, const char *ges, const uint8_t *d, int len, void *user) {
	channel_t *c = user;
	if (!c->opts.emit_data) return;
	char *hex = malloc((size_t)len * 3 + 1); int n = 0;
	for (int i = 0; i < len; i++) n += sprintf(hex + n, "%s%02x", i ? " " : "", d[i]);
	char key[64], h[8]; fnv36(hex, h);
	snprintf(key, sizeof key, "aero:data:%s:%s", aes, h);
	jbuf_t b = {0};
	record_head(&b, c, key, "aero", "data", 0);
	jb_key(&b, "aircraft"); jb_raw(&b, "{"); jb_key(&b, "reg"); jb_raw(&b, "null"); jb_key(&b, "flight"); jb_raw(&b, "null");
	jb_key(&b, "address"); jb_str(&b, aes); jb_raw(&b, "}");
	jb_key(&b, "ground"); jb_raw(&b, "{"); jb_key(&b, "id"); jb_str(&b, ges); jb_key(&b, "name"); jb_raw(&b, "null}");
	jb_key(&b, "text"); jb_str(&b, hex);
	jb_key(&b, "meta"); jb_printf(&b, "{\"rate\":%d}}", aero_rate(c->kind));
	out_emit(b.s);
	jb_free(&b); free(hex);
}

static void on_egc(const stdc_egc_t *e, void *user) {
	channel_t *c = user;
	char key[64], h[8], dedupe[96];
	snprintf(key, sizeof key, "stdc:egc:%d:%d", e->service, e->id);
	fnv36(e->text, h);
	snprintf(dedupe, sizeof dedupe, "%s:%s", key, h);
	for (int i = 0; i < RECENT; i++)
		if (c->recent[i].at && c->now - c->recent[i].at < RECENT_MS && !strcmp(c->recent[i].key, dedupe)) return;
	snprintf(c->recent[c->recent_next].key, sizeof c->recent[0].key, "%s", dedupe);
	c->recent[c->recent_next].at = c->now; c->recent_next = (c->recent_next + 1) % RECENT;
	c->egc_count++;
	events_egc(&(ev_egc_t){.t_ms = c->now, .freq = c->freq_hz, .sat = c->sat, .les = c->have_station ? c->les : -1,
		.station = c->have_station ? c->station : NULL, .service = e->service, .id = e->id, .parts = e->parts,
		.complete = e->complete, .repetition = e->repetition, .service_name = e->service_name, .priority = e->priority,
		.text = e->text});
	jbuf_t b = {0};
	record_head(&b, c, key, "stdc", "egc", 0);
	jb_key(&b, "aircraft"); jb_raw(&b, "null");
	jb_key(&b, "ground");
	if (c->have_station) { jb_printf(&b, "{\"id\":%d", c->les); jb_key(&b, "name"); jb_str(&b, c->station); jb_raw(&b, "}"); }
	else jb_raw(&b, "null");
	jb_key(&b, "from"); jb_str(&b, c->have_station ? c->station : "");
	jb_key(&b, "to"); jb_str(&b, "");
	jb_key(&b, "label"); jb_str(&b, "");
	jb_key(&b, "block"); jb_str(&b, "");
	jb_key(&b, "service"); jb_str(&b, e->service_name);
	jb_key(&b, "priority"); jb_str(&b, e->priority);
	jb_key(&b, "egcId"); jb_printf(&b, "%d", e->id);
	jb_key(&b, "parts"); jb_printf(&b, "%d", e->parts);
	jb_key(&b, "complete"); jb_raw(&b, e->complete ? "true" : "false");
	jb_key(&b, "app"); jb_raw(&b, "null");
	jb_key(&b, "position"); jb_raw(&b, "null");
	jb_key(&b, "text"); jb_str(&b, e->text);
	jb_key(&b, "decode"); jb_raw(&b, "null");
	jb_key(&b, "meta"); jb_printf(&b, "{\"serviceCode\":%d,\"repetition\":%d,\"sat\":", e->service, e->repetition);
	jb_str(&b, c->sat); jb_raw(&b, "}}");
	out_emit(b.s);
	jb_free(&b);
}

// Both STD-C detectors may decode the same 8.64 s frame: count a frame once, from whichever
// finishes it first, and ignore a second report within a few seconds.
static unsigned char mobile_key[64]; static int mobile_key_len;
void channel_set_mobile_key(const unsigned char *key, int len) {
	mobile_key_len = len > 64 ? 64 : len;
	memcpy(mobile_key, key, mobile_key_len);
}

// A ship's mobile number as a keyed hash: the same ship always gives the same 10 hex digits on this
// receiver, but the number cannot be recovered or matched without the key (a plain hash would not
// do: there are only 16 million possible numbers).
static void mobile_hash(unsigned mes, char out[11]) {
	unsigned char m[3] = {mes >> 16, mes >> 8, mes}, h[32];
	hmac_sha256(mobile_key, mobile_key_len, m, 3, h);
	for (int i = 0; i < 5; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

static void on_stdc_frame(const unsigned *counts, const unsigned *bytes, const stdc_sig_t *sigs, int nsigs,
	const stdc_msgpkt_t *msgs, int nmsgs, void *user) {
	channel_t *c = user;
	if (c->tally_frame_ms && c->now - c->tally_frame_ms < 4000) return;
	c->tally_frame_ms = c->now;
	for (int d = 0; d < 256; d++) { c->tally_n[d] += counts[d]; c->tally_b[d] += bytes[d]; }
	for (int i = 0; i < nsigs; i++) {
		const stdc_sig_t *g = &sigs[i];
		char ship[11] = "";
		if (g->has_mes && mobile_key_len) mobile_hash(g->mes, ship);
		if (content_enabled() && (g->desc == 0x81 || g->desc == 0x27 || g->desc == 0x83 || g->desc == 0x2a))
			content_signal(c->now, c->freq_hz, c->sat, g->desc, ship[0] ? ship : NULL, g->les, g->lc, g->pres);
		if (g->has_mes && !mobile_key_len) continue;  // No key: never record a number, hashed or not.
		events_stdc_sig(&(ev_stdc_sig_t){.t_ms = c->now, .freq = c->freq_hz, .sat = c->sat,
			.chan_les = c->have_station ? c->les : -1, .desc = g->desc, .ship = g->has_mes ? ship : NULL,
			.les = g->les, .lc = g->lc, .pres = g->pres, .pkt_no = g->pkt_no, .payload = g->payload});
	}
	for (int i = 0; i < nmsgs && content_enabled(); i++)
		content_packet(c->now, c->freq_hz, c->sat, msgs[i].les, msgs[i].lc, msgs[i].pkt_no, msgs[i].data, msgs[i].len);
}

int channel_take_tally(channel_t *c, unsigned long counts[256], unsigned long bytes[256]) {
	if (c->kind != CH_STDC) return 0;
	int any = 0;
	for (int d = 0; d < 256; d++) { counts[d] = c->tally_n[d]; bytes[d] = c->tally_b[d]; any |= counts[d] != 0; }
	memset(c->tally_n, 0, sizeof c->tally_n); memset(c->tally_b, 0, sizeof c->tally_b);
	return any;
}

static void on_bulletin(const stdc_bulletin_t *e, void *user) {
	channel_t *c = user;
	c->have_station = 1; c->les = e->les;
	snprintf(c->station, sizeof c->station, "%s · LES %d", e->satellite, e->les);
	learn_region(c, e->satellite);
	if (!c->opts.emit_bulletins) return;
	char key[48]; snprintf(key, sizeof key, "stdc:bb:%d", e->frame_number);
	jbuf_t b = {0};
	record_head(&b, c, key, "stdc", "bulletin", 1);
	jb_key(&b, "ground"); jb_printf(&b, "{\"id\":%d", e->les); jb_key(&b, "name"); jb_str(&b, c->station); jb_raw(&b, "}");
	jb_key(&b, "meta"); jb_printf(&b, "{\"frameNumber\":%d,\"utcSeconds\":%.2f,\"satellite\":", e->frame_number, e->utc_seconds);
	jb_str(&b, e->satellite); jb_printf(&b, ",\"version\":%d}}", e->version);
	out_emit(b.s);
	jb_free(&b);
}

channel_t *channel_new(channel_kind_t kind, double freq_hz, const char *sat, int found, double center_hz, double rate,
	const channel_opts_t *opts) {
	channel_t *c = calloc(1, sizeof *c);
	c->kind = kind; c->freq_hz = freq_hz; c->opts = *opts; c->found = found;
	snprintf(c->sat, sizeof c->sat, "%s", sat && *sat ? sat : "?");
	double offset = freq_hz - center_hz;
	switch (kind) {
	case CH_STDC:
		c->psk = psk_new(PSK_BPSK, 1200, rate, offset, 1);
		c->stdc_diff = stdc_new(on_egc, on_bulletin, c);
		c->stdc_coh = stdc_new(on_egc, on_bulletin, c);
		stdc_set_frame_cb(c->stdc_diff, on_stdc_frame); stdc_set_frame_cb(c->stdc_coh, on_stdc_frame);
		stdc_set_content(c->stdc_diff, opts->content); stdc_set_content(c->stdc_coh, opts->content);
		break;
	case CH_AERO_H:
		// Aero frame sync judges lock itself, so the demodulator keeps tracking instead of re-searching.
		c->psk = psk_new(PSK_OQPSK, 5250, rate, offset, 0);
		c->aero = aero_new(10500, on_acars, on_aero_data, c);
		break;
	case CH_AERO_L600: case CH_AERO_L1200:
		c->msk = msk_new(aero_rate(kind), rate, offset, 0);
		c->aero = aero_new(aero_rate(kind), on_acars, on_aero_data, c);
		break;
	}
	if (c->aero) aero_set_raw_blocks(c->aero, opts->raw_blocks);
	return c;
}

void channel_free(channel_t *c) {
	if (!c) return;
	psk_free(c->psk); msk_free(c->msk); aero_free(c->aero);
	stdc_free(c->stdc_diff); stdc_free(c->stdc_coh); free(c->tmp); free(c);
}

// An Aero channel whose carrier search settled on the wrong peak (a weak carrier next to a
// stronger one) finds no frames there. After this long without one it searches again.
#define AERO_H_RESEARCH_MS 20000.0
#define AERO_L_RESEARCH_MS 40000.0
// A carrier that is absent or too weak here would keep its demodulator busy for nothing. After
// AERO_FAILS_BEFORE_REST windows in a row without a frame it rests (processes no samples), for
// AERO_REST_MS, doubling each time it wakes and still finds nothing, up to AERO_REST_MAX_MS. Any
// frame resets it. P-channels transmit continuously, so a rested channel misses little.
#define AERO_FAILS_BEFORE_REST 3
#define AERO_REST_MS 60000.0
#define AERO_REST_MAX_MS 300000.0

void channel_process(channel_t *c, const float *iq, int n, double now_ms) {
	if (!c->progress_ms) c->progress_ms = now_ms;
	c->now = now_ms;
	if (c->aero) {
		if (now_ms < c->rest_until) return;
		long frames = aero_stats(c->aero).frames;
		demod_status_t d = c->msk ? msk_status(c->msk) : psk_status(c->psk);
		if (frames != c->last_frames) { c->last_frames = frames; c->progress_ms = now_ms; c->fails = 0; }
		else if (!d.acquired) c->progress_ms = now_ms;
		else if (now_ms - c->progress_ms > (c->msk ? AERO_L_RESEARCH_MS : AERO_H_RESEARCH_MS)) {
			if (c->msk) msk_restart(c->msk); else psk_restart(c->psk);
			c->progress_ms = now_ms;
			if (++c->fails >= AERO_FAILS_BEFORE_REST) {
				double rest = AERO_REST_MS * (double)(1 << (c->fails - AERO_FAILS_BEFORE_REST < 8 ? c->fails - AERO_FAILS_BEFORE_REST : 8));
				if (rest > AERO_REST_MAX_MS) rest = AERO_REST_MAX_MS;
				c->rest_until = now_ms + rest;
				c->progress_ms = c->rest_until;  // It wakes to a full listening window.
				return;
			}
		}
	}
	const float *s;
	if (c->msk) {
		int k = msk_process(c->msk, iq, n, &s);
		if (k) aero_feed_bits(c->aero, s, k);
	} else if (c->aero) {
		int k = psk_process(c->psk, iq, n, &s);
		if (k) aero_feed_symbols(c->aero, s, k);
	} else {
		int k = psk_process(c->psk, iq, n, &s);
		if (!k) return;
		// Two detectors side by side: differential, Re(s[k]·conj(s[k-1])), and coherent, Re(s[k]).
		c->tmp = dsp_reserve(c->tmp, &c->tmpcap, (size_t)2 * k, sizeof(float));
		float *diff = c->tmp, *coh = c->tmp + k;
		for (int i = 0; i < k; i++) {
			float r = s[2 * i], q = s[2 * i + 1];
			diff[i] = r * c->prev_re + q * c->prev_im; coh[i] = r;
			c->prev_re = r; c->prev_im = q;
		}
		stdc_feed(c->stdc_diff, diff, k, now_ms);
		stdc_feed(c->stdc_coh, coh, k, now_ms);
	}
}

void channel_flush(channel_t *c) {
	if (c->stdc_diff) { stdc_flush(c->stdc_diff); stdc_flush(c->stdc_coh); }
}

void channel_status(const channel_t *c, char *buf, int size) {
	demod_status_t d = c->msk ? msk_status(c->msk) : psk_status(c->psk);
	int n = snprintf(buf, size, "%.4f %-9s %s off %+6.1f Hz snr %4.1f dB", c->freq_hz / 1e6, channel_kind_name(c->kind),
		c->now < c->rest_until ? "rest  " : !d.acquired ? "search" : d.locked ? "lock  " : "track ", d.offset_hz, d.snr_db);
	if (c->aero) {
		aero_stats_t a = aero_stats(c->aero);
		snprintf(buf + n, size - n, " | %s frames %ld ber %.1f%% SU %ld/%ld msgs %ld", a.synced ? "SYNC" : "----",
			a.frames, a.ber < 0 ? 0 : a.ber * 100, a.su_ok, a.su_bad, a.messages);
	} else {
		stdc_stats_t a = stdc_stats(c->stdc_diff), b = stdc_stats(c->stdc_coh);
		const stdc_stats_t *best = a.synced || (!b.synced && a.frames >= b.frames) ? &a : &b;
		snprintf(buf + n, size - n, " | %s frames %ld (+%ld dropped) ber %.1f%% packets %ld/%ld%s%s", best->synced ? "SYNC" : "----",
			best->frames, best->dropped, best->ber < 0 ? 0 : best->ber * 100, best->packets_ok, best->packets_bad,
			c->have_station ? " " : "", c->have_station ? c->station : "");
	}
}

double channel_freq(const channel_t *c) { return c->freq_hz; }
channel_kind_t channel_kind(const channel_t *c) { return c->kind; }

void channel_info(const channel_t *c, channel_info_t *i) {
	demod_status_t d = c->msk ? msk_status(c->msk) : psk_status(c->psk);
	*i = (channel_info_t){.freq = c->freq_hz, .sat = c->sat, .type = channel_kind_name(c->kind), .found = c->found,
		.acquired = d.acquired, .locked = d.locked, .snr_db = d.snr_db, .offset_hz = d.offset_hz, .les = -1,
		.resting = c->now < c->rest_until};
	if (c->aero) {
		aero_stats_t a = aero_stats(c->aero);
		i->synced = a.synced; i->ber = a.ber; i->frames = a.frames; i->units_ok = a.su_ok; i->units_bad = a.su_bad;
		i->messages = a.messages;
	} else {
		stdc_stats_t a = stdc_stats(c->stdc_diff), b = stdc_stats(c->stdc_coh);
		const stdc_stats_t *best = a.synced || (!b.synced && a.frames >= b.frames) ? &a : &b;
		i->synced = best->synced; i->ber = best->ber; i->frames = best->frames; i->units_ok = best->packets_ok;
		i->units_bad = best->packets_bad; i->messages = c->egc_count;
		if (c->have_station) { i->les = c->les; snprintf(i->station, sizeof i->station, "%s", c->station); }
	}
}
