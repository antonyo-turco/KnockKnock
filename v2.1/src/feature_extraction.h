#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <Arduino.h>
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Feature vector — 47 dimensions total
//
//  [0]     impact_score
//  [1- 4]  m/x/y/z  p99
//  [5- 8]  m/x/y/z  jerk_max
//  [9-12]  m/x/y/z  band_20_40
//  [13-16] m/x/y/z  band_1_5
//  [17-20] m/x/y/z  band_5_20
//  [21-23] x/y/z    zcr
//  [24-30] x        top7_freq[0..6]
//  [31-37] y        top7_freq[0..6]
//  [38-44] z        top7_freq[0..6]
//  [45]    time_sin   sin(2π * minute_of_day / 1440)
//  [46]    time_cos   cos(2π * minute_of_day / 1440)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int TOP7_COUNT = 7;

struct InferenceFeatures {
    float impact_score;

    float m_p99,  x_p99,  y_p99,  z_p99;
    float m_jerk_max, x_jerk_max, y_jerk_max, z_jerk_max;
    float m_band_20_40, x_band_20_40, y_band_20_40, z_band_20_40;
    float m_band_1_5,   x_band_1_5,   y_band_1_5,   z_band_1_5;
    float m_band_5_20,  x_band_5_20,  y_band_5_20,  z_band_5_20;
    float x_zcr, y_zcr, z_zcr;
    float x_top7_freq[TOP7_COUNT];
    float y_top7_freq[TOP7_COUNT];
    float z_top7_freq[TOP7_COUNT];

    // Circular time-of-day encoding — avoids the 23:59/00:01 discontinuity.
    // Both are needed: sin alone is ambiguous (morning == afternoon).
    float time_sin;   // sin(2π * minute_of_day / 1440)
    float time_cos;   // cos(2π * minute_of_day / 1440)
};

// Compute all features for one window.
// time_sin and time_cos must be computed by the caller from the current RTC
// time and passed in — this keeps feature_extraction independent of WiFi/RTC.
InferenceFeatures compute_features(
    const int16_t x_values[],
    const int16_t y_values[],
    const int16_t z_values[],
    int           size,
    float         sampling_rate_hz,
    float         time_sin,
    float         time_cos);

// Helper: convert current RTC time to (time_sin, time_cos).
// Returns false if the RTC has not been synced yet (time is invalid).
bool get_time_features(float &time_sin_out, float &time_cos_out);

#endif // FEATURE_EXTRACTION_H
