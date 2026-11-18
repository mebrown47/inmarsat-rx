// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// What differs between Linux and Windows (MSYS2 / MinGW-w64), behind one name each: sockets, file
// modes, time conversions and a few POSIX calls. MinGW-w64 already supplies pthreads, getopt_long,
// dirent and unistd, so this is only the rest. Include it before using sockets or any of these.
#ifndef COMPAT_H
#define COMPAT_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // Windows raises no SIGPIPE.
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0   // Sockets that must not block are made non-blocking with sock_nonblock().
#endif

#define sock_close(fd) closesocket((SOCKET)(fd))
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
// Windows has no struct tm helpers by these names, and strtok_s is its strtok_r.
struct tm *compat_gmtime_r(const time_t *t, struct tm *out);
#define gmtime_r compat_gmtime_r
#define timegm _mkgmtime
#define strtok_r strtok_s
char *compat_strptime(const char *s, const char *fmt, struct tm *tm);
#define strptime compat_strptime
ssize_t compat_getline(char **line, size_t *cap, FILE *f);
#define getline compat_getline
#define compat_mkdir(path, mode) mkdir(path)

#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0   // Only Windows tells text files from binary ones.
#endif
#define sock_close(fd) close(fd)
#define compat_mkdir(path, mode) mkdir(path, mode)
#endif

// Once at start: Winsock needs starting before any socket call. 0 on success.
int sock_init(void);
// Receive and send timeouts in seconds (0: none). Linux takes a timeval, Windows milliseconds.
void sock_timeouts(int fd, int recv_s, int send_s);
// SO_REUSEADDR where it means "rebind after a restart" (Linux); on Windows it would let another
// program take the port, so it is left off there.
void sock_reuse(int fd);
void sock_nonblock(int fd);
// The last socket error as text (errno on Linux, WSAGetLastError on Windows).
const char *sock_err(void);
// Cryptographically random bytes (/dev/urandom, or BCryptGenRandom). 0 on success.
int compat_random(void *buf, size_t n);
long compat_ncpu(void);
// Standard input as raw bytes (Windows otherwise reads it as text and mangles IQ samples).
void compat_stdin_binary(void);

#endif
