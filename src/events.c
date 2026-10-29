// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Messages to the archive and the live page. Archive events follow vdl2-sniffer's: "e" names the
// event, "t" is epoch milliseconds, and an ACARS "dir" of DL is ground to aircraft (UL the other
// way), iridium-sniffer's convention. P-channels only carry DL.
#include "events.h"
#include "archive.h"
#include "out.h"
#include "web.h"
#include <stdio.h>

void events_acars(const ev_acars_t *e) {
	web_acars(e);
	if (!archive_enabled()) return;
	jbuf_t b = {0};
	char blk[2] = {e->block, 0};
	jb_printf(&b, "{\"e\":\"acars\",\"t\":%.0f,\"sys\":\"inmarsat\",\"dir\":\"DL\",\"freq\":%.0f", e->t_ms, e->freq);
	jb_key(&b, "sat"); jb_str(&b, e->sat); jb_key(&b, "svc"); jb_str(&b, e->type);
	jb_key(&b, "aes"); jb_str(&b, e->aes); jb_key(&b, "ges"); jb_str(&b, e->ges);
	jb_key(&b, "reg"); jb_str(&b, e->reg); jb_key(&b, "lbl"); jb_str(&b, e->label); jb_key(&b, "blk"); jb_str(&b, blk);
	jb_key(&b, "txt"); jb_str(&b, e->text);
	if (e->app_kind) {
		jb_key(&b, "app"); jb_str(&b, e->app_kind); jb_key(&b, "app_sum"); jb_str(&b, e->app_summary);
		if (e->tree_json) { jb_key(&b, "dec"); jb_raw(&b, e->tree_json); }
	}
	jb_raw(&b, "}");
	archive_write(e->t_ms, b.s);
	jb_free(&b);
}

void events_egc(const ev_egc_t *e) {
	web_egc(e);
	if (!archive_enabled()) return;
	jbuf_t b = {0};
	jb_printf(&b, "{\"e\":\"egc\",\"t\":%.0f,\"sys\":\"inmarsat\",\"freq\":%.0f", e->t_ms, e->freq);
	jb_key(&b, "sat"); jb_str(&b, e->sat);
	if (e->les >= 0) { jb_printf(&b, ",\"les\":%d", e->les); jb_key(&b, "station"); jb_str(&b, e->station); }
	jb_printf(&b, ",\"svc\":%d,\"id\":%d,\"parts\":%d,\"complete\":%s,\"rep\":%d", e->service, e->id, e->parts,
		e->complete ? "true" : "false", e->repetition);
	jb_key(&b, "service"); jb_str(&b, e->service_name); jb_key(&b, "prio"); jb_str(&b, e->priority);
	jb_key(&b, "txt"); jb_str(&b, e->text);
	jb_raw(&b, "}");
	archive_write(e->t_ms, b.s);
	jb_free(&b);
}

void events_stdc_sig(const ev_stdc_sig_t *e) {
	if (!archive_enabled()) return;
	jbuf_t b = {0};
	jb_printf(&b, "{\"e\":\"ssig\",\"t\":%.0f,\"freq\":%.0f", e->t_ms, e->freq);
	jb_key(&b, "sat"); jb_str(&b, e->sat);
	jb_printf(&b, ",\"d\":\"%02x\"", e->desc);
	if (e->chan_les >= 0) jb_printf(&b, ",\"cles\":%d", e->chan_les);
	if (e->ship) { jb_key(&b, "ship"); jb_str(&b, e->ship); }
	if (e->les >= 0) jb_printf(&b, ",\"les\":%d", e->les);
	if (e->lc >= 0) jb_printf(&b, ",\"lc\":%d", e->lc);
	if (e->pres >= 0) jb_printf(&b, ",\"pres\":%d", e->pres);
	if (e->pkt_no >= 0) jb_printf(&b, ",\"pkt\":%d,\"bytes\":%d", e->pkt_no, e->payload);
	jb_raw(&b, "}");
	archive_write(e->t_ms, b.s);
	jb_free(&b);
}
