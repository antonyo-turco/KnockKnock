#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <Arduino.h>
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Feature vector produced by compute_features() for one window.
//
//  Magnitude features operate on the Euclidean magnitude sqrt(x²+y²+z²).
//  Per-axis features treat each axis independently (signed float values).
// ─────────────────────────────────────────────────────────────────────────────

struct InferenceFeatures {
    // Composite score in [0, 1] — weighted combination of the three
    // normalised magnitude sub-scores (p99, jerk, band power).
    float impact_score;

    // ── Magnitude features ───────────────────────────────────────────────────
    float m_p99;          // 99th percentile of |magnitude| in window
    float m_jerk_max;     // max sample-to-sample delta of magnitude
    float m_band_20_40;   // band power 20–40 Hz of magnitude signal

    // ── Per-axis features ────────────────────────────────────────────────────
    float x_p99;
    float y_p99;
    float z_p99;

    float x_jerk_max;
    float y_jerk_max;
    float z_jerk_max;

    float x_band_20_40;
    float y_band_20_40;
    float z_band_20_40;
};

// Compute all features for one window of SAMPLE_COUNT samples.
InferenceFeatures compute_features(
    const int16_t x_values[],
    const int16_t y_values[],
    const int16_t z_values[],
    int size,
    float sampling_rate_hz);

#endif // FEATURE_EXTRACTION_H
