#ifndef CONFIG_H
#define CONFIG_H

#include "hal/gpio_types.h"
#include "hal/spi_types.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Sampling & FFT
//  SAMPLE_COUNT == FFT_SIZE: no zero-padding, no data discarded.
//  Resolution = SAMPLING_RATE_HZ / FFT_SIZE = 100 / 256 ≈ 0.39 Hz/bin
//  Nyquist     = SAMPLING_RATE_HZ / 2 = 50 Hz
// ─────────────────────────────────────────────────────────────────────────────


#define SAMPLE_COUNT 512
#define FFT_SIZE 512
#define SAMPLING_RATE_HZ 200.0f
#define SAMPLE_PERIOD_MS 5
#define NOVELTY_BUFFER_SIZE 200
#define MIN_CONSECUTIVE 1
#define WAKEUP_THRESHOLD_DEFAULT 14
#define NOVELTY_THRESHOLD 1.5f


/// ─────────────────────────────────────────────────────────────────────────────
///  Activity threshold for ADXL362 wakeup
///  THRESHOLD_MG:     default wake threshold in milli-g
///  THRESHOLD_MG_MIN: never go below this (too sensitive → false triggers)
///  THRESHOLD_MG_MAX: never go above this (too insensitive → misses knocks)
/// ─────────────────────────────────────────────────────────────────────────────
#define THRESHOLD_MG 150     /* 0.150g — good for table knocks            */
#define THRESHOLD_MG_MIN 150  /* floor: very sensitive                     */
#define THRESHOLD_MG_MAX 170 /* ceiling: very insensitive                 */

/// ─────────────────────────────────────────────────────────────────────────────
///  EMA rate-based adaptive threshold
///  LAMBDA_TARGET: target wakeup rate (interrupts/sec). 14/3600 ≈ 1% duty
///                 cycle at the 2.56-second sampling window.
///  ALPHA_EMA:     smoothing factor for the exponential moving average.
///                 Lower = slower reaction; 0.2 tracks ~5-event rolling mean.
/// ─────────────────────────────────────────────────────────────────────────────
#define LAMBDA_TARGET  (14.0f / 3600.0f) /* target rate: ~14 wakes/hr       */
#define ALPHA_EMA       0.2f             /* EMA smoothing factor             */

/// ─────────────────────────────────────────────────────────────────────────────
///  Deep-sleep wakeup sources
///  SYNC_INTERVAL_SEC: periodic timer wakeup for 24-h hub sync
/// ─────────────────────────────────────────────────────────────────────────────
#define SYNC_INTERVAL_SEC (24UL * 3600UL) /* 24 hours in seconds          */

/// ─────────────────────────────────────────────────────────────────────────────
///  ADXL362 activity/inactivity detector settings
/// ─────────────────────────────────────────────────────────────────────────────
#define ACTIVITY_TIME_MS 1      /* 1 sample @ 100 Hz = 10 ms            */
#define INACTIVITY_TIME_MS 5000 /* ms of no motion → ignore (unused)    */
#define MOTION_DIFF_MG 80.0f    /* mg change between samples = motion   */

/// ─────────────────────────────────────────────────────────────────────────────
///  Signal conditioning
/// ─────────────────────────────────────────────────────────────────────────────
#define SIGNAL_GAIN 1.0f /* gain applied to raw ADC data before features   */

// ─────────────────────────────────────────────────────────────────────────────
//  Training phase durations
//
//  The default training is split in two equal halves:
//    Phase 1 – EXPLORING: builds the novelty buffer and seeds k++ centroids.
//    Phase 2 – TRAINING:  refines centroids and computes distance thresholds.
//
//  TRAINING_DEFAULT_DURATION_MS is the *total* duration used when the hub
//  does not specify a custom duration (ml_duration_ms == 0).
//  EXPLORING_DURATION_MS / TRAINING_DURATION_MS each account for half of that.
//
//  These values are also used as defaults inside run_training_phase() when
//  the hub sends ml_duration_ms = 0.
//
//  Uncomment the set matching your use case:
// ─────────────────────────────────────────────────────────────────────────────

// Production: 24 h exploring + 24 h training (total 48 h)
// #define TRAINING_DEFAULT_DURATION_MS  (48UL * 3600UL * 1000UL)
// #define EXPLORING_DURATION_MS         (24UL * 3600UL * 1000UL)
// #define TRAINING_DURATION_MS          (24UL * 3600UL * 1000UL)

// Test: 30 min total (15 min each)
// #define TRAINING_DEFAULT_DURATION_MS  (30UL * 60UL * 1000UL)
// #define EXPLORING_DURATION_MS         (15UL * 60UL * 1000UL)
// #define TRAINING_DURATION_MS          (15UL * 60UL * 1000UL)

// Quick bench: 10 min total (5 min each)

#define MINUTES_MS(min) ((min) * 60UL * 1000UL)
#define TRAINING_DEFAULT_DURATION_MS MINUTES_MS(10)
#define EXPLORING_DURATION_MS MINUTES_MS(5)
#define TRAINING_DURATION_MS MINUTES_MS(5)
// ─────────────────────────────────────────────────────────────────────────────
//  Hardware Pin Definitions
//
//  Wiring for ESP32-C3 Super Mini to ADXL362:
//  ┌──────────────┬───────────────────┐
//  │ ADXL362      │ ESP32-C3 Super Mini │
//  ├──────────────┼───────────────────┤
//  │ VCC / VDD    │ 3.3V              │
//  │ GND          │ GND               │
//  │ MOSI / SDA   │ GPIO 7            │
//  │ MISO / SDO   │ GPIO 2            │
//  │ SCLK / SCL   │ GPIO 6            │
//  │ CS           │ GPIO 10           │
//  │ INT1         │ GPIO 3            │
//  └──────────────┴───────────────────┘
// ─────────────────────────────────────────────────────────────────────────────
#define MY_SPI_HOST SPI2_HOST
#define MY_PIN_MOSI GPIO_NUM_7
#define MY_PIN_MISO GPIO_NUM_2
#define MY_PIN_SCLK GPIO_NUM_6
#define MY_PIN_CS GPIO_NUM_10
#define MY_PIN_INT1 GPIO_NUM_3 /* must be a valid GPIO for deep sleep wakeup   \
                                */
#define MY_SPI_CLOCK 1000000   /* 1 MHz - reduced for debugging on breadboard */

#endif // CONFIG_H
