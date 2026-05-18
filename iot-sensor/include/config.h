#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────────────────────────────────────
//  Sampling & FFT
//  SAMPLE_COUNT == FFT_SIZE: no zero-padding, no data discarded.
//  Resolution = SAMPLING_RATE_HZ / FFT_SIZE = 100 / 256 ≈ 0.39 Hz/bin
//  Nyquist     = SAMPLING_RATE_HZ / 2 = 50 Hz
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __cplusplus


// ─────────────────────────────────────────────────────────────────────────────
//  Novelty buffer  (used during EXPLORING phase)
//
//  Stores normalised feature vectors that are "novel enough" relative to
//  each other.  k++ seeding runs on this buffer at end of EXPLORING.
//
//  RAM cost: NOVELTY_BUFFER_SIZE × FEATURE_DIM × 4 B
//  With FEATURE_DIM=45: 200 × 45 × 4 = 36 kB
//
//  NOVELTY_THRESHOLD: min Euclidean distance (normalised space) required
//  for a sample to be considered novel.  Tune if buffer fills too fast
//  (raise) or too slow (lower).
// ─────────────────────────────────────────────────────────────────────────────


static constexpr int   SAMPLE_COUNT     = 256;
static constexpr int   FFT_SIZE         = 256;
static constexpr float SAMPLING_RATE_HZ = 100.0f;
static constexpr int   SAMPLE_PERIOD_MS = 10;
static constexpr int   NOVELTY_BUFFER_SIZE = 200;
static constexpr float NOVELTY_THRESHOLD   = 1.5f;
#else
#define SAMPLE_COUNT     256
#define FFT_SIZE         256
#define SAMPLING_RATE_HZ 100.0f
#define SAMPLE_PERIOD_MS 10
#define NOVELTY_BUFFER_SIZE 200
#define NOVELTY_THRESHOLD   1.5f
#endif

/// ─────────────────────────────────────────────────────────────────────────────
///  Thresholds and timings - adjust these for your use case
/// ─────────────────────────────────────────────────────────────────────────────
#define THRESHOLD_MG          150   /* 0.150g change from baseline - good for table knocks */
#define ACTIVITY_TIME_MS        1   /* 1 sample @ 100Hz = 10ms - catches brief impulses */
#define INACTIVITY_TIME_MS   5000   /* ms of no motion before going back to sleep */
#define MOTION_DIFF_MG        80.0f /* mg change between samples to count as motion */

#define SIGNAL_GAIN  1.0f  // gain factor to apply to raw accelerometer data before feature extraction

// ─────────────────────────────────────────────────────────────────────────────
//  Phase durations — comment/uncomment the pair you want
// ─────────────────────────────────────────────────────────────────────────────

// Production: 24h exploring + 24h training
// #define EXPLORING_DURATION_MS  (24UL * 3600UL * 1000UL)
// #define TRAINING_DURATION_MS   (24UL * 3600UL * 1000UL)

// Test: 15 min exploring + 15 min training
// #define EXPLORING_DURATION_MS  (15UL * 60UL * 1000UL)
// #define TRAINING_DURATION_MS   (15UL * 60UL * 1000UL)

// Quick bench: 2 min exploring + 2 min training
#define EXPLORING_DURATION_MS    ( 2UL * 60UL * 1000UL)
#define TRAINING_DURATION_MS     ( 2UL * 60UL * 1000UL)

#endif // CONFIG_H
