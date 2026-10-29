// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// The live page (--web), following vdl2-sniffer's web.cc: state kept in memory since the receiver
// started, a small HTTP server on its own thread, one page that polls /api/state.
#include "web.h"
#include "out.h"
#include "plans.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_MSGS 300
#define MAX_AIRCRAFT 3000
#define MAX_GES 64
#define MAX_CHAN 64
#define AIRCRAFT_KEEP_S 7200

typedef struct {
	int egc; double t, freq; char sat[24], type[12];
	// ACARS
	char aes[8], ges[4], reg[12], label[4], block; int ack;
	char *text, *app_kind, *app_summary;
	// EGC
	int les, service, id, parts, complete; char station[48], service_name[48], priority[12];
} msg_t;

typedef struct {
	char aes[8], reg[12], ges[4], label[4];
	double first, last;
	long msgs, cpdlc, adsc;
	char sats[64], types[64];   // space-separated sets
} aircraft_t;

typedef struct { char id[4]; long msgs; double last; char sats[64]; } ges_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int listen_fd = -1, running, obs_port;
static char station_id[128];
static double started, latest;
static msg_t msgs[MAX_MSGS]; static int msg_head, msg_count;
static aircraft_t aircraft[MAX_AIRCRAFT]; static int naircraft;
static ges_t ges[MAX_GES]; static int nges;
static channel_info_t chans[MAX_CHAN]; static int nchans;
static long n_acars, n_acks, n_cpdlc, n_adsc, n_egc;
static struct { char name[160], gain[32]; double centre, rate; int bias; long long samples, dropped; } rx;

static double wall_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

int web_enabled(void) { return running; }

static void add_word(char *set, size_t size, const char *w) {
	char pad[80]; snprintf(pad, sizeof pad, " %s ", w);
	char hay[80]; snprintf(hay, sizeof hay, " %s ", set);
	if (strstr(hay, pad)) return;
	size_t n = strlen(set);
	snprintf(set + n, size - n, "%s%s", n ? " " : "", w);
}

static msg_t *new_msg(void) {
	msg_t *m = &msgs[msg_head];
	free(m->text); free(m->app_kind); free(m->app_summary);
	memset(m, 0, sizeof *m);
	msg_head = (msg_head + 1) % MAX_MSGS;
	if (msg_count < MAX_MSGS) msg_count++;
	return m;
}

void web_acars(const ev_acars_t *e) {
	if (!running) return;
	double t = e->t_ms / 1000;
	pthread_mutex_lock(&lock);
	if (t > latest) latest = t;
	n_acars++;
	if (e->ack) n_acks++;
	int cp = e->app_kind && !strcmp(e->app_kind, "CPDLC"), ad = e->app_kind && !strcmp(e->app_kind, "ADS-C");
	n_cpdlc += cp; n_adsc += ad;
	msg_t *m = new_msg();
	m->t = t; m->freq = e->freq;
	snprintf(m->sat, sizeof m->sat, "%s", e->sat); snprintf(m->type, sizeof m->type, "%s", e->type);
	snprintf(m->aes, sizeof m->aes, "%s", e->aes); snprintf(m->ges, sizeof m->ges, "%s", e->ges);
	snprintf(m->reg, sizeof m->reg, "%s", e->reg); snprintf(m->label, sizeof m->label, "%s", e->label);
	m->block = e->block; m->ack = e->ack; m->text = strdup(e->text);
	if (e->app_kind) { m->app_kind = strdup(e->app_kind); m->app_summary = strdup(e->app_summary ? e->app_summary : ""); }
	aircraft_t *a = NULL;
	for (int i = 0; i < naircraft; i++) if (!strcmp(aircraft[i].aes, e->aes)) { a = &aircraft[i]; break; }
	if (!a) {
		if (naircraft == MAX_AIRCRAFT) {  // Reuse the one heard longest ago.
			int old = 0; for (int i = 1; i < naircraft; i++) if (aircraft[i].last < aircraft[old].last) old = i;
			a = &aircraft[old];
		} else a = &aircraft[naircraft++];
		memset(a, 0, sizeof *a);
		snprintf(a->aes, sizeof a->aes, "%s", e->aes); a->first = t;
	}
	if (e->reg[0]) snprintf(a->reg, sizeof a->reg, "%s", e->reg);
	snprintf(a->ges, sizeof a->ges, "%s", e->ges); snprintf(a->label, sizeof a->label, "%s", e->label);
	a->last = t; a->msgs++; a->cpdlc += cp; a->adsc += ad;
	add_word(a->sats, sizeof a->sats, e->sat); add_word(a->types, sizeof a->types, e->type);
	ges_t *g = NULL;
	for (int i = 0; i < nges; i++) if (!strcmp(ges[i].id, e->ges)) { g = &ges[i]; break; }
	if (!g && nges < MAX_GES) { g = &ges[nges++]; memset(g, 0, sizeof *g); snprintf(g->id, sizeof g->id, "%s", e->ges); }
	if (g) { g->msgs++; g->last = t; add_word(g->sats, sizeof g->sats, e->sat); }
	pthread_mutex_unlock(&lock);
}

void web_egc(const ev_egc_t *e) {
	if (!running) return;
	double t = e->t_ms / 1000;
	pthread_mutex_lock(&lock);
	if (t > latest) latest = t;
	n_egc++;
	msg_t *m = new_msg();
	m->egc = 1; m->t = t; m->freq = e->freq;
	snprintf(m->sat, sizeof m->sat, "%s", e->sat); snprintf(m->type, sizeof m->type, "stdc");
	m->les = e->les; m->service = e->service; m->id = e->id; m->parts = e->parts; m->complete = e->complete;
	snprintf(m->station, sizeof m->station, "%s", e->station ? e->station : "");
	snprintf(m->service_name, sizeof m->service_name, "%s", e->service_name);
	snprintf(m->priority, sizeof m->priority, "%s", e->priority);
	m->text = strdup(e->text);
	pthread_mutex_unlock(&lock);
}

void web_channels(const channel_info_t *info, int n) {
	if (!running) return;
	pthread_mutex_lock(&lock);
	nchans = n < MAX_CHAN ? n : MAX_CHAN;
	memcpy(chans, info, nchans * sizeof *chans);
	pthread_mutex_unlock(&lock);
}

void web_receiver(const char *name, double centre, double rate, const char *gain, int bias) {
	pthread_mutex_lock(&lock);
	snprintf(rx.name, sizeof rx.name, "%s", name); snprintf(rx.gain, sizeof rx.gain, "%s", gain ? gain : "default");
	rx.centre = centre; rx.rate = rate; rx.bias = bias;
	pthread_mutex_unlock(&lock);
}

void web_input(long long samples, long long dropped) {
	pthread_mutex_lock(&lock); rx.samples = samples; rx.dropped = dropped; pthread_mutex_unlock(&lock);
}

static void state_json(jbuf_t *b) {
	double now = wall_now();
	pthread_mutex_lock(&lock);
	jb_printf(b, "{\"now\":%.1f,\"started\":%.1f,\"latest\":", now, started);
	if (latest) jb_printf(b, "%.1f", latest); else jb_raw(b, "null");
	jb_key(b, "station_id"); jb_str(b, station_id);
	jb_key(b, "observatory_port"); jb_printf(b, "%d", obs_port);
	jb_key(b, "counts"); jb_printf(b, "{\"acars\":%ld,\"acks\":%ld,\"cpdlc\":%ld,\"adsc\":%ld,\"egc\":%ld}",
		n_acars, n_acks, n_cpdlc, n_adsc, n_egc);
	jb_key(b, "receiver"); jb_raw(b, "{"); jb_key(b, "name"); jb_str(b, rx.name); jb_key(b, "gain"); jb_str(b, rx.gain);
	jb_printf(b, ",\"centre\":%.0f,\"rate\":%.0f,\"bias\":%s,\"samples\":%lld,\"dropped\":%lld}",
		rx.centre, rx.rate, rx.bias ? "true" : "false", rx.samples, rx.dropped);
	jb_key(b, "satellites"); jb_raw(b, "[");
	for (int i = 0; i < NSATELLITES; i++) {
		jb_raw(b, i ? ",{" : "{"); jb_key(b, "id"); jb_str(b, SATELLITES[i].id); jb_key(b, "name"); jb_str(b, SATELLITES[i].name);
		jb_key(b, "slot"); jb_str(b, SATELLITES[i].slot); jb_key(b, "region"); jb_str(b, SATELLITES[i].region); jb_raw(b, "}");
	}
	jb_raw(b, "]");
	jb_key(b, "channels"); jb_raw(b, "[");
	for (int i = 0; i < nchans; i++) {
		const channel_info_t *c = &chans[i];
		jb_raw(b, i ? ",{" : "{");
		jb_key(b, "freq"); jb_printf(b, "%.0f", c->freq); jb_key(b, "sat"); jb_str(b, c->sat); jb_key(b, "type"); jb_str(b, c->type);
		jb_printf(b, ",\"acquired\":%d,\"locked\":%d,\"synced\":%d,\"found\":%d,\"resting\":%d,\"snr\":%.1f,\"offset\":%.1f,\"ber\":",
			c->acquired, c->locked, c->synced, c->found, c->resting, c->snr_db, c->offset_hz);
		if (c->ber < 0) jb_raw(b, "null"); else jb_printf(b, "%.4f", c->ber);
		jb_printf(b, ",\"frames\":%ld,\"ok\":%ld,\"bad\":%ld,\"msgs\":%ld", c->frames, c->units_ok, c->units_bad, c->messages);
		if (c->les >= 0) { jb_printf(b, ",\"les\":%d", c->les); jb_key(b, "station"); jb_str(b, c->station); }
		jb_raw(b, "}");
	}
	jb_raw(b, "]");
	jb_key(b, "messages"); jb_raw(b, "[");
	for (int k = 0; k < msg_count; k++) {
		const msg_t *m = &msgs[(msg_head - 1 - k + 2 * MAX_MSGS) % MAX_MSGS];
		jb_raw(b, k ? ",{" : "{");
		jb_printf(b, "\"t\":%.1f,\"freq\":%.0f", m->t, m->freq);
		jb_key(b, "sat"); jb_str(b, m->sat); jb_key(b, "type"); jb_str(b, m->type);
		jb_key(b, "txt"); jb_str(b, m->text);
		if (m->egc) {
			jb_printf(b, ",\"egc\":1,\"service\":%d,\"id\":%d,\"parts\":%d,\"complete\":%d", m->service, m->id, m->parts, m->complete);
			jb_key(b, "service_name"); jb_str(b, m->service_name); jb_key(b, "prio"); jb_str(b, m->priority);
			jb_key(b, "station"); jb_str(b, m->station[0] ? m->station : NULL);
		} else {
			char blk[2] = {m->block, 0};
			jb_key(b, "aes"); jb_str(b, m->aes); jb_key(b, "ges"); jb_str(b, m->ges); jb_key(b, "reg"); jb_str(b, m->reg);
			jb_key(b, "lbl"); jb_str(b, m->label); jb_key(b, "blk"); jb_str(b, blk); jb_printf(b, ",\"ack\":%d", m->ack);
			if (m->app_kind) { jb_key(b, "app"); jb_str(b, m->app_kind); jb_key(b, "app_sum"); jb_str(b, m->app_summary); }
		}
		jb_raw(b, "}");
	}
	jb_raw(b, "]");
	jb_key(b, "aircraft"); jb_raw(b, "[");
	int first = 1;
	for (int i = 0; i < naircraft; i++) {
		const aircraft_t *a = &aircraft[i];
		if (latest - a->last > AIRCRAFT_KEEP_S) continue;
		jb_raw(b, first ? "{" : ",{"); first = 0;
		jb_key(b, "aes"); jb_str(b, a->aes); jb_key(b, "reg"); jb_str(b, a->reg); jb_key(b, "ges"); jb_str(b, a->ges);
		jb_key(b, "lbl"); jb_str(b, a->label);
		jb_printf(b, ",\"first\":%.1f,\"last\":%.1f,\"msgs\":%ld,\"cpdlc\":%ld,\"adsc\":%ld", a->first, a->last, a->msgs, a->cpdlc, a->adsc);
		jb_key(b, "sats"); jb_str(b, a->sats); jb_key(b, "types"); jb_str(b, a->types); jb_raw(b, "}");
	}
	jb_raw(b, "]");
	jb_key(b, "ges"); jb_raw(b, "[");
	for (int i = 0; i < nges; i++) {
		int n = 0;
		for (int k = 0; k < naircraft; k++) if (!strcmp(aircraft[k].ges, ges[i].id) && latest - aircraft[k].last <= AIRCRAFT_KEEP_S) n++;
		jb_raw(b, i ? ",{" : "{"); jb_key(b, "id"); jb_str(b, ges[i].id);
		jb_printf(b, ",\"msgs\":%ld,\"last\":%.1f,\"aircraft\":%d", ges[i].msgs, ges[i].last, n);
		jb_key(b, "sats"); jb_str(b, ges[i].sats); jb_raw(b, "}");
	}
	jb_raw(b, "]}");
	pthread_mutex_unlock(&lock);
}

extern const char WEB_PAGE[];

static void handle(int fd) {
	char req[2048]; size_t n = 0;
	while (n < sizeof req - 1) {
		ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
		if (r <= 0) break;
		n += (size_t)r; req[n] = 0;
		if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
	}
	req[n] = 0;
	char method[8] = "", path[256] = "";
	if (sscanf(req, "%7s %255s", method, path) != 2) return;
	char *q = strchr(path, '?'); if (q) *q = 0;
	int head = !strcmp(method, "HEAD");
	jbuf_t body = {0};
	const char *type = "text/plain", *status = "200 OK";
	if (strcmp(method, "GET") && !head) { status = "405 Method Not Allowed"; jb_raw(&body, "GET only"); }
	else if (!strcmp(path, "/")) { jb_raw(&body, WEB_PAGE); type = "text/html; charset=utf-8"; }
	else if (!strcmp(path, "/api/state")) { state_json(&body); type = "application/json"; }
	else if (!strcmp(path, "/healthz")) jb_raw(&body, "ok");
	else { status = "404 Not Found"; jb_raw(&body, "not found"); }
	char hdr[256];
	int h = snprintf(hdr, sizeof hdr, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
		"Connection: close\r\n\r\n", status, type, body.len);
	send(fd, hdr, h, MSG_NOSIGNAL);
	for (size_t off = 0; !head && off < body.len;) {
		ssize_t w = send(fd, body.s + off, body.len - off, MSG_NOSIGNAL);
		if (w <= 0) break;
		off += (size_t)w;
	}
	jb_free(&body);
}

// One request at a time: the page and its poll are small, and a client gets two seconds to send
// its request line before it is dropped.
static void *serve(void *arg) {
	(void)arg;
	while (running) {
		fd_set rf; FD_ZERO(&rf); FD_SET(listen_fd, &rf);
		struct timeval tv = {1, 0};
		if (select(listen_fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		int fd = accept(listen_fd, NULL, NULL);
		if (fd < 0) continue;
		struct timeval to = {2, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
		handle(fd);
		close(fd);
	}
	return NULL;
}

int web_start(int port, const char *sid, int observatory_port) {
	snprintf(station_id, sizeof station_id, "%s", sid ? sid : "");
	obs_port = observatory_port; started = wall_now();
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) { perror("web: socket"); return -1; }
	int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons((uint16_t)port)};
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(listen_fd, 16) < 0) {
		fprintf(stderr, "inmarsat-rx: --web cannot listen on port %d: %s\n", port, strerror(errno));
		close(listen_fd); listen_fd = -1; return -1;
	}
	running = 1;
	pthread_create(&thread, NULL, serve, NULL);
	fprintf(stderr, "inmarsat-rx: live page on http://0.0.0.0:%d/\n", port);
	return 0;
}

void web_stop(void) {
	if (!running) return;
	running = 0;
	pthread_join(thread, NULL);
	close(listen_fd); listen_fd = -1;
}
