#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <Arduino.h>

struct InferenceFeatures {
    float impact_score;
    float m_p99;
    float m_jerk_max;
    float m_band_20_40;
    float m_spectral_flux;
};

InferenceFeatures compute_features(const int16_t x_values[], const int16_t y_values[], const int16_t z_values[], int size, float sampling_rate_hz);

#endif
