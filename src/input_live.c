// Live IQ sources: Airspy R2/Mini and HackRF over USB (libairspy / libhackrf), and rtl_tcp and
// SpyServer over the network. Each feeds a ring buffer that read() drains; samples that arrive
// while the ring is full are dropped and counted.
#define _GNU_SOURCE
#include "input.h"
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef HAVE_AIRSPY
#include <libairspy/airspy.h>
#endif
#ifdef HAVE_HACKRF
#include <libhackrf/hackrf.h>
#endif

volatile long long input_dropped;

// ---- Ring buffer of float I/Q frames ----

typedef struct {
	float *buf; int cap, head, len;  // In frames.
	int closed;
	pthread_mutex_t lock; pthread_cond_t ready;
} ring_t;

static void ring_init(ring_t *r, int frames) {
	r->buf = malloc((size_t)frames * 2 * sizeof(float)); r->cap = frames;
	pthread_mutex_init(&r->lock, NULL); pthread_cond_init(&r->ready, NULL);
}

// Called from the device thread. src: n frames, or NULL to write via convert(i8/u8/s16).
static void ring_write(ring_t *r, const float *src, int n) {
	pthread_mutex_lock(&r->lock);
	int room = r->cap - r->len;
	if (n > room) { input_dropped += n - room; n = room; }
	for (int i = 0; i < n; i++) {
		int at = (r->head + r->len + i) % r->cap;
		r->buf[2 * at] = src[2 * i]; r->buf[2 * at + 1] = src[2 * i + 1];
	}
	r->len += n;
	pthread_cond_signal(&r->ready);
	pthread_mutex_unlock(&r->lock);
}

static void ring_close(ring_t *r) {
	pthread_mutex_lock(&r->lock); r->closed = 1; pthread_cond_broadcast(&r->ready); pthread_mutex_unlock(&r->lock);
}

static int ring_read(ring_t *r, float *iq, int max) {
	pthread_mutex_lock(&r->lock);
	while (!r->len && !r->closed) pthread_cond_wait(&r->ready, &r->lock);
	int n = r->len < max ? r->len : max;
	for (int i = 0; i < n; i++) {
		int at = (r->head + i) % r->cap;
		iq[2 * i] = r->buf[2 * at]; iq[2 * i + 1] = r->buf[2 * at + 1];
	}
	r->head = (r->head + n) % r->cap; r->len -= n;
	pthread_mutex_unlock(&r->lock);
	return n;
}

static void ring_free(ring_t *r) {
	free(r->buf); pthread_mutex_destroy(&r->lock); pthread_cond_destroy(&r->ready);
}

typedef struct live live_t;
struct live {
	ring_t ring;
	void (*stop)(live_t *);
	void *dev; int fd; pthread_t thread; int running;
	float *conv; int convcap;  // Conversion scratch for integer samples.
	// SpyServer state.
	uint32_t max_rate, stages, min_dec, gain_count, min_freq, max_freq, forced_format;
	int have_info, have_sync, can_control;
};

static float *conv_buf(live_t *l, int frames) {
	if (frames > l->convcap) { l->convcap = frames; l->conv = realloc(l->conv, (size_t)frames * 2 * sizeof(float)); }
	return l->conv;
}

static double now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

static int live_read(input_t *in, float *iq, int max) { return ring_read(&((live_t *)in->priv)->ring, iq, max); }

static void live_close(input_t *in) {
	live_t *l = in->priv;
	if (l->stop) l->stop(l);
	ring_free(&l->ring); free(l->conv); free(l); free(in);
}

static input_t *live_new(double rate, double center, const char *name) {
	input_t *in = calloc(1, sizeof *in);
	live_t *l = calloc(1, sizeof *l);
	l->fd = -1;
	ring_init(&l->ring, (int)(2 * rate) + 65536);  // About two seconds.
	in->priv = l; in->read = live_read; in->close = live_close;
	in->rate = rate; in->center_hz = center; in->live = 1; in->start_ms = now_ms();
	snprintf(in->name, sizeof in->name, "%s", name);
	return in;
}

// Lowest of the rates (ascending) whose usable band (±40%) covers half_span; else the highest.
static double pick_rate(const double *rates, int n, double half_span, double wanted) {
	if (wanted > 0) {
		for (int i = 0; i < n; i++) if (fabs(rates[i] - wanted) < 1) return rates[i];
		return 0;
	}
	for (int i = 0; i < n; i++) if (half_span <= 0.4 * rates[i]) return rates[i];
	return rates[n - 1];
}

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void list_rates(const double *r, int n) {
	fprintf(stderr, "Rates offered:");
	for (int i = 0; i < n; i++) fprintf(stderr, " %.0f", r[i]);
	fprintf(stderr, "\n");
}

// ---- Airspy R2 / Mini ----
#ifdef HAVE_AIRSPY
static int airspy_cb(airspy_transfer_t *t) {
	live_t *l = t->ctx;
	if (t->dropped_samples) input_dropped += (long long)t->dropped_samples;
	ring_write(&l->ring, t->samples, t->sample_count);
	return 0;
}

static void airspy_stop(live_t *l) {
	airspy_stop_rx(l->dev); airspy_close(l->dev); ring_close(&l->ring);
}

static input_t *open_airspy(const char *arg, const live_opts_t *o) {
	struct airspy_device *dev;
	int r = arg && *arg ? airspy_open_sn(&dev, strtoull(arg, NULL, 16)) : airspy_open(&dev);
	if (r != AIRSPY_SUCCESS) { fprintf(stderr, "Airspy: cannot open (%s)%s\n", airspy_error_name(r),
		" — is it plugged in, and is the airspy kernel module unloaded (sudo modprobe -r airspy)?"); return NULL; }
	uint32_t count; airspy_get_samplerates(dev, &count, 0);
	uint32_t *list = malloc(count * sizeof *list); airspy_get_samplerates(dev, list, count);
	double rates[16]; int n = 0;
	for (uint32_t i = 0; i < count && n < 16; i++) rates[n++] = list[i];
	free(list);
	qsort(rates, n, sizeof *rates, cmp_double);
	double rate = pick_rate(rates, n, o->half_span, o->rate);
	if (!rate) { fprintf(stderr, "Airspy: %.0f S/s not offered. ", o->rate); list_rates(rates, n); airspy_close(dev); return NULL; }
	airspy_set_sample_type(dev, AIRSPY_SAMPLE_FLOAT32_IQ);
	airspy_set_samplerate(dev, (uint32_t)rate);
	int gain = o->gain ? atoi(o->gain) : 12;
	if (gain < 0) gain = 0;
	if (gain > 21) gain = 21;
	airspy_set_linearity_gain(dev, (uint8_t)gain);
	airspy_set_rf_bias(dev, o->bias ? 1 : 0);
	// Retuning to account for ppm: the R2 has no correction setting of its own.
	double tune = o->center_hz * (1 + o->ppm * 1e-6);
	char name[64]; snprintf(name, sizeof name, "Airspy %s", arg && *arg ? arg : "");
	input_t *in = live_new(rate, o->center_hz, name);
	live_t *l = in->priv; l->dev = dev; l->stop = airspy_stop;
	if ((r = airspy_start_rx(dev, airspy_cb, l)) != AIRSPY_SUCCESS) { fprintf(stderr, "Airspy: start failed (%s)\n", airspy_error_name(r)); l->stop = NULL; airspy_close(dev); live_close(in); return NULL; }
	// The firmware sets up the tuner on RX start, so the frequency goes after it (as airspy_rx does).
	airspy_set_freq(dev, (uint32_t)llround(tune));
	fprintf(stderr, "Airspy: %.0f S/s, linearity gain %d, bias-tee %s\n", rate, gain, o->bias ? "ON" : "off");
	return in;
}
#endif

// ---- HackRF ----
#ifdef HAVE_HACKRF
static int hackrf_cb(hackrf_transfer *t) {
	live_t *l = t->rx_ctx;
	int n = t->valid_length / 2;
	float *f = conv_buf(l, n);
	const int8_t *b = (const int8_t *)t->buffer;
	for (int i = 0; i < 2 * n; i++) f[i] = b[i] / 128.f;
	ring_write(&l->ring, f, n);
	return 0;
}

static void hackrf_stop(live_t *l) {
	hackrf_stop_rx(l->dev); hackrf_close(l->dev); hackrf_exit(); ring_close(&l->ring);
}

static input_t *open_hackrf(const char *arg, const live_opts_t *o) {
	hackrf_device *dev;
	hackrf_init();
	int r = hackrf_open_by_serial(arg && *arg ? arg : NULL, &dev);
	if (r != HACKRF_SUCCESS) { fprintf(stderr, "HackRF: cannot open (%s)\n", hackrf_error_name(r)); hackrf_exit(); return NULL; }
	// Below 2 MS/s the HackRF's filters are poor.
	static const double rates[] = {2e6, 2.5e6, 4e6, 5e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
	double rate = o->rate > 0 ? o->rate : pick_rate(rates, 9, o->half_span, 0);
	hackrf_set_sample_rate(dev, rate);
	hackrf_set_baseband_filter_bandwidth(dev, hackrf_compute_baseband_filter_bw((uint32_t)(0.75 * rate)));
	int lna = 32, vga = 20;
	if (o->gain) sscanf(o->gain, "%d,%d", &lna, &vga);
	hackrf_set_lna_gain(dev, (uint32_t)(lna / 8 * 8));
	hackrf_set_vga_gain(dev, (uint32_t)(vga / 2 * 2));
	hackrf_set_amp_enable(dev, o->amp ? 1 : 0);
	hackrf_set_antenna_enable(dev, o->bias ? 1 : 0);
	hackrf_set_freq(dev, (uint64_t)llround(o->center_hz * (1 + o->ppm * 1e-6)));
	char name[64]; snprintf(name, sizeof name, "HackRF %s", arg && *arg ? arg : "");
	input_t *in = live_new(rate, o->center_hz, name);
	live_t *l = in->priv; l->dev = dev; l->stop = hackrf_stop;
	if ((r = hackrf_start_rx(dev, hackrf_cb, l)) != HACKRF_SUCCESS) { fprintf(stderr, "HackRF: start failed (%s)\n", hackrf_error_name(r)); l->stop = NULL; hackrf_close(dev); live_close(in); return NULL; }
	fprintf(stderr, "HackRF: %.0f S/s, LNA %d dB, VGA %d dB, amp %s, antenna power %s\n", rate, lna / 8 * 8, vga / 2 * 2,
		o->amp ? "ON" : "off", o->bias ? "ON" : "off");
	return in;
}
#endif

// ---- Network helpers ----

static int tcp_connect(const char *hostport, const char *what) {
	char host[256]; const char *colon = strrchr(hostport, ':');
	if (!colon || colon == hostport || (size_t)(colon - hostport) >= sizeof host) { fprintf(stderr, "%s: want HOST:PORT\n", what); return -1; }
	memcpy(host, hostport, colon - hostport); host[colon - hostport] = 0;
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
	int err = getaddrinfo(host, colon + 1, &hints, &res);
	if (err) { fprintf(stderr, "%s %s: %s\n", what, hostport, gai_strerror(err)); return -1; }
	int fd = -1;
	for (struct addrinfo *a = res; a; a = a->ai_next) {
		fd = socket(a->ai_family, SOCK_STREAM, 0);
		if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
		if (fd >= 0) close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) { fprintf(stderr, "%s %s: cannot connect (%s)\n", what, hostport, strerror(errno)); return -1; }
	int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

static int read_full(int fd, void *buf, size_t n) {
	for (size_t got = 0; got < n;) {
		ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
		if (r <= 0) return -1;
		got += r;
	}
	return 0;
}

static void net_stop(live_t *l) {
	if (l->fd >= 0) shutdown(l->fd, SHUT_RDWR);
	if (l->running) pthread_join(l->thread, NULL);
	if (l->fd >= 0) close(l->fd);
	l->fd = -1;
	ring_close(&l->ring);
}

// ---- rtl_tcp (also the SDR Workbench's hackrf_tcp feeder) ----

static void rtl_cmd(int fd, int cmd, uint32_t v) {
	uint8_t b[5] = {(uint8_t)cmd, v >> 24, v >> 16, v >> 8, v};
	send(fd, b, 5, MSG_NOSIGNAL);
}

static void *rtl_thread(void *arg) {
	live_t *l = arg;
	uint8_t buf[65536];
	int carry = 0;
	for (;;) {
		ssize_t r = recv(l->fd, buf + carry, sizeof buf - carry, 0);
		if (r <= 0) break;
		int bytes = carry + (int)r, n = bytes / 2;
		float *f = conv_buf(l, n);
		for (int i = 0; i < 2 * n; i++) f[i] = (buf[i] - 127.5f) / 128.f;
		ring_write(&l->ring, f, n);
		carry = bytes & 1;
		if (carry) buf[0] = buf[bytes - 1];
	}
	fprintf(stderr, "rtl_tcp: connection closed\n");
	ring_close(&l->ring);
	return NULL;
}

static input_t *open_rtl_tcp(const char *hostport, const live_opts_t *o) {
	int fd = tcp_connect(hostport, "rtl_tcp");
	if (fd < 0) return NULL;
	uint8_t hello[12];
	if (read_full(fd, hello, 12) || memcmp(hello, "RTL0", 4)) { fprintf(stderr, "rtl_tcp %s: not an rtl_tcp server\n", hostport); close(fd); return NULL; }
	static const double rates[] = {250000, 1024000, 1536000, 1792000, 1920000, 2048000, 2160000, 2400000, 2560000, 2880000, 3200000};
	double rate = o->rate > 0 ? o->rate : pick_rate(rates, 11, o->half_span, 0);
	rtl_cmd(fd, 0x02, (uint32_t)rate);
	rtl_cmd(fd, 0x01, (uint32_t)llround(o->center_hz));
	if (o->ppm) rtl_cmd(fd, 0x05, (uint32_t)(int32_t)lround(o->ppm));
	if (o->gain) { rtl_cmd(fd, 0x03, 1); rtl_cmd(fd, 0x04, (uint32_t)lround(atof(o->gain) * 10)); }
	else rtl_cmd(fd, 0x03, 0);
	rtl_cmd(fd, 0x0e, o->bias ? 1 : 0);
	char name[128]; snprintf(name, sizeof name, "rtl_tcp %s", hostport);
	input_t *in = live_new(rate, o->center_hz, name);
	live_t *l = in->priv; l->fd = fd; l->stop = net_stop;
	l->running = !pthread_create(&l->thread, NULL, rtl_thread, l);
	fprintf(stderr, "rtl_tcp %s: %.0f S/s, gain %s, bias-tee %s\n", hostport, rate, o->gain ? o->gain : "auto", o->bias ? "ON" : "off");
	return in;
}

// ---- SpyServer (Airspy's protocol; layout follows the SDR Workbench's usb/drivers/spyserver.js) ----

enum { SS_CMD_HELLO = 0, SS_CMD_SET = 2 };
enum { SS_STREAMING_MODE = 0, SS_STREAMING_ENABLED = 1, SS_GAIN = 2, SS_IQ_FORMAT = 100, SS_IQ_FREQUENCY = 101, SS_IQ_DECIMATION = 102 };
enum { SS_MSG_DEVICE_INFO = 0, SS_MSG_CLIENT_SYNC = 1, SS_MSG_IQ_UINT8 = 100, SS_MSG_IQ_INT16 = 101 };
#define SS_PROTOCOL ((2u << 24) | 0x782)
#define SS_HEADER 20
// Default to the widest rate at or below this: remote links are usually the bottleneck.
#define SS_DEFAULT_MAX_RATE 160000

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void put32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }

static void ss_send(int fd, uint32_t type, const uint8_t *body, uint32_t len) {
	uint8_t h[8]; put32(h, type); put32(h + 4, len);
	send(fd, h, 8, MSG_NOSIGNAL); if (len) send(fd, body, len, MSG_NOSIGNAL);
}
static void ss_set(int fd, uint32_t id, uint32_t v) { uint8_t b[8]; put32(b, id); put32(b + 4, v); ss_send(fd, SS_CMD_SET, b, 8); }

// Reads one message; returns its type (low 16 bits) or -1. *body is reallocated to fit.
static int ss_message(int fd, uint8_t **body, uint32_t *cap, uint32_t *size) {
	uint8_t h[SS_HEADER];
	if (read_full(fd, h, SS_HEADER)) return -1;
	*size = le32(h + 16);
	if (*size > (64u << 20)) return -1;
	if (*size > *cap) { *cap = *size; *body = realloc(*body, *cap); }
	if (*size && read_full(fd, *body, *size)) return -1;
	return (int)(le32(h + 4) & 0xffff);
}

static void *ss_thread(void *arg) {
	live_t *l = arg;
	uint8_t *body = NULL; uint32_t cap = 0, size;
	int type;
	while ((type = ss_message(l->fd, &body, &cap, &size)) >= 0) {
		if (type == SS_MSG_IQ_INT16) {
			int n = size / 4; float *f = conv_buf(l, n);
			for (int i = 0; i < 2 * n; i++) { int16_t v = (int16_t)(body[2 * i] | body[2 * i + 1] << 8); f[i] = v / 32768.f; }
			ring_write(&l->ring, f, n);
		} else if (type == SS_MSG_IQ_UINT8) {
			int n = size / 2; float *f = conv_buf(l, n);
			for (int i = 0; i < 2 * n; i++) f[i] = (body[i] - 128) / 128.f;
			ring_write(&l->ring, f, n);
		}
	}
	free(body);
	fprintf(stderr, "SpyServer: connection closed\n");
	ring_close(&l->ring);
	return NULL;
}

static input_t *open_spyserver(const char *hostport, const live_opts_t *o) {
	int fd = tcp_connect(hostport, "SpyServer");
	if (fd < 0) return NULL;
	struct timeval tv = {15, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	const char *client = "inmarsat-rx";
	uint8_t hello[64]; put32(hello, SS_PROTOCOL); memcpy(hello + 4, client, strlen(client));
	ss_send(fd, SS_CMD_HELLO, hello, 4 + (uint32_t)strlen(client));
	// The node answers with DeviceInfo, then ClientSync; settings sent before the sync are dropped.
	uint8_t *body = NULL; uint32_t cap = 0, size;
	uint32_t max_rate = 0, stages = 0, gain_count = 0, min_dec = 0, forced = 0, min_iq = 0, max_iq = 0;
	int have_info = 0, have_sync = 0, can_control = 0, type;
	while (!(have_info && have_sync) && (type = ss_message(fd, &body, &cap, &size)) >= 0) {
#define U(o) ((o) + 4 <= size ? le32(body + (o)) : 0)
		if (type == SS_MSG_DEVICE_INFO && size >= 0x30) {
			max_rate = U(0x08); stages = U(0x10); gain_count = U(0x18); min_dec = U(0x28); forced = U(0x2c);
			min_iq = U(0x1c); max_iq = U(0x20); have_info = 1;
		} else if (type == SS_MSG_CLIENT_SYNC) {
			can_control = U(0x00) != 0;
			// Some nodes send a shorter sync than the reference client expects; read only what arrived.
			if (U(0x18)) { min_iq = U(0x14); max_iq = U(0x18); }
			have_sync = 1;
		}
#undef U
	}
	free(body);
	if (!have_info || !have_sync) { fprintf(stderr, "SpyServer %s: no device info / sync from the node\n", hostport); close(fd); return NULL; }
	tv.tv_sec = 0; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (o->center_hz < min_iq || o->center_hz > max_iq) {
		fprintf(stderr, "SpyServer %s: %.4f MHz is outside what this node serves (%.4f–%.4f MHz)\n", hostport,
			o->center_hz / 1e6, min_iq / 1e6, max_iq / 1e6);
		close(fd); return NULL;
	}
	// Real rates are max_rate / 2^n, for n from the enforced minimum up to the stage count.
	double rates[32]; int decs[32], n = 0;
	for (int d = (int)stages; d >= (int)min_dec && n < 32; d--) { rates[n] = max_rate / pow(2, d); decs[n++] = d; }
	double rate = 0; int dec = -1;
	if (o->rate > 0) { for (int i = 0; i < n; i++) if (fabs(rates[i] - o->rate) < 1) { rate = rates[i]; dec = decs[i]; } }
	else {
		for (int i = 0; i < n && dec < 0; i++) if (o->half_span <= 0.4 * rates[i]) { rate = rates[i]; dec = decs[i]; }
		if (dec < 0) { rate = rates[n - 1]; dec = decs[n - 1]; }
		if (rate > SS_DEFAULT_MAX_RATE && o->half_span <= 0.4 * SS_DEFAULT_MAX_RATE)
			for (int i = n - 1; i >= 0; i--) if (rates[i] <= SS_DEFAULT_MAX_RATE) { rate = rates[i]; dec = decs[i]; break; }
	}
	if (dec < 0) { fprintf(stderr, "SpyServer: %.0f S/s not offered. ", o->rate); list_rates(rates, n); close(fd); return NULL; }
	ss_set(fd, SS_STREAMING_MODE, 1);                 // IQ only.
	ss_set(fd, SS_IQ_FORMAT, forced ? forced : 2);    // 2 = int16.
	ss_set(fd, SS_IQ_DECIMATION, (uint32_t)dec);
	ss_set(fd, SS_IQ_FREQUENCY, (uint32_t)llround(o->center_hz));
	if (o->gain && can_control && gain_count) {
		int g = atoi(o->gain); if (g > (int)gain_count - 1) g = (int)gain_count - 1; if (g < 0) g = 0;
		ss_set(fd, SS_GAIN, (uint32_t)g);
	}
	ss_set(fd, SS_STREAMING_ENABLED, 1);
	char name[128]; snprintf(name, sizeof name, "SpyServer %s", hostport);
	input_t *in = live_new(rate, o->center_hz, name);
	live_t *l = in->priv; l->fd = fd; l->stop = net_stop;
	l->running = !pthread_create(&l->thread, NULL, ss_thread, l);
	fprintf(stderr, "SpyServer %s: %.0f S/s (decimation %d), %s\n", hostport, rate, dec,
		can_control ? "control granted" : "no control: gain fixed by the node");
	return in;
}

// ---- Dispatch ----

int input_is_live(const char *spec) {
	return !strncmp(spec, "airspy", 6) || !strncmp(spec, "hackrf", 6) || !strncmp(spec, "rtl_tcp:", 8) || !strncmp(spec, "spyserver:", 10);
}

input_t *input_open_live(const char *spec, const live_opts_t *o) {
	const char *arg = strchr(spec, ':');
	arg = arg ? arg + 1 : NULL;
	if (!strncmp(spec, "rtl_tcp:", 8)) return open_rtl_tcp(arg, o);
	if (!strncmp(spec, "spyserver:", 10)) return open_spyserver(arg, o);
	if (!strncmp(spec, "airspy", 6)) {
#ifdef HAVE_AIRSPY
		return open_airspy(arg, o);
#else
		fprintf(stderr, "Built without libairspy\n"); return NULL;
#endif
	}
	if (!strncmp(spec, "hackrf", 6)) {
#ifdef HAVE_HACKRF
		return open_hackrf(arg, o);
#else
		fprintf(stderr, "Built without libhackrf\n"); return NULL;
#endif
	}
	return NULL;
}
