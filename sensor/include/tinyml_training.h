#ifndef TINYML_TRAINING_H
#define TINYML_TRAINING_H

#include "config.h"
#include "feature_extraction.h"

#define KMEANS_K        10

// 51 features — see feature_extraction.h for full index map
#define FEATURE_DIM     51

// Per-centroid anomaly threshold: max(mean_dist + SIGMA_MULT * sigma,
//                                    dist_max * MAX_DIST_MARGIN, MIN_THRESHOLD)
#define SIGMA_MULT      3.0f
#define MAX_DIST_MARGIN 1.1f
#define MIN_THRESHOLD   0.30f

// Minimum novelty buffer entries required before k++ seeding is allowed.
// Must be >= KMEANS_K.
#define MIN_NOVELTY_FOR_SEEDING 20

typedef struct KMeansModel {
    float    centroids[KMEANS_K][FEATURE_DIM];
    uint32_t centroid_counts[KMEANS_K];

    float    dist_mean[KMEANS_K];
    float    dist_M2[KMEANS_K];
    float    dist_max[KMEANS_K];
    uint32_t dist_n[KMEANS_K];
    float    dist_threshold[KMEANS_K];

    // Normalisation stats — frozen at end of EXPLORING, applied during TRAINING
    float    norm_mean[FEATURE_DIM];
    float    norm_M2[FEATURE_DIM];
    float    norm_std[FEATURE_DIM];
    uint32_t norm_n;

    uint32_t total_samples;
    bool     initialised;   // true after k++ seeding (end of EXPLORING)
    bool     finalised;     // true after training_finalize() (end of TRAINING)

    // Suggested wakeup threshold calibrated to this installation during EXPLORING.
    // Set by exploring_finalize() from m_p99 Welford stats. 0 = not computed.
    // Units: mg (±2 g range at 1 mg/LSB).
    float    suggested_threshold_mg;
} KMeansModel;

// Phase 0: init
void training_init(KMeansModel *model);

// Phase 1: EXPLORING
// Call every window. Updates Welford normalisation and novelty buffer.
void exploring_update(KMeansModel *model, const InferenceFeatures *features);

// Call once at end of EXPLORING. Freezes normalisation, runs k++ seeding.
// Returns false if too few novel samples were collected.
bool exploring_finalize(KMeansModel *model);

// Novelty buffer fill level (0-100 %).
uint8_t exploring_novelty_pct(void);

// Phase 2: TRAINING
// Call every window. Updates centroids and distance stats.
void training_update(KMeansModel *model, const InferenceFeatures *features);

// Call once at end of TRAINING. Computes thresholds, marks model finalised.
void training_finalize(KMeansModel *model);

// NVS persistence
bool training_save(const KMeansModel *model);
bool training_load(KMeansModel *model);
void training_erase_nvs(void);

// Inference: returns true = baseline. Outputs distance and cluster index.
bool training_is_baseline(const KMeansModel *model,
                           const InferenceFeatures *features,
                           float *out_distance,
                           int   *out_cluster);

void training_print_model(const KMeansModel *model);

#endif // TINYML_TRAINING_H
