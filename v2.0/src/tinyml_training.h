#ifndef TINYML_TRAINING_H
#define TINYML_TRAINING_H

#include <Arduino.h>
#include "config.h"
#include "feature_extraction.h"

// ─────────────────────────────────────────────
//  Tuneable parameters
// ─────────────────────────────────────────────

static constexpr int   KMEANS_K        = 4;

// 45 features — see feature_extraction.h for full index map
static constexpr int   FEATURE_DIM     = 45;

// Threshold = max(mean_dist + SIGMA_MULT * sigma, dist_max * MAX_DIST_MARGIN)
static constexpr float SIGMA_MULT      = 3.0f;
static constexpr float MAX_DIST_MARGIN = 1.1f;
static constexpr float MIN_THRESHOLD   = 0.30f;

// Minimum novelty buffer entries required before k++ is allowed.
// Must be >= KMEANS_K.  Warn if fewer entries collected at end of EXPLORING.
static constexpr int   MIN_NOVELTY_FOR_SEEDING = 20;

// ─────────────────────────────────────────────
//  Model struct  (saved to NVS after TRAINING)
// ─────────────────────────────────────────────

struct KMeansModel {
    float    centroids[KMEANS_K][FEATURE_DIM];
    uint32_t centroid_counts[KMEANS_K];

    float    dist_mean[KMEANS_K];
    float    dist_M2[KMEANS_K];
    float    dist_max[KMEANS_K];
    uint32_t dist_n[KMEANS_K];
    float    dist_threshold[KMEANS_K];

    // Normalisation — frozen at end of EXPLORING, used during TRAINING
    float    norm_mean[FEATURE_DIM];
    float    norm_M2[FEATURE_DIM];
    float    norm_std[FEATURE_DIM];
    uint32_t norm_n;

    uint32_t total_samples;
    bool     initialised;   // true after k++ seeding (end of EXPLORING)
    bool     finalised;     // true after training_finalize() (end of TRAINING)
};

// ─────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────

// ── Phase 0: init ────────────────────────────
void training_init(KMeansModel &model);

// ── Phase 1: EXPLORING (24 h) ────────────────
// Call every window.  Updates Welford normalisation + novelty buffer.
void exploring_update(KMeansModel &model, const InferenceFeatures &features);

// Call once at end of EXPLORING.
// Freezes normalisation, runs k++ on novelty buffer → centroids placed.
// Returns false if too few novel samples were collected (model may be poor).
bool exploring_finalize(KMeansModel &model);

// How full is the novelty buffer? (0-100 %)
uint8_t exploring_novelty_pct();

// ── Phase 2: TRAINING (24 h) ─────────────────
// Call every window.  Updates centroids + distance stats with frozen normalisation.
void training_update(KMeansModel &model, const InferenceFeatures &features);

// Call once at end of TRAINING.  Computes thresholds and marks model finalised.
void training_finalize(KMeansModel &model);

// ── NVS persistence ───────────────────────────
bool training_save(const KMeansModel &model);
bool training_load(KMeansModel &model);
void training_erase_nvs();

// ── Inference ────────────────────────────────
// Returns true = baseline.  out_distance = normalised dist to nearest centroid.
bool training_is_baseline(const KMeansModel &model,
                           const InferenceFeatures &features,
                           float *out_distance = nullptr,
                           int   *out_cluster  = nullptr);

void training_print_model(const KMeansModel &model);

#endif // TINYML_TRAINING_H
