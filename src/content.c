// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Local-only STD-C message view. Messages to ships go: the NCS announces one (0x81: ship, LES,
// logical channel), the LES sends it on that logical channel of its own TDM (0xAA packets), asks the
// ship to acknowledge (0x08) and clears the channel (0x27). A delivery here opens at the
// announcement, collects the packets of that satellite, LES and logical channel by sequence number,
// and closes at the clear, a new announcement on the channel, or after 15 quiet minutes. (Logical
// channel assignments, 0x83, are the other direction: a ship asking to send.) Signalling is often
// repeated in later frames; a repeat of the open delivery's announcement is ignored. Its text is decoded as IA5; content
// that is not clearly text is shown as binary, by size only. Finished deliveries go to an hourly file
// (mode 600, in a directory of mode 700) and stay 24 hours.
#define _GNU_SOURCE
#include "content.h"
#include "out.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define KEEP_MS (24 * 3600 * 1000.0)
#define QUIET_MS (15 * 60 * 1000.0)
#define MAX_OPEN 128
#define MAX_PKTS 256
#define MAX_RECS 20000
#define TEXT_SHARE 0.85   // At least this share of printable IA5 characters to show as text.

typedef struct { int no, len; uint8_t data[256]; } pkt_t;
typedef struct {
	double t_open, t_last, freq; char sat[24], ship[12]; int les, lc, pres;
	pkt_t *pkts; int npkts;
} open_t;
typedef struct {
	double t, freq; char sat[24], ship[12]; int les, lc, pres, packets, missing, bytes, binary;
	char *text;
} rec_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int enabled, listen_fd = -1, running;
static pthread_t thread;
static char dir[512];
static open_t opened[MAX_OPEN]; static int nopen;
static rec_t recs[MAX_RECS]; static int nrecs;
static double latest;

int content_enabled(void) { return enabled; }

static void rec_free(rec_t *r) { free(r->text); r->text = NULL; }

// IA5 text, or NULL when the bytes do not look like text. Control characters other than line
// breaks and tabs are dropped.
static char *ia5(const uint8_t *b, int n) {
	int printable = 0;
	for (int i = 0; i < n; i++) { int c = b[i] & 0x7f; printable += (c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t'; }
	if (!n || printable < TEXT_SHARE * n) return NULL;
	char *t = malloc((size_t)n + 1); int k = 0;
	for (int i = 0; i < n; i++) {
		int c = b[i] & 0x7f;
		if (c == '\r') { if (i + 1 < n && (b[i + 1] & 0x7f) == '\n') continue; c = '\n'; }
		if ((c >= 0x20 && c < 0x7f) || c == '\n' || c == '\t') t[k++] = (char)c;
	}
	t[k] = 0;
	return t;
}

static int cmp_pkt(const void *a, const void *b) { return ((const pkt_t *)a)->no - ((const pkt_t *)b)->no; }

static void file_append(const rec_t *r) {
	time_t tt = (time_t)(r->t / 1000); struct tm g; gmtime_r(&tt, &g);
	char path[700];
	snprintf(path, sizeof path, "%s/deliveries-%04d%02d%02d%02d.jsonl", dir, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour);
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0) { fprintf(stderr, "content: cannot write %s: %s\n", path, strerror(errno)); return; }
	jbuf_t b = {0};
	jb_printf(&b, "{\"t\":%.0f,\"freq\":%.0f", r->t, r->freq); jb_key(&b, "sat"); jb_str(&b, r->sat);
	jb_key(&b, "ship"); jb_str(&b, r->ship[0] ? r->ship : NULL);
	jb_printf(&b, ",\"les\":%d,\"lc\":%d,\"pres\":%d,\"packets\":%d,\"missing\":%d,\"bytes\":%d,\"binary\":%d", r->les, r->lc,
		r->pres, r->packets, r->missing, r->bytes, r->binary);
	jb_key(&b, "text"); jb_str(&b, r->text); jb_raw(&b, "}\n");
	if (write(fd, b.s, b.len) < 0) fprintf(stderr, "content: write failed: %s\n", strerror(errno));
	close(fd); jb_free(&b);
}

static void add_rec(rec_t *r) {
	if (nrecs == MAX_RECS) { rec_free(&recs[0]); memmove(recs, recs + 1, (MAX_RECS - 1) * sizeof *recs); nrecs--; }
	recs[nrecs++] = *r;
}

static void finish(open_t *o) {
	if (o->npkts) {
		qsort(o->pkts, o->npkts, sizeof *o->pkts, cmp_pkt);
		uint8_t *all = malloc((size_t)o->npkts * 256 + 16 * o->npkts); int n = 0, missing = 0, uniq = 0, prev = -1;
		char *text = NULL; size_t tlen = 0;
		int binary = 0;
		for (int i = 0; i < o->npkts; i++) {
			if (o->pkts[i].no == prev) continue;  // A repeat.
			if (prev >= 0 && o->pkts[i].no > prev + 1) missing += o->pkts[i].no - prev - 1;
			prev = o->pkts[i].no; uniq++;
			memcpy(all + n, o->pkts[i].data, o->pkts[i].len); n += o->pkts[i].len;
		}
		// Decide text or binary on the whole, then build the text with […] at each gap.
		char *probe = ia5(all, n);
		if (probe) {
			free(probe);
			prev = -1;
			for (int i = 0; i < o->npkts; i++) {
				if (o->pkts[i].no == prev) continue;
				char *part = ia5(o->pkts[i].data, o->pkts[i].len);
				const char *gap = prev >= 0 && o->pkts[i].no > prev + 1 ? " […] " : "";
				size_t add = strlen(gap) + (part ? strlen(part) : 0);
				text = realloc(text, tlen + add + 1);
				memcpy(text + tlen, gap, strlen(gap)); tlen += strlen(gap);
				if (part) { memcpy(text + tlen, part, strlen(part)); tlen += strlen(part); free(part); }
				text[tlen] = 0;
				prev = o->pkts[i].no;
			}
		} else binary = 1;
		rec_t r = {.t = o->t_open, .freq = o->freq, .les = o->les, .lc = o->lc, .pres = o->pres, .packets = uniq,
			.missing = missing, .bytes = n, .binary = binary, .text = text};
		snprintf(r.sat, sizeof r.sat, "%s", o->sat); snprintf(r.ship, sizeof r.ship, "%s", o->ship);
		file_append(&r);
		add_rec(&r);
		memset(all, 0, n); free(all);
	}
	memset(o->pkts, 0, (size_t)o->npkts * sizeof *o->pkts);
	free(o->pkts);
	*o = opened[--nopen];
}

static open_t *find_open(const char *sat, int les, int lc) {
	for (int i = 0; i < nopen; i++) if (!strcmp(opened[i].sat, sat) && opened[i].les == les && opened[i].lc == lc) return &opened[i];
	return NULL;
}

static open_t *open_new(double t_ms, const char *sat, int les, int lc, const char *ship, int pres) {
	if (nopen == MAX_OPEN) return NULL;
	open_t *o = &opened[nopen++];
	*o = (open_t){.t_open = t_ms, .t_last = t_ms, .les = les, .lc = lc, .pres = pres};
	snprintf(o->sat, sizeof o->sat, "%s", sat); snprintf(o->ship, sizeof o->ship, "%s", ship ? ship : "");
	o->pkts = calloc(MAX_PKTS, sizeof *o->pkts);
	return o;
}

void content_signal(double t_ms, double freq, const char *sat, int desc, const char *ship, int les, int lc, int pres) {
	if (!enabled) return;
	pthread_mutex_lock(&lock);
	if (t_ms > latest) latest = t_ms;
	(void)freq;
	if (desc == 0x81 && lc >= 0) {
		open_t *o = find_open(sat, les, lc);
		if (o && ship && !strcmp(o->ship, ship)) { o->t_last = t_ms; pthread_mutex_unlock(&lock); return; }  // A repeat.
		if (o) finish(o);
		open_new(t_ms, sat, les, lc, ship, pres);
	} else if (desc == 0x27 && lc >= 0) {
		open_t *o = find_open(sat, les, lc);
		if (o && o->npkts) finish(o);  // A clear before any packet is a repeat of an earlier one.
	}
	pthread_mutex_unlock(&lock);
}

void content_packet(double t_ms, double freq, const char *sat, int les, int lc, int pkt_no, const uint8_t *data, int len) {
	if (!enabled) return;
	pthread_mutex_lock(&lock);
	open_t *o = find_open(sat, les, lc);
	// Packets with no announcement heard (missed, or before we started): a delivery to an unknown ship.
	if (!o) o = open_new(t_ms, sat, les, lc, NULL, -1);
	if (o && !o->freq) o->freq = freq;  // The LES channel it was delivered on.
	for (int i = 0; o && i < o->npkts; i++) if (o->pkts[i].no == pkt_no) { o->t_last = t_ms; o = NULL; }  // A repeat.
	if (o && o->npkts < MAX_PKTS) {
		pkt_t *p = &o->pkts[o->npkts++];
		p->no = pkt_no; p->len = len > 256 ? 256 : len; memcpy(p->data, data, p->len);
		o->t_last = t_ms;
	}
	pthread_mutex_unlock(&lock);
}

static void prune_files(double now_ms) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		int y, m, dd, h;
		if (sscanf(e->d_name, "deliveries-%4d%2d%2d%2d.jsonl", &y, &m, &dd, &h) != 4) continue;
		struct tm g = {.tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = dd, .tm_hour = h};
		double end_ms = ((double)timegm(&g) + 3600) * 1000;
		if (now_ms - end_ms > KEEP_MS) {
			char path[800]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
			unlink(path);
		}
	}
	closedir(d);
}

void content_tick(double t_ms) {
	if (!enabled) return;
	pthread_mutex_lock(&lock);
	if (t_ms > latest) latest = t_ms;
	for (int i = 0; i < nopen;) if (t_ms - opened[i].t_last > QUIET_MS) finish(&opened[i]); else i++;
	int k = 0;
	while (k < nrecs && t_ms - recs[k].t > KEEP_MS) rec_free(&recs[k++]);
	if (k) { memmove(recs, recs + k, (nrecs - k) * sizeof *recs); nrecs -= k; }
	static double last_prune;
	if (t_ms - last_prune > 600000) { prune_files(t_ms); last_prune = t_ms; }
	pthread_mutex_unlock(&lock);
}

// ---- Reading back the last 24 hours at start (our own one-line-per-delivery JSON) ----

static int json_num(const char *line, const char *key, double *v) {
	char k[32]; snprintf(k, sizeof k, "\"%s\":", key);
	const char *p = strstr(line, k);
	if (!p) return 0;
	*v = strtod(p + strlen(k), NULL);
	return 1;
}

static int json_str(const char *line, const char *key, char *out, size_t size) {
	char k[32]; snprintf(k, sizeof k, "\"%s\":\"", key);
	const char *p = strstr(line, k);
	if (!p) return 0;
	p += strlen(k);
	size_t n = 0;
	while (*p && *p != '"' && n + 1 < size) {
		if (*p == '\\' && p[1]) {
			p++;
			char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
			if (*p == 'u' && strlen(p) >= 5) { c = (char)strtol((char[]){p[1], p[2], p[3], p[4], 0}, NULL, 16); p += 4; }
			out[n++] = c; p++;
		} else out[n++] = *p++;
	}
	out[n] = 0;
	return 1;
}

static int cmp_rec(const void *a, const void *b) { double x = ((const rec_t *)a)->t, y = ((const rec_t *)b)->t; return x < y ? -1 : x > y; }

static void load(double now_ms) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	char *line = NULL; size_t cap = 0;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "deliveries-", 11)) continue;
		char path[800]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		while (getline(&line, &cap, f) > 0) {
			rec_t r = {0}; double v;
			if (!json_num(line, "t", &r.t) || now_ms - r.t > KEEP_MS) continue;
			if (json_num(line, "freq", &v)) r.freq = v;
			json_str(line, "sat", r.sat, sizeof r.sat); json_str(line, "ship", r.ship, sizeof r.ship);
			if (json_num(line, "les", &v)) r.les = (int)v;
			if (json_num(line, "lc", &v)) r.lc = (int)v;
			if (json_num(line, "pres", &v)) r.pres = (int)v;
			if (json_num(line, "packets", &v)) r.packets = (int)v;
			if (json_num(line, "missing", &v)) r.missing = (int)v;
			if (json_num(line, "bytes", &v)) r.bytes = (int)v;
			if (json_num(line, "binary", &v)) r.binary = (int)v;
			char *t = malloc(cap + 1);
			if (json_str(line, "text", t, cap + 1)) r.text = t; else free(t);
			if (nrecs < MAX_RECS) recs[nrecs++] = r; else rec_free(&r);
		}
		fclose(f);
	}
	free(line); closedir(d);
	qsort(recs, nrecs, sizeof *recs, cmp_rec);
}

// ---- The page, on 127.0.0.1 only ----

extern const char CONTENT_PAGE[];

static void state_json(jbuf_t *b) {
	struct timeval tv; gettimeofday(&tv, NULL);
	pthread_mutex_lock(&lock);
	jb_printf(b, "{\"now\":%.0f,\"latest\":%.0f,\"open\":%d", tv.tv_sec * 1000.0, latest, nopen);
	jb_key(b, "dir"); jb_str(b, dir);
	jb_key(b, "deliveries"); jb_raw(b, "[");
	for (int i = nrecs - 1, first = 1; i >= 0; i--) {
		const rec_t *r = &recs[i];
		jb_raw(b, first ? "{" : ",{"); first = 0;
		jb_printf(b, "\"t\":%.0f,\"freq\":%.0f", r->t, r->freq); jb_key(b, "sat"); jb_str(b, r->sat);
		jb_key(b, "ship"); jb_str(b, r->ship[0] ? r->ship : NULL);
		jb_printf(b, ",\"les\":%d,\"lc\":%d,\"pres\":%d,\"packets\":%d,\"missing\":%d,\"bytes\":%d,\"binary\":%d", r->les, r->lc,
			r->pres, r->packets, r->missing, r->bytes, r->binary);
		jb_key(b, "text"); jb_str(b, r->text); jb_raw(b, "}");
	}
	jb_raw(b, "]}");
	pthread_mutex_unlock(&lock);
}

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
	jbuf_t body = {0};
	const char *type = "text/plain", *status = "200 OK";
	if (strcmp(method, "GET")) { status = "405 Method Not Allowed"; jb_raw(&body, "GET only"); }
	else if (!strcmp(path, "/")) { jb_raw(&body, CONTENT_PAGE); type = "text/html; charset=utf-8"; }
	else if (!strcmp(path, "/api/deliveries")) { state_json(&body); type = "application/json"; }
	else if (!strcmp(path, "/healthz")) jb_raw(&body, "ok");
	else { status = "404 Not Found"; jb_raw(&body, "not found"); }
	char hdr[256];
	int h = snprintf(hdr, sizeof hdr, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
		"Connection: close\r\n\r\n", status, type, body.len);
	send(fd, hdr, h, MSG_NOSIGNAL);
	for (size_t off = 0; off < body.len;) {
		ssize_t w = send(fd, body.s + off, body.len - off, MSG_NOSIGNAL);
		if (w <= 0) break;
		off += (size_t)w;
	}
	jb_free(&body);
}

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

int content_start(const char *d, int port) {
	snprintf(dir, sizeof dir, "%s", d);
	if (mkdir(dir, 0700) && errno != EEXIST) { fprintf(stderr, "content: cannot create %s: %s\n", dir, strerror(errno)); return -1; }
	chmod(dir, 0700);
	struct timeval tv; gettimeofday(&tv, NULL);
	double now = tv.tv_sec * 1000.0;
	prune_files(now);
	load(now);
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) { perror("content: socket"); return -1; }
	int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	// Loopback only: reach it from elsewhere through an SSH tunnel.
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons((uint16_t)port)};
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(listen_fd, 8) < 0) {
		fprintf(stderr, "content: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
		close(listen_fd); listen_fd = -1; return -1;
	}
	enabled = running = 1;
	pthread_create(&thread, NULL, serve, NULL);
	fprintf(stderr, "inmarsat-rx: STD-C messages (local only) on http://127.0.0.1:%d/, kept 24 h in %s (%d from the last 24 h)\n",
		port, dir, nrecs);
	return 0;
}

void content_stop(void) {
	if (!enabled) return;
	pthread_mutex_lock(&lock);
	while (nopen) finish(&opened[0]);
	pthread_mutex_unlock(&lock);
	running = 0;
	pthread_join(thread, NULL);
	close(listen_fd);
	enabled = 0;
}
