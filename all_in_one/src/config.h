#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────────────────────────────────────
//  Shared project-wide constants
//  Include this header in every .cpp that needs sampling or FFT parameters.
// ─────────────────────────────────────────────────────────────────────────────

// Number of accelerometer samples per inference window.
// Must equal FFT_SIZE so the FFT consumes exactly one window with no
// zero-padding and no data discarded.
static constexpr int   SAMPLE_COUNT      = 256;

// FFT length — kept identical to SAMPLE_COUNT intentionally.
// Resolution = SAMPLING_RATE_HZ / FFT_SIZE = 100 / 256 ≈ 0.39 Hz/bin
// Nyquist     = SAMPLING_RATE_HZ / 2       = 50 Hz
static constexpr int   FFT_SIZE          = 256;

// Accelerometer output data rate and derived timing
static constexpr float SAMPLING_RATE_HZ  = 100.0f;
static constexpr int   SAMPLE_PERIOD_MS  = 10;      // 1000 / SAMPLING_RATE_HZ

// ─────────────────────────────────────────────────────────────────────────────
//  K-means++ seeding buffer size
//
//  Each window lasts SAMPLE_COUNT / SAMPLING_RATE_HZ = 2.56 s.
//  The seeding phase collects SEED_BUFFER_SIZE windows before placing
//  centroids — it must complete well within the chosen training duration.
//
//  Keep this in sync with TRAINING_DURATION_MS in main.cpp:
//    2 min  bench   → ~46  total windows → use 12
//    15 min test    → ~351 total windows → use 50
//    24 h   prod    → ~33750 windows     → use 100
//
//  Rule of thumb: SEED_BUFFER_SIZE ≈ 25% of total training windows.
// ─────────────────────────────────────────────────────────────────────────────

// static constexpr int   SEED_BUFFER_SIZE  = 12;    //  2 min bench
// static constexpr int   SEED_BUFFER_SIZE  = 50;   // 15 min test
static constexpr int   SEED_BUFFER_SIZE  = 100;  // 24 h production

#endif // CONFIG_H
