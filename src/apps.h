// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// ACARS application decoding (FANS-1/A CPDLC and ADS-C, other ARINC 622, MIAM) with libacars.
#ifndef APPS_H
#define APPS_H

typedef struct {
	char kind[32];       // "CPDLC", "ADS-C", "MIAM", …
	char *summary;       // One line: CPDLC element labels, position.
	int error, has_pos;
	double lat, lon, alt; int has_alt;
	char *json, *text;   // libacars JSON tree and text rendering.
} apps_t;

// uplink: 1 for ground to air (Aero P-channel). Returns 0 when libacars finds no application
// (or the program was built without libacars).
int apps_decode(const char *label, const char *text, int uplink, apps_t *out);
void apps_free(apps_t *a);
int apps_available(void);

#endif
