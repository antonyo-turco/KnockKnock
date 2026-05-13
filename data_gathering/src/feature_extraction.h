#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <Arduino.h>

struct InferenceFeatures {
    float impact_score;

    // magnitude features
    float m_p99;
    float m_jerk_max;
    float m_band_20_40;
    float m_spectral_flux;

    // per-axis features
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

InferenceFeatures compute_features(const int16_t x_values[], const int16_t y_values[], const int16_t z_values[], int size, float sampling_rate_hz);

#endif
