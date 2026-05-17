#ifndef TINYML_TRAINING_H
#define TINYML_TRAINING_H

#include <Arduino.h>
#include "config.h"
#include "feature_extraction.h"

// ─────────────────────────────────────────────
//  Tuneable parameters
// ─────────────────────────────────────────────

static constexpr int   KMEANS_K        = 10;

// 47 features — 45 vibration + 2 time-of-day (time_sin, time_cos)
static constexpr int   FEATURE_DIM     = 47;

static constexpr float SIGMA_MULT      = 3.0f;
static constexpr float MAX_DIST_MARGIN = 1.1f;
static constexpr float MIN_THRESHOLD   = 0.30f;

static constexpr int   MIN_NOVELTY_FOR_SEEDING = 20;

// ─────────────────────────────────────────────
//  Model struct
// ─────────────────────────────────────────────

struct KMeansModel {
    float    centroids[KMEANS_K][FEATURE_DIM];
    uint32_t centroid_counts[KMEANS_K];

    float    dist_mean[KMEANS_K];
    float    dist_M2[KMEANS_K];
    float    dist_max[KMEANS_K];
    uint32_t dist_n[KMEANS_K];
    float    dist_threshold[KMEANS_K];

    float    norm_mean[FEATURE_DIM];
    float    norm_M2[FEATURE_DIM];
    float    norm_std[FEATURE_DIM];
    uint32_t norm_n;

    uint32_t total_samples;
    bool     initialised;
    bool     finalised;
};

// ─────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────

void    training_init        (KMeansModel &model);

void    exploring_update     (KMeansModel &model, const InferenceFeatures &features);
bool    exploring_finalize   (KMeansModel &model);
uint8_t exploring_novelty_pct();

void    training_update      (KMeansModel &model, const InferenceFeatures &features);
void    training_finalize    (KMeansModel &model);

bool    training_save        (const KMeansModel &model);
bool    training_load        (KMeansModel &model);
void    training_erase_nvs   ();

bool    training_is_baseline (const KMeansModel &model,
                               const InferenceFeatures &features,
                               float *out_distance = nullptr,
                               int   *out_cluster  = nullptr);

void    training_print_model (const KMeansModel &model);

#endif // TINYML_TRAINING_H
