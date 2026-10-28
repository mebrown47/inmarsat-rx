// Inmarsat-C (STD-C) NCS TDM decoder: frame sync, de-permutation, de-interleaving, soft Viterbi,
// descrambling, packet framing/checksum, bulletin boards and EGC message reassembly.
#ifndef STDC_H
#define STDC_H

typedef struct {
	int id, service, repetition, complete, parts;
	const char *service_name, *priority;
	const char *text;
} stdc_egc_t;

typedef struct {
	int frame_number, les, version;
	double utc_seconds;
	const char *satellite;
} stdc_bulletin_t;

typedef struct {
	int synced, sync_score; long frames, dropped, packets_ok, packets_bad; double ber;  // ber < 0: none yet.
} stdc_stats_t;

typedef void (*stdc_egc_cb)(const stdc_egc_t *msg, void *user);
typedef void (*stdc_bulletin_cb)(const stdc_bulletin_t *b, void *user);

typedef struct stdc stdc_t;
stdc_t *stdc_new(stdc_egc_cb on_egc, stdc_bulletin_cb on_bulletin, void *user);
void stdc_free(stdc_t *s);
// soft: one value per 1200 Bd symbol (positive = 1). now_ms: current time, for EGC expiry.
void stdc_feed(stdc_t *s, const float *soft, int n, double now_ms);
// Emits EGC messages still waiting for parts (end of input).
void stdc_flush(stdc_t *s);
stdc_stats_t stdc_stats(const stdc_t *s);

#endif
