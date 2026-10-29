// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Channel plans. Only carriers decoded here are listed; --scan finds the rest. Frequencies are
// nominal: the decoders search ±1.2 kHz (Aero-L) or ±2.5 kHz (Aero-H+, STD-C) around them.
#include "plans.h"
#include <stdio.h>
#include <string.h>

const satellite_t SATELLITES[] = {
	{"i4f3", "I-4 F3", "98°W", "Americas"},
	{"i3f5", "I-3 F5", "54°W", "Atlantic"},
	{"alphasat", "Alphasat", "25°E", "Europe, Africa"},
};
const int NSATELLITES = sizeof SATELLITES / sizeof SATELLITES[0];

const plan_channel_t PLAN[] = {
	// I-4 F3, Mandan ND recordings 2026-10-04 (Airspy R2): every carrier decoded at 0.0-0.3 % BER.
	{"i4f3", 1537.7000, "stdc"},
	{"i4f3", 1545.0500, "aerol600"}, {"i4f3", 1545.0600, "aerol600"}, {"i4f3", 1545.0650, "aerol600"},
	{"i4f3", 1545.0750, "aerol1200"},
	{"i4f3", 1545.0800, "aerol600"}, {"i4f3", 1545.0850, "aerol600"}, {"i4f3", 1545.0900, "aerol600"},
	{"i4f3", 1545.1000, "aerol600"}, {"i4f3", 1545.1100, "aerol600"}, {"i4f3", 1545.1700, "aerol600"},
	{"i4f3", 1545.1750, "aerol600"},
	{"i4f3", 1546.0050, "aero"}, {"i4f3", 1546.0200, "aero"}, {"i4f3", 1546.0625, "aero"},
	{"i4f3", 1546.0775, "aero"},
	// Found by --scan on the Atlanta feeder, 2026-10-29: Aero-L via GES 43/44 (39 aircraft also heard
	// on the channels above, and an aircraft is logged on to one satellite at a time), and the LES
	// TDM carriers whose bulletin boards report AOR-W, as 1537.700 does.
	{"i4f3", 1545.0250, "aerol600"}, {"i4f3", 1545.0300, "aerol600"}, {"i4f3", 1545.0350, "aerol600"},
	{"i4f3", 1545.0400, "aerol600"},
	{"i4f3", 1539.5250, "stdc"}, {"i4f3", 1539.5450, "stdc"}, {"i4f3", 1539.5550, "stdc"}, {"i4f3", 1539.5650, "stdc"},
	{"i4f3", 1539.5850, "stdc"}, {"i4f3", 1539.6850, "stdc"},
	// I-3 F5 (AOR-E NCS, LES 44), decoded live from Mandan.
	{"i3f5", 1541.4500, "stdc"},
	{"i3f5", 1546.0550, "aero"}, {"i3f5", 1546.0700, "aero"},
	// LES TDM carriers whose bulletin boards report AOR-E, as 1541.450 does (Atlanta, 2026-10-29).
	{"i3f5", 1539.5050, "stdc"}, {"i3f5", 1539.5950, "stdc"}, {"i3f5", 1539.6150, "stdc"}, {"i3f5", 1539.6350, "stdc"},
	{"i3f5", 1539.6650, "stdc"},
	// Alphasat NCS over the UK SpyServer: ~5.6 kHz above the nominal 1537.100.
	{"alphasat", 1537.1056, "stdc"},
};
const int NPLAN = sizeof PLAN / sizeof PLAN[0];

const satellite_t *satellite_find(const char *id) {
	for (int i = 0; i < NSATELLITES; i++)
		if (!strcmp(SATELLITES[i].id, id) || !strcmp(SATELLITES[i].name, id)) return &SATELLITES[i];
	return NULL;
}

void plans_print(void *fp) {
	FILE *f = fp;
	for (int s = 0; s < NSATELLITES; s++) {
		fprintf(f, "  %-9s %s %s (%s):", SATELLITES[s].id, SATELLITES[s].name, SATELLITES[s].slot, SATELLITES[s].region);
		for (int i = 0; i < NPLAN; i++)
			if (!strcmp(PLAN[i].sat, SATELLITES[s].id)) fprintf(f, " %.4f:%s", PLAN[i].mhz, PLAN[i].type);
		fprintf(f, "\n");
	}
}
