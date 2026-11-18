// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Linux and Windows versions of what compat.h declares.
#include "compat.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>

int sock_init(void) {
	WSADATA w;
	return WSAStartup(MAKEWORD(2, 2), &w) ? -1 : 0;
}

void sock_timeouts(int fd, int recv_s, int send_s) {
	DWORD r = (DWORD)recv_s * 1000, s = (DWORD)send_s * 1000;
	setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof r);
	setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&s, sizeof s);
}

void sock_reuse(int fd) { (void)fd; }

void sock_nonblock(int fd) { u_long on = 1; ioctlsocket((SOCKET)fd, FIONBIO, &on); }

const char *sock_err(void) {
	static __thread char buf[256];
	int e = WSAGetLastError();
	if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)e, 0, buf, sizeof buf, NULL))
		snprintf(buf, sizeof buf, "socket error %d", e);
	for (size_t n = strlen(buf); n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == '.'); n--) buf[n - 1] = 0;
	return buf;
}

int compat_random(void *buf, size_t n) {
	return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

long compat_ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (long)si.dwNumberOfProcessors; }

void compat_stdin_binary(void) { _setmode(_fileno(stdin), _O_BINARY); }

struct tm *compat_gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }

// Only what inmarsat-rx asks for: "%Y-%m-%dT%H:%M:%S". Returns the rest of the string, or NULL.
char *compat_strptime(const char *s, const char *fmt, struct tm *tm) {
	int y, mo, d, h, mi, se, n = 0;
	if (strcmp(fmt, "%Y-%m-%dT%H:%M:%S") || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6)
		return NULL;
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return NULL;
	tm->tm_year = y - 1900; tm->tm_mon = mo - 1; tm->tm_mday = d; tm->tm_hour = h; tm->tm_min = mi; tm->tm_sec = se;
	return (char *)s + n;
}

ssize_t compat_getline(char **line, size_t *cap, FILE *f) {
	size_t n = 0;
	int c;
	if (!*line || !*cap) { *cap = 256; *line = realloc(*line, *cap); if (!*line) return -1; }
	while ((c = fgetc(f)) != EOF) {
		if (n + 2 > *cap) {
			char *p = realloc(*line, *cap * 2);
			if (!p) return -1;
			*line = p; *cap *= 2;
		}
		(*line)[n++] = (char)c;
		if (c == '\n') break;
	}
	(*line)[n] = 0;
	return n ? (ssize_t)n : -1;
}

#else
#include <sys/time.h>

int sock_init(void) { return 0; }

void sock_timeouts(int fd, int recv_s, int send_s) {
	struct timeval r = {recv_s, 0}, s = {send_s, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof s);
}

void sock_reuse(int fd) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }

void sock_nonblock(int fd) { int fl = fcntl(fd, F_GETFL); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }

const char *sock_err(void) { return strerror(errno); }

int compat_random(void *buf, size_t n) {
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t r = read(fd, buf, n);
	close(fd);
	return r == (ssize_t)n ? 0 : -1;
}

long compat_ncpu(void) { return sysconf(_SC_NPROCESSORS_ONLN); }

void compat_stdin_binary(void) {}
#endif
