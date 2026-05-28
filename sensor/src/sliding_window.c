#include "sliding_window.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SLIDING_WINDOW";

void sliding_window_init(SlidingWindowState *state, const KMeansModel *model) {
    if (!state || !model) return;
    
    memset(state, 0, sizeof(SlidingWindowState));
    state->model = model;
    state->write_idx = 0;
    state->new_sample_count = 0;
    state->vote_idx = 0;
    state->vote_count = 0;
    state->ready = false;
    
    ESP_LOGI(TAG, "Initialized. Buffer size: %d samples/axis, voting window: %d",
             CIRCULAR_BUFFER_SIZE, VOTE_WINDOW_N);
}

bool sliding_window_add_sample(SlidingWindowState *state, int16_t x, int16_t y, int16_t z) {
    if (!state) return false;

    // Always write to the circular buffer so no raw data is lost while the
    // ML task is still processing the previous window.
    int idx = state->write_idx % CIRCULAR_BUFFER_SIZE;
    state->buffer_x[idx] = x;
    state->buffer_y[idx] = y;
    state->buffer_z[idx] = z;
    state->write_idx++;

    // Once the window is signalled (ready=true), stop counting new samples
    // until sliding_window_step() resets the state.  Without this guard,
    // every sample past the 50th would return true and spam xSemaphoreGive.
    if (state->ready) {
        return false;
    }

    state->new_sample_count++;
    if (state->new_sample_count >= STEP_SAMPLES) {
        state->ready = true;
        return true;
    }
    return false;
}

int sliding_window_step(SlidingWindowState *state, float time_sin, float time_cos) {
    if (!state || !state->ready || !state->model) {
        return -1;  // Error: not ready or invalid state
    }
    
    // Extract the last WINDOW_SAMPLES from the circular buffer
    static int16_t window_x[WINDOW_SAMPLES];
    static int16_t window_y[WINDOW_SAMPLES];
    static int16_t window_z[WINDOW_SAMPLES];
    
    // Calculate the start index of the window
    // The window ends at (write_idx - 1), starts at (write_idx - WINDOW_SAMPLES)
    int start_idx = (state->write_idx - WINDOW_SAMPLES + CIRCULAR_BUFFER_SIZE) % CIRCULAR_BUFFER_SIZE;
    
    for (int i = 0; i < WINDOW_SAMPLES; i++) {
        int src_idx = (start_idx + i) % CIRCULAR_BUFFER_SIZE;
        window_x[i] = state->buffer_x[src_idx];
        window_y[i] = state->buffer_y[src_idx];
        window_z[i] = state->buffer_z[src_idx];
    }
    
    // Compute features
    InferenceFeatures feat = compute_features(
        window_x, window_y, window_z,
        WINDOW_SAMPLES,
        SAMPLING_RATE_HZ,
        time_sin, time_cos
    );
    
    // Run inference
    float distance = 0.0f;
    int cluster = 0;
    bool is_baseline = training_is_baseline(state->model, &feat, &distance, &cluster);
    
    // Update vote ring
    uint8_t vote = is_baseline ? 0 : 1;
    
    // Remove old vote from count
    state->vote_count -= state->vote_ring[state->vote_idx];
    
    // Add new vote
    state->vote_ring[state->vote_idx] = vote;
    state->vote_count += vote;
    
    // Advance ring index
    state->vote_idx = (state->vote_idx + 1) % VOTE_WINDOW_N;
    
    // Store last features for debugging
    state->last_features = feat;
    state->last_distance = distance;
    state->last_cluster = cluster;
    
    // Reset new sample count
    state->new_sample_count = 0;
    state->ready = false;
    
    ESP_LOGD(TAG, "[VOTE] is_baseline=%d, dist=%.3f, cluster=%d, vote_count=%d/%d",
             is_baseline, distance, cluster, state->vote_count, VOTE_WINDOW_N);
    
    return vote;
}

int sliding_window_get_vote_count(const SlidingWindowState *state) {
    if (!state) return 0;
    return state->vote_count;
}

void sliding_window_reset(SlidingWindowState *state) {
    if (!state) return;
    
    memset(state->buffer_x, 0, sizeof(state->buffer_x));
    memset(state->buffer_y, 0, sizeof(state->buffer_y));
    memset(state->buffer_z, 0, sizeof(state->buffer_z));
    memset(state->vote_ring, 0, sizeof(state->vote_ring));
    
    state->write_idx = 0;
    state->new_sample_count = 0;
    state->vote_idx = 0;
    state->vote_count = 0;
    state->ready = false;
    
    ESP_LOGI(TAG, "Reset complete");
}