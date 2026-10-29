// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Local-only view of STD-C messages to ships (--stdc-content). Off unless asked for. Deliveries are
// rebuilt from the LES channels' message packets, kept for 24 hours in a directory only this user
// can read, and shown on a page served on 127.0.0.1 only: never archived, fed or put on the
// shared pages. Ships appear as the same keyed hash as in the metadata.
#ifndef CONTENT_H
#define CONTENT_H

#include <stdint.h>

int content_start(const char *dir, int port);  // 0 ok
int content_enabled(void);
void content_stop(void);

// From the STD-C channels (after the once-per-frame dedupe). ship: keyed hash or NULL.
void content_signal(double t_ms, double freq, const char *sat, int desc, const char *ship, int les, int lc, int pres);
void content_packet(double t_ms, double freq, const char *sat, int les, int lc, int pkt_no, const uint8_t *data, int len);
// Closes deliveries quiet for 15 minutes and drops anything older than 24 hours. Call now and then.
void content_tick(double t_ms);

#endif
