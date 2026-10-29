// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// JSONL archive for the Observatory: inmarsat-YYYYMMDD.jsonl, one file per UTC day of the
// event's own time (a replayed recording lands on its own day). Thread-safe.
#ifndef ARCHIVE_H
#define ARCHIVE_H

int archive_open(const char *dir);   // 0 ok
int archive_enabled(void);
void archive_write(double t_ms, const char *line);
void archive_close(void);

#endif
