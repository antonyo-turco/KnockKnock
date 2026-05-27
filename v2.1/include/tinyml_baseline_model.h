#pragma once

#include <math.h>

// Auto-generated from data_analysis/train_tinyml_baseline.py
// Baseline detector based on exported feature statistics.

#define TINYML_BASELINE_FEATURE_COUNT 5
#define TINYML_BASELINE_ALLOWED_VIOLATIONS 1

#define TINYML_IMPACT_SCORE_THRESHOLD 0.110681064f
#define TINYML_M_P99_THRESHOLD 1107.423083984f
#define TINYML_M_JERK_MAX_THRESHOLD 97.504253790f
#define TINYML_M_BAND_20_40_THRESHOLD 122851.425368300f
#define TINYML_M_SPECTRAL_FLUX_THRESHOLD 0.007801546f

static inline int tinyml_is_baseline_sample(
    float impact_score,
    float m_p99,
    float m_jerk_max,
    float m_band_20_40,
    float m_spectral_flux) {
    int violations = 0;
    if (impact_score > TINYML_IMPACT_SCORE_THRESHOLD) {
        violations++;
    }
    if (m_p99 > TINYML_M_P99_THRESHOLD) {
        violations++;
    }
    if (m_jerk_max > TINYML_M_JERK_MAX_THRESHOLD) {
        violations++;
    }
    if (m_band_20_40 > TINYML_M_BAND_20_40_THRESHOLD) {
        violations++;
    }
    if (m_spectral_flux > TINYML_M_SPECTRAL_FLUX_THRESHOLD) {
        violations++;
    }

    return violations <= TINYML_BASELINE_ALLOWED_VIOLATIONS;
}
