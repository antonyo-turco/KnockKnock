#ifndef TINYML_TRAINING_H
#define TINYML_TRAINING_H

#include <Arduino.h>
#include "config.h"
#include "feature_extraction.h"

// ─────────────────────────────────────────────
//  Tuneable parameters
// ─────────────────────────────────────────────

static constexpr int   KMEANS_K          = 4;

// 13 features (spectral flux removed):
//   [0]  impact_score
//   [1]  m_p99       [2]  x_p99       [3]  y_p99       [4]  z_p99
//   [5]  m_jerk_max  [6]  x_jerk_max  [7]  y_jerk_max  [8]  z_jerk_max
//   [9]  m_band_20_40 [10] x_band_20_40 [11] y_band_20_40 [12] z_band_20_40
static constexpr int   FEATURE_DIM       = 13;

// Threshold = max(mean_dist + SIGMA_MULT * sigma_dist,  dist_max * MAX_DIST_MARGIN)
static constexpr float SIGMA_MULT        = 3.0f;
static constexpr float MAX_DIST_MARGIN   = 1.1f;

// Absolute floor — avoids hair-trigger alarms on perfectly silent clusters
static constexpr float MIN_THRESHOLD     = 0.30f;

// ─────────────────────────────────────────────
//  Model struct  (saved to NVS after training)
// ─────────────────────────────────────────────

struct KMeansModel {
    // Centroids in normalised (z-score) space
    float    centroids[KMEANS_K][FEATURE_DIM];
    uint32_t centroid_counts[KMEANS_K];

    // Per-cluster distance statistics — Welford online
    float    dist_mean[KMEANS_K];
    float    dist_M2[KMEANS_K];
    float    dist_max[KMEANS_K];         // max distance seen during training
    uint32_t dist_n[KMEANS_K];
    float    dist_threshold[KMEANS_K];   // set by training_finalize()

    // Global per-feature normalisation — Welford, raw space
    float    norm_mean[FEATURE_DIM];
    float    norm_M2[FEATURE_DIM];
    float    norm_std[FEATURE_DIM];      // frozen by training_finalize()
    uint32_t norm_n;

    uint32_t total_samples;
    bool     initialised;   // true after k++ seeding is complete
    bool     finalised;     // true after training_finalize()
};

// ─────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────

void training_init    (KMeansModel &model);
void training_update  (KMeansModel &model, const InferenceFeatures &features);
void training_finalize(KMeansModel &model);

bool training_save    (const KMeansModel &model);
bool training_load    (KMeansModel &model);
void training_erase_nvs();

// Returns true = within learned baseline.
// out_distance (optional): normalised Euclidean distance to nearest centroid.
bool training_is_baseline(const KMeansModel &model,
                           const InferenceFeatures &features,
                           float *out_distance = nullptr);

void training_print_model(const KMeansModel &model);

#endif // TINYML_TRAINING_H
