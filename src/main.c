// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// inmarsat-rx: Inmarsat L-band decoder for Aero ACARS (Aero-H+ 10.5k OQPSK, Aero-L 600/1200 MSK)
// and STD-C EGC. Decodes every channel in the input band at once — from channel plans, the
// command line, or carriers it finds itself (--scan) — and writes one JSON record per message (the
// SDR Workbench message-log schema) to stdout and/or UDP, a JSONL archive for the Observatory, and
// a live page.
#define _GNU_SOURCE
#include "apps.h"
#include "archive.h"
#include "channel.h"
#include "cluster.h"
#include "input.h"
#include "out.h"
#include "plans.h"
#include "scan.h"
#include "web.h"
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CHANNELS 64
#define MAX_CLUSTERS 32
#define MAX_TRIALS 6            // --scan: carriers being tried at once
#define IGNORE_S (30 * 60)      // --scan: a carrier that never synced is not tried again for this long
#define IDLE_DROP_S (15 * 60)   // --scan: a found channel with no new frames for this long is dropped

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static void usage(FILE *f) {
	fprintf(f,
		"Usage: inmarsat-rx [options] (--plan SAT[,SAT] | -c FREQ:TYPE[:SAT] ... | --scan)\n"
		"\n"
		"Decodes Inmarsat Aero ACARS and STD-C EGC from IQ samples; one JSON record per message.\n"
		"\n"
		"Input:\n"
		"  -i, --input SRC      IQ file, NAME.sigmf-meta, - for stdin (default), or a live source:\n"
		"                         airspy[:SERIAL]       Airspy R2/Mini (USB)\n"
		"                         hackrf[:SERIAL]       HackRF (USB)\n"
		"                         rtl_tcp:HOST:PORT     rtl_tcp server (RTL-SDR, or hackrf_tcp)\n"
		"                         spyserver:HOST:PORT   SpyServer node\n"
		"  -f, --format FMT     cu8, cs8, cs16, cf32 (files; SigMF metadata sets it)\n"
		"  -s, --rate HZ        sample rate (live: default the lowest rate that covers the channels)\n"
		"  -F, --center FREQ    centre frequency (live: default midway between the channels)\n"
		"      --start TIME     UTC time of the first sample of a file, e.g. 2026-10-04T23:38:16Z\n"
		"                       (SigMF metadata sets it; record times count from it)\n"
		"  -g, --gain G         airspy: linearity gain 0-21 (default 12); hackrf: LNA,VGA in dB (default 32,20);\n"
		"                       rtl_tcp: dB (default auto); spyserver: gain index (default: the node's)\n"
		"  -b, --bias-tee       power the antenna/LNA through the coax (airspy, hackrf, rtl_tcp)\n"
		"      --amp            HackRF RF amplifier (+14 dB)\n"
		"  -p, --ppm PPM        frequency correction\n"
		"\n"
		"Channels (FREQ in MHz, e.g. 1545.1700, or Hz):\n"
		"      --plan SAT[,SAT] the channel plans of these satellites (\"all\" for every plan); channels\n"
		"                       outside the input band are skipped. --plans lists them.\n"
		"  -c, --channel FREQ:TYPE[:SAT]  one channel. TYPE: aero (Aero-H+ 10.5k OQPSK), aerol600, aerol1200\n"
		"                       (Aero-L MSK), stdc (STD-C NCS 1200 Bd BPSK). Tune within ±1.2 kHz (Aero-L)\n"
		"                       or ±2.5 kHz (others) of the carrier.\n"
		"      --sat SAT        satellite for -c channels and found carriers without one (default: the\n"
		"                       --plan satellite when there is just one)\n"
		"      --scan           find carriers in the band, try each as the types its width allows, and keep\n"
		"                       the ones that reach frame sync (dropped again after 15 min without frames)\n"
		"      --scan-period S  seconds of spectrum averaged per scan (default 30)\n"
		"\n"
		"Output:\n"
		"  -u, --udp HOST:PORT  also send each record as a UDP datagram\n"
		"  -q, --quiet          no records on stdout (use with --udp, --archive or --web)\n"
		"      --archive DIR    JSONL archive for the Observatory (inmarsat-YYYYMMDD.jsonl)\n"
		"      --web PORT       live page on this port (e.g. 8892)\n"
		"      --observatory-port PORT  the Observatory's port on this host, for the live page's link (default 8893)\n"
		"      --hide-acks      leave ACARS link acknowledgements (label _d) out of the records\n"
		"      --data           also emit non-ACARS Aero user data (hex)\n"
		"      --blocks         one record per ACARS block, without joining multi-block messages\n"
		"                       (meta.more is true on blocks that end with ETB)\n"
		"      --bulletins      also emit STD-C bulletin boards (one per 8.64 s frame)\n"
		"      --station NAME   this receiver's name, in the records and on the live page\n"
		"      --no-clusters    give every channel its own full-rate first stage (for comparison)\n"
		"      --block N        frames per read (default about 50 ms, at most 65536; results do not depend on it)\n"
		"      --stats SEC      per-channel status on stderr every SEC seconds of samples (default 30, 0 = off)\n"
		"  -t, --threads N      decoding threads (default: one per channel, at most the CPU count)\n"
		"  -h, --help\n"
		"\n"
		"Examples:\n"
		"  inmarsat-rx -i airspy -b --plan i4f3 --web 8892 --archive /var/lib/inmarsat/archive -q\n"
		"  inmarsat-rx -i capture.sigmf-meta -c 1545.1700:aerol600 -c 1546.0050:aero\n");
}

static double parse_freq(const char *s) {
	double v = atof(s);
	return v < 1e5 ? v * 1e6 : v;  // MHz unless clearly Hz.
}

typedef struct { double freq; int kind; char sat[24]; } spec_t;

// A decoded channel and its bookkeeping. A trial is a carrier --scan is trying as types[ti].
typedef struct {
	channel_t *ch;
	long prev_frames, prev_ok, prev_bad, prev_msgs;  // Totals at the last health minute.
	long seen_frames; double progress_s;             // Frames at the last change, and when.
	int trial, types[3], ntypes, ti; double trial_start;
	int cl;                                          // Its cluster (the shared first stage).
} slot_t;

typedef struct {
	slot_t *slots; int nchannels, nthreads;
	cluster_t *clusters; int nclusters;
	pthread_barrier_t start, mid, done;
	const float *iq; int n; double now_ms; int quit;
} pool_t;
typedef struct { pool_t *pool; int index; } worker_arg_t;

static void *worker(void *arg) {
	worker_arg_t *w = arg; pool_t *p = w->pool;
	for (;;) {
		pthread_barrier_wait(&p->start);
		if (p->quit) return NULL;
		// First the clusters narrow the block, then every channel works on its cluster's output.
		for (int i = w->index; i < p->nclusters; i += p->nthreads) cluster_process(&p->clusters[i], p->iq, p->n);
		pthread_barrier_wait(&p->mid);
		for (int i = w->index; i < p->nchannels; i += p->nthreads) {
			const cluster_t *c = &p->clusters[p->slots[i].cl];
			channel_process(p->slots[i].ch, c->out, c->n, p->now_ms);
		}
		pthread_barrier_wait(&p->done);
	}
}

static const char *scanner_note(int scan) { return scan ? ", scanning for more" : ""; }

static double wall_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

static void print_status(slot_t *s, int n, double seconds) {
	fprintf(stderr, "--- %.0f s\n", seconds);
	for (int i = 0; i < n; i++) {
		char line[256]; channel_status(s[i].ch, line, sizeof line);
		channel_info_t ci; channel_info(s[i].ch, &ci);
		fprintf(stderr, "%-8s %s%s\n", ci.sat, line, s[i].trial ? "  (trying)" : ci.found ? "  (found)" : "");
	}
}

static void chan_event(double t_ms, const char *op, const channel_info_t *ci, const char *why) {
	if (!archive_enabled()) return;
	jbuf_t b = {0};
	jb_printf(&b, "{\"e\":\"chan\",\"t\":%.0f,\"op\":\"%s\",\"f\":%.0f", t_ms, op, ci->freq);
	jb_key(&b, "svc"); jb_str(&b, ci->type); jb_key(&b, "sat"); jb_str(&b, ci->sat); jb_key(&b, "why"); jb_str(&b, why);
	jb_raw(&b, "}");
	archive_write(t_ms, b.s);
	jb_free(&b);
}

// Per-minute channel health for the Observatory: counts since the last minute, and state now.
static void health_event(double minute_ms, slot_t *s, int n) {
	jbuf_t b = {0};
	jb_printf(&b, "{\"e\":\"imix\",\"t\":%.0f,\"ch\":{", minute_ms);
	int any = 0;
	for (int i = 0; i < n; i++) {
		if (s[i].trial) continue;
		channel_info_t ci; channel_info(s[i].ch, &ci);
		jb_printf(&b, "%s\"%.0f\":{", any ? "," : "", ci.freq); any = 1;
		jb_key(&b, "svc"); jb_str(&b, ci.type); jb_key(&b, "sat"); jb_str(&b, ci.sat);
		jb_printf(&b, ",\"fr\":%ld,\"ok\":%ld,\"bad\":%ld,\"msg\":%ld,\"snr\":%.1f,\"sync\":%d,\"found\":%d",
			ci.frames - s[i].prev_frames, ci.units_ok - s[i].prev_ok, ci.units_bad - s[i].prev_bad,
			ci.messages - s[i].prev_msgs, ci.acquired ? ci.snr_db : 0, ci.synced, ci.found);
		if (ci.ber >= 0) jb_printf(&b, ",\"ber\":%.4f", ci.ber);
		if (ci.les >= 0) { jb_printf(&b, ",\"les\":%d", ci.les); jb_key(&b, "station"); jb_str(&b, ci.station); }
		jb_raw(&b, "}");
		s[i].prev_frames = ci.frames; s[i].prev_ok = ci.units_ok; s[i].prev_bad = ci.units_bad; s[i].prev_msgs = ci.messages;
	}
	jb_raw(&b, "}}");
	archive_write(minute_ms, b.s);
	jb_free(&b);
	// STD-C packet tally for the minute: count and bytes by descriptor byte. Metadata only: no
	// packet contents are archived.
	jbuf_t p = {0};
	jb_printf(&p, "{\"e\":\"spkt\",\"t\":%.0f,\"ch\":{", minute_ms);
	int anyp = 0;
	for (int i = 0; i < n; i++) {
		unsigned long cnt[256], byt[256];
		if (s[i].trial || !channel_take_tally(s[i].ch, cnt, byt)) continue;
		channel_info_t ci; channel_info(s[i].ch, &ci);
		jb_printf(&p, "%s\"%.0f\":{", anyp ? "," : "", ci.freq); anyp = 1;
		jb_key(&p, "sat"); jb_str(&p, ci.sat);
		if (ci.les >= 0) { jb_printf(&p, ",\"les\":%d", ci.les); jb_key(&p, "station"); jb_str(&p, ci.station); }
		jb_raw(&p, ",\"d\":{");
		int first = 1;
		for (int d = 0; d < 256; d++) if (cnt[d]) { jb_printf(&p, "%s\"%02x\":[%lu,%lu]", first ? "" : ",", d, cnt[d], byt[d]); first = 0; }
		jb_raw(&p, "}}");
	}
	jb_raw(&p, "}}");
	if (anyp) archive_write(minute_ms, p.s);
	jb_free(&p);
}

// The clusters, and making channels on them. A channel joins the cluster whose passband holds it,
// or gets a cluster of its own; clusters no channel uses any more are freed between blocks.
static cluster_t clusters[MAX_CLUSTERS]; static int nclusters;
static double in_rate_g, in_center_g; static int clusters_off;

static int cluster_for(double freq) {
	int i = cluster_find(clusters, nclusters, freq);
	if (i >= 0) return i;
	if (nclusters == MAX_CLUSTERS) return 0;  // Cannot happen with 64 channels in 32 clusters of 250 kHz; stay safe.
	clusters[nclusters] = (cluster_t){.center = freq, .half = 0};
	if (clusters_off) clusters[nclusters].passthrough = 1, clusters[nclusters].rate = in_rate_g, clusters[nclusters].center = in_center_g;
	else cluster_init(&clusters[nclusters], in_rate_g, in_center_g);
	return nclusters++;
}

static channel_t *make_channel(slot_t *s, int kind, double freq, const char *sat, int found, const channel_opts_t *opts) {
	s->cl = cluster_for(freq);
	const cluster_t *c = &clusters[s->cl];
	return channel_new(kind, freq, sat, found, c->center, c->rate, opts);
}

static void clusters_gc(slot_t *slots, int nslots) {
	for (int i = 0; i < nclusters;) {
		int used = 0;
		for (int k = 0; k < nslots; k++) used |= slots[k].cl == i;
		if (used) { i++; continue; }
		cluster_free(&clusters[i]);
		clusters[i] = clusters[--nclusters];
		for (int k = 0; k < nslots; k++) if (slots[k].cl == nclusters) slots[k].cl = i;
	}
}

// Frames a trial needs to be kept, and how long it gets: a few frame periods.
static double trial_seconds(int kind) { return kind == CH_STDC ? 30 : kind == CH_AERO_H ? 10 : kind == CH_AERO_L1200 ? 20 : 40; }
static long trial_frames(int kind) { return kind == CH_STDC ? 2 : 3; }

int main(int argc, char **argv) {
	const char *input = "-", *udp = NULL, *station = NULL, *gain = NULL, *plan = NULL, *sat_default = NULL;
	const char *archive_dir = NULL;
	int bias = 0, amp = 0, scan = 0, web_port = 0, obs_port = 8893; double ppm = 0, scan_period = 30;
	int format = -1, quiet = 0, threads = 0, block = 0;
	double rate = 0, center = 0, stats_every = 30, start_arg = 0;
	channel_opts_t opts = {0};
	spec_t specs[MAX_CHANNELS]; int nspecs = 0;
	enum { OPT_HIDE_ACKS = 256, OPT_START, OPT_BLOCKS, OPT_DATA, OPT_BULLETINS, OPT_STATION, OPT_STATS, OPT_BLOCK, OPT_AMP,
		OPT_PLAN, OPT_PLANS, OPT_SAT, OPT_SCAN, OPT_SCAN_PERIOD, OPT_ARCHIVE, OPT_WEB, OPT_OBS_PORT, OPT_NO_CLUSTERS };
	static const struct option longopts[] = {
		{"input", required_argument, 0, 'i'}, {"format", required_argument, 0, 'f'},
		{"rate", required_argument, 0, 's'}, {"center", required_argument, 0, 'F'},
		{"channel", required_argument, 0, 'c'}, {"udp", required_argument, 0, 'u'},
		{"quiet", no_argument, 0, 'q'}, {"threads", required_argument, 0, 't'},
		{"gain", required_argument, 0, 'g'}, {"bias-tee", no_argument, 0, 'b'}, {"amp", no_argument, 0, OPT_AMP},
		{"ppm", required_argument, 0, 'p'}, {"hide-acks", no_argument, 0, OPT_HIDE_ACKS}, {"data", no_argument, 0, OPT_DATA},
		{"blocks", no_argument, 0, OPT_BLOCKS}, {"start", required_argument, 0, OPT_START},
		{"bulletins", no_argument, 0, OPT_BULLETINS}, {"station", required_argument, 0, OPT_STATION},
		{"stats", required_argument, 0, OPT_STATS}, {"block", required_argument, 0, OPT_BLOCK},
		{"plan", required_argument, 0, OPT_PLAN}, {"plans", no_argument, 0, OPT_PLANS}, {"sat", required_argument, 0, OPT_SAT},
		{"scan", no_argument, 0, OPT_SCAN}, {"scan-period", required_argument, 0, OPT_SCAN_PERIOD},
		{"archive", required_argument, 0, OPT_ARCHIVE}, {"web", required_argument, 0, OPT_WEB},
		{"observatory-port", required_argument, 0, OPT_OBS_PORT}, {"no-clusters", no_argument, 0, OPT_NO_CLUSTERS}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
	int c;
	while ((c = getopt_long(argc, argv, "i:f:s:F:c:u:qt:g:bp:h", longopts, NULL)) != -1) {
		switch (c) {
		case 'i': input = optarg; break;
		case 'f':
			if ((format = iq_format_parse(optarg)) < 0) { fprintf(stderr, "Unknown format %s\n", optarg); return 2; }
			break;
		case 's': rate = atof(optarg); break;
		case 'F': center = parse_freq(optarg); break;
		case 'c': {
			char kind_name[16] = "", sat[24] = "";
			const char *colon = strchr(optarg, ':');
			if (colon) sscanf(colon + 1, "%15[^:]:%23s", kind_name, sat);
			int kind = channel_kind_parse(kind_name);
			if (kind < 0) { fprintf(stderr, "Channel %s: want FREQ:TYPE[:SAT] with TYPE aero, aerol600, aerol1200 or stdc\n", optarg); return 2; }
			if (nspecs == MAX_CHANNELS) { fprintf(stderr, "At most %d channels\n", MAX_CHANNELS); return 2; }
			const satellite_t *st = sat[0] ? satellite_find(sat) : NULL;
			specs[nspecs] = (spec_t){parse_freq(optarg), kind, ""};
			snprintf(specs[nspecs].sat, sizeof specs[nspecs].sat, "%s", st ? st->id : sat);
			nspecs++;
			break;
		}
		case 'u': udp = optarg; break;
		case 'g': gain = optarg; break;
		case 'b': bias = 1; break;
		case 'p': ppm = atof(optarg); break;
		case OPT_AMP: amp = 1; break;
		case 'q': quiet = 1; break;
		case 't': threads = atoi(optarg); break;
		case OPT_HIDE_ACKS: opts.hide_acks = 1; break;
		case OPT_DATA: opts.emit_data = 1; break;
		case OPT_BLOCKS: opts.raw_blocks = 1; break;
		case OPT_START: {
			struct tm tm = {0}; const char *rest = strptime(optarg, "%Y-%m-%dT%H:%M:%S", &tm);
			if (!rest) { fprintf(stderr, "--start wants YYYY-MM-DDTHH:MM:SS[.fff]Z\n"); return 2; }
			start_arg = ((double)timegm(&tm) + (*rest == '.' ? atof(rest) : 0)) * 1000;
			break;
		}
		case OPT_BULLETINS: opts.emit_bulletins = 1; break;
		case OPT_STATION: station = optarg; break;
		case OPT_STATS: stats_every = atof(optarg); break;
		case OPT_BLOCK: block = atoi(optarg); if (block < 256) block = 256; break;
		case OPT_PLAN: plan = optarg; break;
		case OPT_PLANS: plans_print(stdout); return 0;
		case OPT_SAT: sat_default = optarg; break;
		case OPT_SCAN: scan = 1; break;
		case OPT_SCAN_PERIOD: scan_period = atof(optarg); if (scan_period < 5) scan_period = 5; break;
		case OPT_ARCHIVE: archive_dir = optarg; break;
		case OPT_WEB: web_port = atoi(optarg); break;
		case OPT_OBS_PORT: obs_port = atoi(optarg); break;
		case OPT_NO_CLUSTERS: clusters_off = 1; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	// Plans: every listed satellite's channels (frequencies outside the band are dropped below).
	int nplan_sats = 0; const char *plan_sat = NULL;
	if (plan) {
		char list[256]; snprintf(list, sizeof list, "%s", plan);
		for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
			int any = !strcmp(tok, "all");
			const satellite_t *st = any ? NULL : satellite_find(tok);
			if (!any && !st) { fprintf(stderr, "No plan for %s. Plans:\n", tok); plans_print(stderr); return 2; }
			for (int s = 0; s < NSATELLITES; s++) {
				if (!any && &SATELLITES[s] != st) continue;
				nplan_sats++; plan_sat = SATELLITES[s].id;
				for (int i = 0; i < NPLAN && nspecs < MAX_CHANNELS; i++)
					if (!strcmp(PLAN[i].sat, SATELLITES[s].id))
						specs[nspecs++] = (spec_t){PLAN[i].mhz * 1e6, channel_kind_parse(PLAN[i].type), ""},
						snprintf(specs[nspecs - 1].sat, sizeof specs[0].sat, "%s", PLAN[i].sat);
			}
		}
	}
	if (!sat_default) sat_default = nplan_sats == 1 ? plan_sat : "?";
	else { const satellite_t *st = satellite_find(sat_default); if (st) sat_default = st->id; }
	for (int i = 0; i < nspecs; i++) if (!specs[i].sat[0]) snprintf(specs[i].sat, sizeof specs[i].sat, "%s", sat_default);
	if (!nspecs && !scan) { usage(stderr); return 2; }
	if (!nspecs && !center && input_is_live(input)) { fprintf(stderr, "--scan with no channels needs -F (where to listen)\n"); return 2; }

	input_t *in;
	if (input_is_live(input)) {
		double lo = nspecs ? specs[0].freq : center, hi = lo;
		for (int i = 1; i < nspecs; i++) { if (specs[i].freq < lo) lo = specs[i].freq; if (specs[i].freq > hi) hi = specs[i].freq; }
		if (!center) {
			center = (lo + hi) / 2;
			// Keep the centre (where many SDRs have a DC spike) off every carrier.
			for (int moved = 1; moved;) {
				moved = 0;
				for (int i = 0; i < nspecs; i++) if (fabs(specs[i].freq - center) < 15000) { center += 15000; moved = 1; }
			}
		}
		// Margin for the widest signal (Aero-H+, ±5 kHz) plus the carrier search.
		double half_span = fmax(fabs(hi - center), fabs(lo - center)) + 10000;
		live_opts_t lo_opts = {.center_hz = center, .rate = rate, .half_span = half_span, .gain = gain, .bias = bias, .amp = amp, .ppm = ppm};
		in = input_open_live(input, &lo_opts);
	} else {
		in = input_open_file(input, format);
		if (in && rate) in->rate = rate;
		if (in && center) in->center_hz = center;
	}
	if (!in) return 1;
	if (!in->rate || !in->center_hz) { fprintf(stderr, "Give the sample rate (-s) and centre frequency (-F)\n"); return 2; }
	double start_ms = start_arg ? start_arg : in->start_ms ? in->start_ms : wall_ms();

	// Reads of about 50 ms: messages are timed by the read they finish in.
	if (!block) block = (int)fmin(65536, fmax(1024, in->rate / 20));
	if (out_init(!quiet, udp)) return 1;
	if (archive_dir && archive_open(archive_dir)) return 1;
	if (web_port && web_start(web_port, station, obs_port)) return 1;
	web_receiver(in->name, in->center_hz, in->rate, gain, bias);
	opts.source_kind = in->live ? "sdr" : "file";
	opts.source_name = station ? station : in->name;

	slot_t slots[MAX_CHANNELS] = {0}; int nslots = 0, skipped = 0;
	// Keep channels clear of the band edge, where the SDR's own filter rolls off.
	const double usable = in->rate / 2 - fmin(5000, 0.15 * in->rate);
	// Clusters for the channels given at the start; --scan adds to them, or adds more.
	in_rate_g = in->rate; in_center_g = in->center_hz;
	if (!clusters_off) {
		double fs[MAX_CHANNELS]; int nf = 0;
		for (int i = 0; i < nspecs; i++) if (fabs(specs[i].freq - in->center_hz) <= usable) fs[nf++] = specs[i].freq;
		for (int i = 1; i < nf; i++) for (int j = i; j > 0 && fs[j - 1] > fs[j]; j--) { double t = fs[j]; fs[j] = fs[j - 1]; fs[j - 1] = t; }
		nclusters = cluster_plan(fs, nf, clusters, MAX_CLUSTERS);
		for (int i = 0; i < nclusters; i++) cluster_init(&clusters[i], in->rate, in->center_hz);
	}
	for (int i = 0; i < nspecs; i++) {
		if (fabs(specs[i].freq - in->center_hz) > usable) {
			if (plan) { skipped++; continue; }  // Plans may list more than the band holds.
			fprintf(stderr, "Channel %.4f MHz is outside the input band (%.4f MHz ± %.0f kHz)\n",
				specs[i].freq / 1e6, in->center_hz / 1e6, in->rate / 2e3);
			return 2;
		}
		slot_t *s = &slots[nslots++];
		s->ch = make_channel(s, specs[i].kind, specs[i].freq, specs[i].sat, 0, &opts);
	}
	fprintf(stderr, "inmarsat-rx: %s, %.0f S/s at %.4f MHz, %d channel%s%s%s\n", in->name, in->rate, in->center_hz / 1e6,
		nslots, nslots == 1 ? "" : "s", scanner_note(scan), apps_available() ? "" : " (built without libacars: no CPDLC/ADS-C decode)");
	for (int i = 0; i < nclusters; i++) {
		int n = 0; for (int k = 0; k < nslots; k++) n += slots[k].cl == i;
		if (clusters[i].passthrough) continue;
		fprintf(stderr, "inmarsat-rx: cluster %.4f MHz ± %.0f kHz at %.0f S/s: %d channel%s\n", clusters[i].center / 1e6,
			clusters[i].half / 1e3, clusters[i].rate, n, n == 1 ? "" : "s");
	}
	if (skipped) fprintf(stderr, "inmarsat-rx: %d plan channel%s outside %.4f MHz ± %.0f kHz skipped\n", skipped,
		skipped == 1 ? "" : "s", in->center_hz / 1e6, in->rate / 2e3);
	if (archive_enabled()) {
		jbuf_t b = {0};
		jb_printf(&b, "{\"e\":\"start\",\"t\":%.0f,\"sys\":\"inmarsat\",\"centre\":%.0f,\"rate\":%.0f", start_ms, in->center_hz, in->rate);
		jb_key(&b, "src"); jb_str(&b, opts.source_name); jb_key(&b, "ch"); jb_raw(&b, "[");
		for (int i = 0; i < nslots; i++) {
			channel_info_t ci; channel_info(slots[i].ch, &ci);
			jb_printf(&b, "%s{\"f\":%.0f", i ? "," : "", ci.freq); jb_key(&b, "svc"); jb_str(&b, ci.type);
			jb_key(&b, "sat"); jb_str(&b, ci.sat); jb_raw(&b, "}");
		}
		jb_raw(&b, "]}");
		archive_write(start_ms, b.s);
		jb_free(&b);
	}

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (threads <= 0) threads = scan ? (int)ncpu : nslots < ncpu ? nslots : (int)ncpu;
	if (threads < 1) threads = 1;
	if (threads > MAX_CHANNELS) threads = MAX_CHANNELS;
	pool_t pool = {.slots = slots, .nchannels = nslots, .nthreads = threads, .clusters = clusters, .nclusters = nclusters};
	pthread_t tids[MAX_CHANNELS]; worker_arg_t args[MAX_CHANNELS];
	pthread_barrier_init(&pool.start, NULL, threads + 1);
	pthread_barrier_init(&pool.mid, NULL, threads);   // Workers only: clusters done before channels start.
	pthread_barrier_init(&pool.done, NULL, threads + 1);
	for (int i = 0; i < threads; i++) { args[i] = (worker_arg_t){&pool, i}; pthread_create(&tids[i], NULL, worker, &args[i]); }

	scan_t *scanner = scan ? scan_new(in->rate, in->center_hz, scan_period) : NULL;
	struct { double freq, until; } ignore[256]; int nignore = 0;

	signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
	float *bufs[2] = {malloc((size_t)block * 2 * sizeof(float)), malloc((size_t)block * 2 * sizeof(float))};
	long long consumed = 0, dropped_total = 0; double next_stats = stats_every, next_web = 0;
	long long minute = (long long)floor(start_ms / 60000);
	for (int k = 0; !stop; k ^= 1) {
		int n = in->read(in, bufs[k], block);
		if (n <= 0) { if (n < 0) fprintf(stderr, "Input error\n"); break; }
		if (input_dropped) {
			fprintf(stderr, "Dropped %lld samples (decoding fell behind or USB overrun)\n", input_dropped);
			dropped_total += input_dropped; input_dropped = 0;
		}
		pool.iq = bufs[k]; pool.n = n; pool.now_ms = start_ms + consumed / in->rate * 1000; pool.nchannels = nslots;
		pool.nclusters = nclusters;
		pthread_barrier_wait(&pool.start);
		if (scanner) scan_feed(scanner, bufs[k], n);  // Alongside the workers.
		pthread_barrier_wait(&pool.done);
		consumed += n;
		const double seconds = consumed / in->rate, now_ms = start_ms + seconds * 1000;

		if (archive_enabled() && (long long)floor(now_ms / 60000) != minute) {
			health_event(minute * 60000.0, slots, nslots);
			minute = (long long)floor(now_ms / 60000);
		}

		// --scan: settle trials, drop idle found channels, then try new carriers.
		for (int i = 0; scanner && i < nslots;) {
			slot_t *s = &slots[i];
			channel_info_t ci; channel_info(s->ch, &ci);
			if (ci.frames != s->seen_frames) { s->seen_frames = ci.frames; s->progress_s = seconds; }
			int drop = 0; const char *why = "";
			// A trial is kept as soon as it has enough frames, and given up after its time.
			if (s->trial && (ci.frames >= trial_frames(s->types[s->ti]) || seconds - s->trial_start >= trial_seconds(s->types[s->ti]))) {
				if (ci.frames >= trial_frames(s->types[s->ti])) {
					s->trial = 0; s->progress_s = seconds;
					fprintf(stderr, "scan: %.4f MHz is %s (%ld frames) — kept\n", ci.freq / 1e6, ci.type, ci.frames);
					chan_event(now_ms, "add", &ci, "scan");
				} else if (++s->ti < s->ntypes) {  // Same carrier, next type.
					double f = ci.freq; const char *sat = ci.sat;
					char satc[24]; snprintf(satc, sizeof satc, "%s", sat);
					channel_free(s->ch);
					s->ch = make_channel(s, s->types[s->ti], f, satc, 1, &opts);
					s->trial_start = seconds; s->seen_frames = 0;
				} else { drop = 1; why = "no frame sync"; }
			} else if (!s->trial && ci.found && seconds - s->progress_s > IDLE_DROP_S) { drop = 1; why = "no frames for 15 min"; }
			if (drop) {
				if (!s->trial) { fprintf(stderr, "scan: %.4f MHz dropped (%s)\n", ci.freq / 1e6, why); chan_event(now_ms, "drop", &ci, why); }
				if (nignore < 256) ignore[nignore++] = (typeof(ignore[0])){ci.freq, seconds + IGNORE_S};
				channel_free(s->ch);
				slots[i] = slots[--nslots];
				continue;
			}
			i++;
		}
		carrier_t found[128];
		int nfound = scanner ? scan_report(scanner, found, 128) : -1;
		for (int f = 0; f < nfound; f++) {
			const carrier_t *cr = &found[f];
			int trials = 0, near = 0;
			for (int i = 0; i < nslots; i++) {
				channel_info_t ci; channel_info(slots[i].ch, &ci);
				trials += slots[i].trial;
				double tol = (slots[i].types[0] == CH_AERO_H || !strcmp(ci.type, "aero") || cr->width > 6000) ? 7000 : 1500;
				if (fabs(ci.freq - cr->freq) < tol) near = 1;
			}
			for (int g = 0; g < nignore; g++) if (fabs(ignore[g].freq - cr->freq) < 1000 && seconds < ignore[g].until) near = 1;
			if (near || trials >= MAX_TRIALS || nslots >= MAX_CHANNELS) continue;
			if (fabs(cr->freq - in->center_hz) > usable) continue;
			// What the width allows: MSK 600 ~0.9 kHz, MSK 1200 and STD-C BPSK ~1.5-2.5 kHz, Aero-H+ ~10 kHz.
			int types[3], nt = 0;
			if (cr->width < 300) continue;  // An unmodulated tone or a spur.
			if (cr->width < 1300) types[nt++] = CH_AERO_L600;
			else if (cr->width < 2400) { types[nt++] = CH_AERO_L1200; types[nt++] = CH_STDC; }
			else if (cr->width < 4500) types[nt++] = CH_STDC;
			else if (cr->width > 6000 && cr->width < 16000) types[nt++] = CH_AERO_H;
			if (!nt) continue;
			slot_t *s = &slots[nslots];
			memset(s, 0, sizeof *s);
			s->trial = 1; memcpy(s->types, types, sizeof types); s->ntypes = nt; s->ti = 0; s->trial_start = seconds;
			s->ch = make_channel(s, types[0], cr->freq, sat_default, 1, &opts);
			nslots++;
			fprintf(stderr, "scan: trying %.4f MHz (%.0f Hz wide, +%.1f dB) as %s\n", cr->freq / 1e6, cr->width, cr->snr_db,
				channel_kind_name(types[0]));
		}
		for (int g = 0; g < nignore;) if (seconds >= ignore[g].until) ignore[g] = ignore[--nignore]; else g++;
		if (scanner) clusters_gc(slots, nslots);

		if (web_enabled() && seconds >= next_web) {
			channel_info_t infos[MAX_CHANNELS]; int ni = 0;
			for (int i = 0; i < nslots; i++) if (!slots[i].trial) channel_info(slots[i].ch, &infos[ni++]);
			web_channels(infos, ni);
			web_input(consumed, dropped_total);
			next_web = seconds + 1;
		}
		if (stats_every > 0 && seconds >= next_stats) { print_status(slots, nslots, seconds); next_stats += stats_every; }
	}
	pool.quit = 1;
	pthread_barrier_wait(&pool.start);
	for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);
	for (int i = 0; i < nslots; i++) channel_flush(slots[i].ch);
	if (archive_enabled()) health_event(minute * 60000.0, slots, nslots);
	print_status(slots, nslots, consumed / in->rate);
	web_stop();
	for (int i = 0; i < nslots; i++) channel_free(slots[i].ch);
	for (int i = 0; i < nclusters; i++) cluster_free(&clusters[i]);
	scan_free(scanner);
	in->close(in); out_close(); archive_close();
	free(bufs[0]); free(bufs[1]);
	return 0;
}
