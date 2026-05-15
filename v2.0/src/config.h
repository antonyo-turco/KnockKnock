#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────────────────────────────────────
//  Sampling & FFT
//  SAMPLE_COUNT == FFT_SIZE: no zero-padding, no data discarded.
//  Resolution = SAMPLING_RATE_HZ / FFT_SIZE = 100 / 256 ≈ 0.39 Hz/bin
//  Nyquist     = SAMPLING_RATE_HZ / 2 = 50 Hz
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int   SAMPLE_COUNT     = 256;
static constexpr int   FFT_SIZE         = 256;
static constexpr float SAMPLING_RATE_HZ = 100.0f;
static constexpr int   SAMPLE_PERIOD_MS = 10;

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

static constexpr int   NOVELTY_BUFFER_SIZE = 200;
static constexpr float NOVELTY_THRESHOLD   = 1.5f;

#endif // CONFIG_H
