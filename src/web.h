// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// inmarsat-rx's own live page (--web): what the receiver is hearing now, served from memory with
// no Python and no database, as vdl2-sniffer's. GET / is one self-contained page that polls
// GET /api/state every few seconds.
#ifndef WEB_H
#define WEB_H

#include "events.h"

int web_start(int port, const char *station_id, int observatory_port);  // 0 ok
void web_stop(void);
int web_enabled(void);

void web_receiver(const char *name, double centre, double rate, const char *gain, int bias);
void web_input(long long samples, long long dropped);
void web_channels(const channel_info_t *info, int n);
void web_acars(const ev_acars_t *e);
void web_egc(const ev_egc_t *e);

#endif
