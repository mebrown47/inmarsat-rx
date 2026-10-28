// K=7, rate 1/2 convolutional code used by Inmarsat STD-C and Aero (taps 0x5B/0x79 with the
// newest input bit in bit 6 of the register; JAERO's 109/79 are the reversed convention).
#ifndef FEC_H
#define FEC_H

#include <stdint.h>

// Soft-decision Viterbi over n coded values (positive = 1); writes n/2 bits to out. The encoder
// state at the start is unknown (it runs on from the previous frame), so all states start equal.
// Returns the encoder state at the start of the chosen path.
int fec_viterbi(const float *soft, int n, uint8_t *out);
// Encodes nbits bits from start_state into 2*nbits coded bits.
void fec_encode(const uint8_t *bits, int nbits, int start_state, uint8_t *out);
// Channel BER estimate: re-encodes the decision and compares with the hard received bits.
double fec_ber(const float *soft, int n, const uint8_t *bits, int start_state);

#endif
