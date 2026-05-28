#ifndef SLIDING_WINDOW_H
#define SLIDING_WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include "feature_extraction.h"
#include "tinyml_training.h"

/**
 * @file sliding_window.h
 * @brief Rolling window + majority voting for anomaly detection
 *
 * Implements a 256-sample sliding window with 50-sample step.
 * Produces a binary anomaly decision every step, maintained in a 20-decision
 * ring buffer (5 seconds).  Alarm triggered when vote_count >= VOTE_THRESHOLD.
 */

// ─────────────────────────────────────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────────────────────────────────────

#define WINDOW_SAMPLES   256    // 256 samples @ 200 Hz = 1.28 seconds
#define STEP_SAMPLES     50     // 50 samples @ 200 Hz = 0.25 seconds
#define VOTE_WINDOW_N    20     // Ring buffer of 20 decisions = 5 seconds
#define VOTE_THRESHOLD   3      // Alarm when >= 3 anomalies in lookback

// Derived: circular buffer size = WINDOW_SAMPLES + STEP_SAMPLES * (VOTE_WINDOW_N - 1)
#define CIRCULAR_BUFFER_SIZE (WINDOW_SAMPLES + STEP_SAMPLES * (VOTE_WINDOW_N - 1))

// ─────────────────────────────────────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    // Circular buffers (one for each axis)
    int16_t buffer_x[CIRCULAR_BUFFER_SIZE];
    int16_t buffer_y[CIRCULAR_BUFFER_SIZE];
    int16_t buffer_z[CIRCULAR_BUFFER_SIZE];
    
    // Write index (incremented after each add_sample)
    int write_idx;
    
    // Count of new samples since last extraction
    int new_sample_count;
    
    // Ring buffer of binary decisions: 0 = normal, 1 = anomaly
    uint8_t vote_ring[VOTE_WINDOW_N];
    int vote_idx;
    int vote_count;  // sum of vote_ring elements
    
    // Last computed features and inference result
    InferenceFeatures last_features;
    float last_distance;
    int last_cluster;
    
    // Reference to the trained model (set at init)
    const KMeansModel *model;
    
    // Flag: true if window is ready for feature extraction
    bool ready;
} SlidingWindowState;

// ─────────────────────────────────────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Initialize the sliding window state.
 * @param state Pointer to state struct
 * @param model Reference to trained KMeans model (used for inference)
 */
void sliding_window_init(SlidingWindowState *state, const KMeansModel *model);

/**
 * @brief Add a single sample to the window.
 * @param state Pointer to state struct
 * @param x,y,z Raw accelerometer values (int16_t)
 * @return true if window now has STEP_SAMPLES new samples (ready for extraction)
 */
bool sliding_window_add_sample(SlidingWindowState *state, int16_t x, int16_t y, int16_t z);

/**
 * @brief Extract features from the current window and run inference.
 *        Updates vote_ring and vote_count.  Must only be called when
 *        add_sample() returns true.
 *
 * @param state Pointer to state struct
 * @param time_sin, time_cos Current time-of-day features
 * @return 1 if anomaly detected, 0 if normal, -1 on error
 */
int sliding_window_step(SlidingWindowState *state, float time_sin, float time_cos);

/**
 * @brief Get the current vote count (number of anomalies in ring buffer).
 * @param state Pointer to state struct
 * @return Number of anomalies (0 to VOTE_WINDOW_N)
 */
int sliding_window_get_vote_count(const SlidingWindowState *state);

/**
 * @brief Reset the state for a new wakeup cycle.
 * @param state Pointer to state struct
 */
void sliding_window_reset(SlidingWindowState *state);

#endif // SLIDING_WINDOW_H
