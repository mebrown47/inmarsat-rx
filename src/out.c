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
static int use_stdout = 1, udp_fd = -1;
static struct sockaddr_storage udp_addr; static socklen_t udp_len;

int out_init(int to_stdout, const char *udp) {
	use_stdout = to_stdout;
	if (!udp) return 0;
	char host[256]; const char *colon = strrchr(udp, ':');
	if (!colon || colon == udp || (size_t)(colon - udp) >= sizeof host) { fprintf(stderr, "--udp wants HOST:PORT\n"); return -1; }
	memcpy(host, udp, colon - udp); host[colon - udp] = 0;
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *res;
	int err = getaddrinfo(host, colon + 1, &hints, &res);
	if (err) { fprintf(stderr, "--udp %s: %s\n", udp, gai_strerror(err)); return -1; }
	udp_fd = socket(res->ai_family, SOCK_DGRAM, 0);
	memcpy(&udp_addr, res->ai_addr, res->ai_addrlen); udp_len = res->ai_addrlen;
	freeaddrinfo(res);
	return udp_fd < 0 ? -1 : 0;
}

void out_emit(const char *json) {
	pthread_mutex_lock(&lock);
	if (use_stdout) { fputs(json, stdout); fputc('\n', stdout); fflush(stdout); }
	// One datagram per record; MSG_DONTWAIT so a stalled network never holds up decoding.
	if (udp_fd >= 0) sendto(udp_fd, json, strlen(json), MSG_DONTWAIT, (struct sockaddr *)&udp_addr, udp_len);
	pthread_mutex_unlock(&lock);
}

void out_close(void) { if (udp_fd >= 0) close(udp_fd); udp_fd = -1; }
