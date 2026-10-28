// Channels and their JSON records. The record shape is the SDR Workbench message log's
// (log/records.js, v1), so the same tools read both.
#include "channel.h"
#include "aero.h"
#include "apps.h"
#include "demod.h"
#include "out.h"
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
	psk_t *psk; msk_t *msk;
	aero_t *aero;
	stdc_t *stdc_diff, *stdc_coh; float prev_re, prev_im;
	float *tmp; size_t tmpcap;
	double now;
	int have_station, les; char station[48];
	// EGC messages already emitted: both STD-C paths report the same message.
	struct { char key[96]; double at; } recent[RECENT]; int recent_next;
};

static pthread_mutex_t apps_lock = PTHREAD_MUTEX_INITIALIZER;

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
	if (ack && c->opts.hide_acks) return;
	apps_t apps; int has_app = 0;
	if (m->text[0]) {
		pthread_mutex_lock(&apps_lock);  // libacars is not documented as thread-safe.
		has_app = apps_decode(m->label, m->text, 1, &apps);
		pthread_mutex_unlock(&apps_lock);
	}
	const char *reg = m->reg;
	while (*reg == '.') reg++;
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
	char block[2] = {m->block, 0};
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
	jb_key(&b, "rate"); jb_printf(&b, "%d", aero_rate(c->kind)); jb_key(&b, "more"); jb_raw(&b, m->more ? "true" : "false");
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
	jb_key(&b, "meta"); jb_printf(&b, "{\"serviceCode\":%d,\"repetition\":%d}}", e->service, e->repetition);
	out_emit(b.s);
	jb_free(&b);
}

static void on_bulletin(const stdc_bulletin_t *e, void *user) {
	channel_t *c = user;
	c->have_station = 1; c->les = e->les;
	snprintf(c->station, sizeof c->station, "%s · LES %d", e->satellite, e->les);
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

channel_t *channel_new(channel_kind_t kind, double freq_hz, double center_hz, double rate, const channel_opts_t *opts) {
	channel_t *c = calloc(1, sizeof *c);
	c->kind = kind; c->freq_hz = freq_hz; c->opts = *opts;
	double offset = freq_hz - center_hz;
	switch (kind) {
	case CH_STDC:
		c->psk = psk_new(PSK_BPSK, 1200, rate, offset, 1);
		c->stdc_diff = stdc_new(on_egc, on_bulletin, c);
		c->stdc_coh = stdc_new(on_egc, on_bulletin, c);
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
	return c;
}

void channel_free(channel_t *c) {
	if (!c) return;
	psk_free(c->psk); msk_free(c->msk); aero_free(c->aero);
	stdc_free(c->stdc_diff); stdc_free(c->stdc_coh); free(c->tmp); free(c);
}

void channel_process(channel_t *c, const float *iq, int n, double now_ms) {
	c->now = now_ms;
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
		!d.acquired ? "search" : d.locked ? "lock  " : "track ", d.offset_hz, d.snr_db);
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
