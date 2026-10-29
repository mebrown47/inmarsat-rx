// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// JSON line output: a small string builder plus the sinks (stdout, UDP). Thread-safe emit.
#ifndef OUT_H
#define OUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct { char *s; size_t len, cap; } jbuf_t;
void jb_raw(jbuf_t *b, const char *s);
void jb_rawn(jbuf_t *b, const char *s, size_t n);
void jb_printf(jbuf_t *b, const char *fmt, ...);
void jb_str(jbuf_t *b, const char *s);           // JSON string literal, or null when s is NULL.
void jb_key(jbuf_t *b, const char *k);           // ,"k": (comma unless right after '{').
void jb_free(jbuf_t *b);

// Sinks. stdout is on unless out_init(0, ...). udp: "host:port" or NULL.
int out_init(int to_stdout, const char *udp);
// Writes one record (a JSON object without trailing newline) to every sink.
void out_emit(const char *json);
void out_close(void);

// FNV-1a 32-bit, base 36 (matches the workbench's record keys for ASCII text).
void fnv36(const char *s, char out[8]);

#endif
