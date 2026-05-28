#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Feature vector — 51 dimensions total:
//
//  [0]     impact_score          weighted composite in [0,1]
//  [1- 4]  m/x/y/z  p99          99th percentile of |signal|
//  [5- 8]  m/x/y/z  jerk_max     max |sample[i] - sample[i-1]|
//  [9-12]  m/x/y/z  band_20_40   band power 20-40 Hz
//  [13-16] m/x/y/z  band_40_100  band power 40-100 Hz
//  [17-20] m/x/y/z  band_1_5     band power  1-5  Hz
//  [21-24] m/x/y/z  band_5_20    band power  5-20 Hz
//  [25-27] x/y/z    zcr          zero-crossing rate  (crossings / sample)
//  [28-34] x  top7_freq[0..6]    Hz of 7 highest-magnitude FFT bins, ascending
//  [35-41] y  top7_freq[0..6]
//  [42-48] z  top7_freq[0..6]
//  [49]    time_sin              sin(2pi * minute_of_day / 1440)
//  [50]    time_cos              cos(2pi * minute_of_day / 1440)

#define TOP7_COUNT 7

typedef struct InferenceFeatures {
    float impact_score;

    float m_p99,  x_p99,  y_p99,  z_p99;
    float m_jerk_max, x_jerk_max, y_jerk_max, z_jerk_max;
    float m_band_20_40, x_band_20_40, y_band_20_40, z_band_20_40;
    float m_band_40_100, x_band_40_100, y_band_40_100, z_band_40_100;
    float m_band_1_5, x_band_1_5, y_band_1_5, z_band_1_5;
    float m_band_5_20, x_band_5_20, y_band_5_20, z_band_5_20;
    float x_zcr, y_zcr, z_zcr;

    float x_top7_freq[TOP7_COUNT];
    float y_top7_freq[TOP7_COUNT];
    float z_top7_freq[TOP7_COUNT];

    // Circular time-of-day encoding — avoids the 23:59/00:01 discontinuity.
    // Both values are needed: sin alone is ambiguous (morning == afternoon).
    float time_sin;
    float time_cos;
} InferenceFeatures;

// Compute all features for one window.
// time_sin and time_cos are passed in from the caller to keep this module
// independent of WiFi/RTC state.
InferenceFeatures compute_features(
    const int16_t x_values[],
    const int16_t y_values[],
    const int16_t z_values[],
    int           size,
    float         sampling_rate_hz,
    float         time_sin,
    float         time_cos);

// Convert current RTC time to (time_sin, time_cos).
// Returns false if the RTC has not been synced (time is invalid).
bool get_time_features(float *time_sin_out, float *time_cos_out);

#ifdef __cplusplus
}
#endif

#endif // FEATURE_EXTRACTION_H
