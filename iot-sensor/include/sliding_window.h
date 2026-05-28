#ifndef SLIDING_WINDOW_H
#define SLIDING_WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include "feature_extraction.h"
#include "tinyml_training.h"

// Rolling window anomaly detector with majority voting.
// Window: 256 samples @ 200 Hz = 1.28 s, step 50 samples = 0.25 s.
// Alarm triggered when >= VOTE_THRESHOLD anomalies appear in the
// last VOTE_WINDOW_N decisions (5-second lookback).

#define WINDOW_SAMPLES   256
#define STEP_SAMPLES     50
#define VOTE_WINDOW_N    20
#define VOTE_THRESHOLD   3

// Circular buffer fits the full window plus all step-sized overhangs.
#define CIRCULAR_BUFFER_SIZE (WINDOW_SAMPLES + STEP_SAMPLES * (VOTE_WINDOW_N - 1))

typedef struct {
    int16_t buffer_x[CIRCULAR_BUFFER_SIZE];
    int16_t buffer_y[CIRCULAR_BUFFER_SIZE];
    int16_t buffer_z[CIRCULAR_BUFFER_SIZE];

    int write_idx;
    int new_sample_count;

    uint8_t vote_ring[VOTE_WINDOW_N];
    int vote_idx;
    int vote_count;

    InferenceFeatures last_features;
    float last_distance;
    int last_cluster;

    const KMeansModel *model;
    bool ready;
} SlidingWindowState;

void sliding_window_init(SlidingWindowState *state, const KMeansModel *model);

// Returns true when STEP_SAMPLES new samples have been collected.
bool sliding_window_add_sample(SlidingWindowState *state, int16_t x, int16_t y, int16_t z);

// Extract features and run inference. Updates vote ring.
// Returns 1 = anomaly, 0 = normal, -1 = error.
int sliding_window_step(SlidingWindowState *state, float time_sin, float time_cos);

int  sliding_window_get_vote_count(const SlidingWindowState *state);
void sliding_window_reset(SlidingWindowState *state);

#endif // SLIDING_WINDOW_H
