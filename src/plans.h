// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Channel plans: the carriers of each Inmarsat satellite this receiver knows, by service.
#ifndef PLANS_H
#define PLANS_H

typedef struct {
	const char *id, *name, *slot;   // "i4f3", "I-4 F3", "98°W"
	const char *region;             // Ocean region / coverage, for display.
} satellite_t;

typedef struct {
	const char *sat;                // satellite_t.id
	double mhz;
	const char *type;               // channel type: aero, aerol600, aerol1200, stdc
} plan_channel_t;

extern const satellite_t SATELLITES[];
extern const int NSATELLITES;
extern const plan_channel_t PLAN[];
extern const int NPLAN;

// The satellite with this id, or NULL.
const satellite_t *satellite_find(const char *id);
// Lists the plans on f (for --plan help).
void plans_print(void *f);

#endif
