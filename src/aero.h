// Inmarsat Aero P-channel decoders: Aero-H+ (10.5 kbit/s OQPSK) and Aero-L (600/1200 bit/s MSK).
// Frame sync, de-interleaving, soft Viterbi, descrambling, signal-unit CRC, ISU/SSU reassembly
// and ACARS parsing.
#ifndef AERO_H
#define AERO_H

#include <stdint.h>

typedef struct {
	char aes[7], ges[3], reg[8], label[3];
	char mode, tak, block;
	int more;
	char *text;   // NUL-terminated; empty when the block has no text.
} aero_acars_t;

typedef struct {
	int synced; long frames, su_ok, su_bad, messages; double ber;  // ber < 0: none yet.
} aero_stats_t;

typedef void (*aero_acars_cb)(const aero_acars_t *msg, void *user);
// Non-ACARS user data (aes, ges, payload bytes).
typedef void (*aero_data_cb)(const char *aes, const char *ges, const uint8_t *data, int len, void *user);

typedef struct aero aero_t;
// bit_rate: 10500 (Aero-H+) or 600 / 1200 (Aero-L).
aero_t *aero_new(int bit_rate, aero_acars_cb on_acars, aero_data_cb on_data, void *user);
void aero_free(aero_t *a);
// Aero-H+: interleaved I/Q soft symbols from the OQPSK demodulator.
void aero_feed_symbols(aero_t *a, const float *iq, int n);
// Aero-L: soft data bits (positive = 1) from the MSK demodulator.
void aero_feed_bits(aero_t *a, const float *bits, int n);
aero_stats_t aero_stats(const aero_t *a);
// Report every ACARS block as it arrives instead of joining multi-block (ETB) messages.
void aero_set_raw_blocks(aero_t *a, int on);

uint16_t aero_su_crc(const uint8_t *bytes);

#endif
