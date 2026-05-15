#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <Arduino.h>
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Feature vector — 45 dimensions total
//
//  [0]     impact_score          weighted composite in [0,1]
//
//  [1- 4]  m/x/y/z  p99          99th percentile of |signal|
//  [5- 8]  m/x/y/z  jerk_max     max |sample[i] - sample[i-1]|
//  [9-12]  m/x/y/z  band_20_40   band power 20-40 Hz
//  [13-16] m/x/y/z  band_1_5     band power  1-5  Hz
//  [17-20] m/x/y/z  band_5_20    band power  5-20 Hz
//
//  [21-23] x/y/z    zcr           zero-crossing rate  (crossings / sample)
//
//  [24-30] x  top7_freq[0..6]    Hz of 7 highest-magnitude FFT bins, ascending
//  [31-37] y  top7_freq[0..6]
//  [38-44] z  top7_freq[0..6]
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int TOP7_COUNT = 7;

struct InferenceFeatures {
    float impact_score;

    // ── p99 ──────────────────────────────────────────────────────────────────
    float m_p99,  x_p99,  y_p99,  z_p99;

    // ── jerk_max ─────────────────────────────────────────────────────────────
    float m_jerk_max, x_jerk_max, y_jerk_max, z_jerk_max;

    // ── band power 20-40 Hz ──────────────────────────────────────────────────
    float m_band_20_40, x_band_20_40, y_band_20_40, z_band_20_40;

    // ── band power 1-5 Hz ────────────────────────────────────────────────────
    float m_band_1_5, x_band_1_5, y_band_1_5, z_band_1_5;

    // ── band power 5-20 Hz ───────────────────────────────────────────────────
    float m_band_5_20, x_band_5_20, y_band_5_20, z_band_5_20;

    // ── zero-crossing rate ───────────────────────────────────────────────────
    float x_zcr, y_zcr, z_zcr;

    // ── top-7 dominant frequencies per axis (Hz, ascending) ─────────────────
    float x_top7_freq[TOP7_COUNT];
    float y_top7_freq[TOP7_COUNT];
    float z_top7_freq[TOP7_COUNT];
};

InferenceFeatures compute_features(
    const int16_t x_values[],
    const int16_t y_values[],
    const int16_t z_values[],
    int           size,
    float         sampling_rate_hz);

#endif // FEATURE_EXTRACTION_H
