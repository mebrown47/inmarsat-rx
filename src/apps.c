// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// libacars glue, following the SDR Workbench (wasm/acars-shim.c, usb/acars-apps.js). The summary
// picks every CPDLC element's choice_label and the first position anywhere in the tree.
#include "apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBACARS
#include <libacars/acars.h>
#include <libacars/libacars.h>
#include <libacars/vstring.h>

int apps_available(void) { return 1; }

// Value after "key": in a JSON text, as a number; 0 when absent.
static int json_number_after(const char *from, const char *key, double *v) {
	const char *p = strstr(from, key);
	if (!p) return 0;
	p += strlen(key);
	char *end; double x = strtod(p, &end);
	if (end == p) return 0;
	*v = x; return 1;
}

static void summarize(apps_t *a) {
	const char *j = a->json;
	if (strstr(j, "\"cpdlc\":")) strcpy(a->kind, "CPDLC");
	else if (strstr(j, "\"adsc\":")) strcpy(a->kind, "ADS-C");
	else if (!strncmp(j, "{\"miam\"", 7)) strcpy(a->kind, "MIAM");
	else {
		const char *p = strstr(j, "\"msg_type\":\"");
		if (p) { p += 12; size_t n = strcspn(p, "\""); if (n > 31) n = 31; memcpy(a->kind, p, n); a->kind[n] = 0; }
		else strcpy(a->kind, "ARINC 622");
	}
	size_t cap = 256, len = 0; char *line = malloc(cap); line[0] = 0;
	// Unique choice_label values, in order.
	for (const char *p = j; (p = strstr(p, "\"choice_label\":\"")); ) {
		p += 16; size_t n = strcspn(p, "\"");
		char label[128]; if (n > sizeof label - 1) n = sizeof label - 1;
		memcpy(label, p, n); label[n] = 0; p += n;
		char quoted[136]; snprintf(quoted, sizeof quoted, " · %s · ", label);
		char padded[1024]; snprintf(padded, sizeof padded, " · %s · ", line);
		if (strstr(padded, quoted)) continue;
		while (len + n + 8 > cap) { cap *= 2; line = realloc(line, cap); }
		len += sprintf(line + len, "%s%s", len ? " · " : "", label);
	}
	// libacars nests position differently per ADS-C report type, so search for it.
	const char *lat = strstr(j, "\"lat\":");
	if (lat && json_number_after(lat, "\"lat\":", &a->lat) && json_number_after(lat, "\"lon\":", &a->lon)) {
		a->has_pos = 1;
		a->has_alt = json_number_after(lat, "\"alt\":", &a->alt);
		while (len + 64 > cap) { cap *= 2; line = realloc(line, cap); }
		len += sprintf(line + len, "%sPOS %.4f%c %.4f%c", len ? "  " : "", a->lat < 0 ? -a->lat : a->lat, a->lat >= 0 ? 'N' : 'S',
			a->lon < 0 ? -a->lon : a->lon, a->lon >= 0 ? 'E' : 'W');
		if (a->has_alt) len += sprintf(line + len, "  %.0f ft", a->alt);
	}
	if (!len && a->text) {
		// Nothing to pick out: the most specific heading among libacars' first lines.
		const char *p = a->text; char head[128] = "";
		for (int i = 0; i < 4 && *p; i++) {
			const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
			while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
			while (n && (p[n - 1] == ' ' || p[n - 1] == '\r')) n--;
			if (n > 1 && p[n - 1] == ':' && n < sizeof head) { memcpy(head, p, n - 1); head[n - 1] = 0; }
			p = e ? e + 1 : p + n;
		}
		if (head[0]) { free(line); line = strdup(head); len = strlen(line); }
	}
	a->summary = line;
	a->error = strstr(j, "\"err\":true") || strstr(j, "\"crc_ok\":false");
}

int apps_decode(const char *label, const char *text, int uplink, apps_t *out) {
	memset(out, 0, sizeof *out);
	la_msg_dir dir = uplink ? LA_MSG_DIR_GND2AIR : LA_MSG_DIR_AIR2GND;
	char sublabel[3] = {0}, mfi[3] = {0};
	// Skip the "- #MD/AA" style sublabel and message function identifier, as la_acars_parse does.
	int offset = la_acars_extract_sublabel_and_mfi(label, dir, text, (int)strlen(text), sublabel, mfi);
	la_proto_node *node = la_acars_decode_apps(label, offset > 0 ? text + offset : text, dir);
	if (!node) return 0;
	la_vstring *vj = la_proto_tree_format_json(NULL, node), *vt = la_proto_tree_format_text(NULL, node);
	out->json = strdup(vj->str); out->text = strdup(vt->str);
	la_vstring_destroy(vj, true); la_vstring_destroy(vt, true);
	la_proto_tree_destroy(node);
	summarize(out);
	return 1;
}

#else

int apps_available(void) { return 0; }
int apps_decode(const char *label, const char *text, int uplink, apps_t *out) {
	(void)label; (void)text; (void)uplink; memset(out, 0, sizeof *out); return 0;
}

#endif

void apps_free(apps_t *a) { free(a->summary); free(a->json); free(a->text); memset(a, 0, sizeof *a); }
