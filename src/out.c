// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
#include "out.h"
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void reserve(jbuf_t *b, size_t extra) {
	if (b->len + extra + 1 <= b->cap) return;
	size_t n = b->cap ? b->cap : 256;
	while (n < b->len + extra + 1) n *= 2;
	b->s = realloc(b->s, n); b->cap = n;
}

void jb_rawn(jbuf_t *b, const char *s, size_t n) { reserve(b, n); memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
void jb_raw(jbuf_t *b, const char *s) { jb_rawn(b, s, strlen(s)); }

void jb_printf(jbuf_t *b, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
	reserve(b, n);
	va_start(ap, fmt); vsnprintf(b->s + b->len, n + 1, fmt, ap); va_end(ap);
	b->len += n;
}

void jb_str(jbuf_t *b, const char *s) {
	if (!s) { jb_raw(b, "null"); return; }
	jb_raw(b, "\"");
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': jb_raw(b, "\\\""); break;
		case '\\': jb_raw(b, "\\\\"); break;
		case '\n': jb_raw(b, "\\n"); break;
		case '\r': jb_raw(b, "\\r"); break;
		case '\t': jb_raw(b, "\\t"); break;
		default:
			if (*p < 0x20 || *p == 0x7f) jb_printf(b, "\\u%04x", *p);
			else jb_rawn(b, (const char *)p, 1);
		}
	}
	jb_raw(b, "\"");
}

void jb_key(jbuf_t *b, const char *k) {
	if (b->len && b->s[b->len - 1] != '{') jb_raw(b, ",");
	jb_str(b, k); jb_raw(b, ":");
}

void jb_free(jbuf_t *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

void fnv36(const char *s, char out[8]) {
	uint32_t h = 0x811c9dc5u;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 0x01000193u; }
	char tmp[8]; int n = 0;
	do { int d = h % 36; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); h /= 36; } while (h);
	for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	out[n] = 0;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int use_stdout = 1;

// UDP destinations, such as a local acarshub and an aggregator. A name that does not resolve yet
// (at boot, before the network is up) is tried again every 10 s, on a thread of its own so a slow
// lookup never holds up decoding; records meanwhile are not sent there.
#define MAX_UDP 8
typedef struct {
	char host[256], port[16]; int fd, warned;
	struct sockaddr_storage addr; socklen_t len;
} udp_dest_t;
static udp_dest_t dests[MAX_UDP]; static int ndests;

// Resolves a destination (without the lock: the lookup can take seconds) and, if it resolves,
// installs it under the lock. Returns whether it is ready.
static int udp_resolve(udp_dest_t *d) {
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *res;
	int err = getaddrinfo(d->host, d->port, &hints, &res);
	if (err) {
		if (!d->warned) fprintf(stderr, "--udp %s:%s: %s; trying again every 10 s\n", d->host, d->port, gai_strerror(err));
		d->warned = 1;
		return 0;
	}
	int fd = socket(res->ai_family, SOCK_DGRAM, 0);
	if (fd >= 0) {
		pthread_mutex_lock(&lock);
		memcpy(&d->addr, res->ai_addr, res->ai_addrlen); d->len = res->ai_addrlen; d->fd = fd;
		pthread_mutex_unlock(&lock);
		if (d->warned) fprintf(stderr, "--udp %s:%s: resolved, sending\n", d->host, d->port);
	}
	freeaddrinfo(res);
	return fd >= 0;
}

static void *retry_thread(void *arg) {
	(void)arg;
	for (;;) {
		sleep(10);
		int left = 0;
		for (int i = 0; i < ndests; i++) if (dests[i].fd < 0) left += !udp_resolve(&dests[i]);
		if (!left) return NULL;
	}
}

int out_add_udp(const char *list) {
	char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
	for (char *save = NULL, *s = strtok_r(buf, ", ", &save); s; s = strtok_r(NULL, ", ", &save)) {
		const char *colon = strrchr(s, ':');
		if (!colon || colon == s || (size_t)(colon - s) >= sizeof dests[0].host || strlen(colon + 1) >= sizeof dests[0].port ||
			!colon[1]) { fprintf(stderr, "--udp wants HOST:PORT, not %s\n", s); return -1; }
		if (ndests == MAX_UDP) { fprintf(stderr, "--udp: at most %d destinations\n", MAX_UDP); return -1; }
		udp_dest_t *d = &dests[ndests++];
		*d = (udp_dest_t){.fd = -1};
		// [v6addr]:port as well as host:port.
		const char *h = s; size_t hl = colon - s;
		if (hl >= 2 && h[0] == '[' && h[hl - 1] == ']') { h++; hl -= 2; }
		memcpy(d->host, h, hl); d->host[hl] = 0;
		snprintf(d->port, sizeof d->port, "%s", colon + 1);
	}
	return 0;
}

int out_init(int to_stdout) {
	use_stdout = to_stdout;
	int left = 0;
	for (int i = 0; i < ndests; i++) left += !udp_resolve(&dests[i]);
	pthread_t t;
	if (left && !pthread_create(&t, NULL, retry_thread, NULL)) pthread_detach(t);
	return 0;
}

void out_emit(const char *json) {
	pthread_mutex_lock(&lock);
	if (use_stdout) { fputs(json, stdout); fputc('\n', stdout); fflush(stdout); }
	// One datagram per record and destination; MSG_DONTWAIT so a stalled network never holds up decoding.
	for (int i = 0; i < ndests; i++) {
		udp_dest_t *d = &dests[i];
		if (d->fd >= 0) sendto(d->fd, json, strlen(json), MSG_DONTWAIT, (struct sockaddr *)&d->addr, d->len);
	}
	pthread_mutex_unlock(&lock);
}

void out_close(void) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < ndests; i++) if (dests[i].fd >= 0) { close(dests[i].fd); dests[i].fd = -1; }
	pthread_mutex_unlock(&lock);
}
