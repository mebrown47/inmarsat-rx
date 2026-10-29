// Copyright (c) 2026 Mike Brown
// SPDX-License-Identifier: GPL-3.0-or-later
// Carrier discovery for --scan: an averaged power spectrum of the input, and the carriers that
// stand above the local noise floor, with their width (which says what they might be).
#ifndef SCAN_H
#define SCAN_H

typedef struct { double freq, width, snr_db; } carrier_t;

typedef struct scan scan_t;
// period_s: seconds of input averaged per report (one FFT per second).
scan_t *scan_new(double rate, double centre, double period_s);
void scan_free(scan_t *s);
void scan_feed(scan_t *s, const float *iq, int n);
// After each period: fills up to max carriers, strongest first; returns how many (-1: not yet).
int scan_report(scan_t *s, carrier_t *out, int max);

#endif
