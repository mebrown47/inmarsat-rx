// inmarsat-rx: Inmarsat L-band decoder for Aero ACARS (Aero-H+ 10.5k OQPSK, Aero-L 600/1200 MSK)
// and STD-C EGC. Decodes every listed channel in the input band at once and writes one JSON
// record per message (the SDR Workbench message-log schema) to stdout and/or UDP.
#define _GNU_SOURCE
#include "apps.h"
#include "channel.h"
#include "input.h"
#include "out.h"
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define MAX_CHANNELS 64

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static void usage(FILE *f) {
	fprintf(f,
		"Usage: inmarsat-rx [options] -c FREQ:TYPE [-c FREQ:TYPE ...]\n"
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
		"  -g, --gain G         airspy: linearity gain 0-21 (default 12); hackrf: LNA,VGA in dB (default 32,20);\n"
		"                       rtl_tcp: dB (default auto); spyserver: gain index (default: the node's)\n"
		"  -b, --bias-tee       power the antenna/LNA through the coax (airspy, hackrf, rtl_tcp)\n"
		"      --amp            HackRF RF amplifier (+14 dB)\n"
		"  -p, --ppm PPM        frequency correction\n"
		"\n"
		"Channels (FREQ in MHz, e.g. 1545.1697, or Hz):\n"
		"  -c, --channel FREQ:TYPE   TYPE: aero (Aero-H+ 10.5k OQPSK), aerol600, aerol1200 (Aero-L MSK),\n"
		"                            stdc (STD-C NCS 1200 Bd BPSK). Tune within ±1.2 kHz (Aero-L) or\n"
		"                            ±2.5 kHz (others) of the carrier.\n"
		"\n"
		"Output:\n"
		"  -u, --udp HOST:PORT  also send each record as a UDP datagram\n"
		"  -q, --quiet          no records on stdout (use with --udp)\n"
		"      --hide-acks      leave out ACARS link acknowledgements (label _d)\n"
		"      --data           also emit non-ACARS Aero user data (hex)\n"
		"      --bulletins      also emit STD-C bulletin boards (one per 8.64 s frame)\n"
		"      --station NAME   source name in the records (default: the input name)\n"
		"      --block N        frames per read (default 65536; results do not depend on it)\n"
		"      --stats SEC      per-channel status on stderr every SEC seconds of samples (default 30, 0 = off)\n"
		"  -t, --threads N      decoding threads (default: one per channel, at most the CPU count)\n"
		"  -h, --help\n"
		"\n"
		"Example (the test capture from the SDR Workbench):\n"
		"  inmarsat-rx -i aero120.sigmf-meta -c 1546.005:aero -c 1545.1697:aerol600 -c 1545.0745:aerol1200\n");
}

static double parse_freq(const char *s) {
	double v = atof(s);
	return v < 1e5 ? v * 1e6 : v;  // MHz unless clearly Hz.
}

typedef struct {
	channel_t **channels; int nchannels, nthreads;
	pthread_barrier_t start, done;
	const float *iq; int n; double now_ms; int quit;
} pool_t;
typedef struct { pool_t *pool; int index; } worker_arg_t;

static void *worker(void *arg) {
	worker_arg_t *w = arg; pool_t *p = w->pool;
	for (;;) {
		pthread_barrier_wait(&p->start);
		if (p->quit) return NULL;
		for (int i = w->index; i < p->nchannels; i += p->nthreads) channel_process(p->channels[i], p->iq, p->n, p->now_ms);
		pthread_barrier_wait(&p->done);
	}
}

static double wall_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

static void print_status(channel_t **ch, int n, double seconds) {
	fprintf(stderr, "--- %.0f s\n", seconds);
	for (int i = 0; i < n; i++) { char line[256]; channel_status(ch[i], line, sizeof line); fprintf(stderr, "%s\n", line); }
}

int main(int argc, char **argv) {
	const char *input = "-", *udp = NULL, *station = NULL, *gain = NULL;
	int bias = 0, amp = 0; double ppm = 0;
	int format = -1, quiet = 0, threads = 0, block = 65536;
	double rate = 0, center = 0, stats_every = 30;
	channel_opts_t opts = {0};
	struct { double freq; int kind; } specs[MAX_CHANNELS]; int nspecs = 0;
	enum { OPT_HIDE_ACKS = 256, OPT_DATA, OPT_BULLETINS, OPT_STATION, OPT_STATS, OPT_BLOCK, OPT_AMP };
	static const struct option longopts[] = {
		{"input", required_argument, 0, 'i'}, {"format", required_argument, 0, 'f'},
		{"rate", required_argument, 0, 's'}, {"center", required_argument, 0, 'F'},
		{"channel", required_argument, 0, 'c'}, {"udp", required_argument, 0, 'u'},
		{"quiet", no_argument, 0, 'q'}, {"threads", required_argument, 0, 't'},
		{"gain", required_argument, 0, 'g'}, {"bias-tee", no_argument, 0, 'b'}, {"amp", no_argument, 0, OPT_AMP},
		{"ppm", required_argument, 0, 'p'},
		{"hide-acks", no_argument, 0, OPT_HIDE_ACKS}, {"data", no_argument, 0, OPT_DATA},
		{"bulletins", no_argument, 0, OPT_BULLETINS}, {"station", required_argument, 0, OPT_STATION},
		{"stats", required_argument, 0, OPT_STATS}, {"block", required_argument, 0, OPT_BLOCK}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
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
			const char *colon = strchr(optarg, ':');
			int kind = colon ? channel_kind_parse(colon + 1) : -1;
			if (kind < 0) { fprintf(stderr, "Channel %s: want FREQ:TYPE with TYPE aero, aerol600, aerol1200 or stdc\n", optarg); return 2; }
			if (nspecs == MAX_CHANNELS) { fprintf(stderr, "At most %d channels\n", MAX_CHANNELS); return 2; }
			specs[nspecs].freq = parse_freq(optarg); specs[nspecs].kind = kind; nspecs++;
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
		case OPT_BULLETINS: opts.emit_bulletins = 1; break;
		case OPT_STATION: station = optarg; break;
		case OPT_STATS: stats_every = atof(optarg); break;
		case OPT_BLOCK: block = atoi(optarg); if (block < 256) block = 256; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (!nspecs) { usage(stderr); return 2; }

	input_t *in;
	if (input_is_live(input)) {
		double lo = specs[0].freq, hi = specs[0].freq;
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
	double start_ms = in->start_ms ? in->start_ms : wall_ms();

	if (out_init(!quiet, udp)) return 1;
	opts.source_kind = in->live ? "sdr" : "file";
	opts.source_name = station ? station : in->name;
	channel_t *channels[MAX_CHANNELS];
	for (int i = 0; i < nspecs; i++) {
		double off = specs[i].freq - in->center_hz;
		if (fabs(off) > in->rate / 2 - 5000) {
			fprintf(stderr, "Channel %.4f MHz is outside the input band (%.4f MHz ± %.0f kHz)\n",
				specs[i].freq / 1e6, in->center_hz / 1e6, in->rate / 2e3);
			return 2;
		}
		channels[i] = channel_new(specs[i].kind, specs[i].freq, in->center_hz, in->rate, &opts);
	}
	fprintf(stderr, "inmarsat-rx: %s, %.0f S/s at %.4f MHz, %d channel%s%s\n", in->name, in->rate, in->center_hz / 1e6,
		nspecs, nspecs == 1 ? "" : "s", apps_available() ? "" : " (built without libacars: no CPDLC/ADS-C decode)");

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (threads <= 0) threads = nspecs < ncpu ? nspecs : (int)ncpu;
	if (threads > nspecs) threads = nspecs;
	pool_t pool = {.channels = channels, .nchannels = nspecs, .nthreads = threads};
	pthread_t tids[MAX_CHANNELS]; worker_arg_t args[MAX_CHANNELS];
	pthread_barrier_init(&pool.start, NULL, threads + 1);
	pthread_barrier_init(&pool.done, NULL, threads + 1);
	for (int i = 0; i < threads; i++) { args[i] = (worker_arg_t){&pool, i}; pthread_create(&tids[i], NULL, worker, &args[i]); }

	signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
	float *bufs[2] = {malloc((size_t)block * 2 * sizeof(float)), malloc((size_t)block * 2 * sizeof(float))};
	long long consumed = 0; double next_stats = stats_every;
	for (int k = 0; !stop; k ^= 1) {
		int n = in->read(in, bufs[k], block);
		if (n <= 0) { if (n < 0) fprintf(stderr, "Input error\n"); break; }
		if (input_dropped) { fprintf(stderr, "Dropped %lld samples (decoding fell behind or USB overrun)\n", input_dropped); input_dropped = 0; }
		pool.iq = bufs[k]; pool.n = n; pool.now_ms = start_ms + consumed / in->rate * 1000;
		pthread_barrier_wait(&pool.start);
		pthread_barrier_wait(&pool.done);
		consumed += n;
		double seconds = consumed / in->rate;
		if (stats_every > 0 && seconds >= next_stats) { print_status(channels, nspecs, seconds); next_stats += stats_every; }
	}
	pool.quit = 1;
	pthread_barrier_wait(&pool.start);
	for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);
	for (int i = 0; i < nspecs; i++) channel_flush(channels[i]);
	print_status(channels, nspecs, consumed / in->rate);
	for (int i = 0; i < nspecs; i++) channel_free(channels[i]);
	in->close(in); out_close();
	free(bufs[0]); free(bufs[1]);
	return 0;
}
