#ifndef CONFIG_H
#define CONFIG_H

#include "hal/gpio_types.h"
#include "hal/spi_types.h"

// Sampling & FFT
// SAMPLE_COUNT == FFT_SIZE: no zero-padding, no data discarded.
// Resolution = SAMPLING_RATE_HZ / FFT_SIZE = 200 / 512 = 0.39 Hz/bin
// Nyquist     = SAMPLING_RATE_HZ / 2 = 100 Hz

#define SAMPLE_COUNT 512
#define FFT_SIZE 512
#define SAMPLING_RATE_HZ 200.0f
#define SAMPLE_PERIOD_MS 5
#define NOVELTY_BUFFER_SIZE 200
#define NOVELTY_THRESHOLD 1.5f

// Debounce voting window
// With MIN_CONSECUTIVE=2 and 2.56s windows (512@200Hz), an alarm requires
// at least 5.12s of continuous anomaly. Set to 1 to disable voting.
#define MIN_CONSECUTIVE   2

// ADXL362 activity wakeup threshold (mg)
// WAKEUP_THRESHOLD_DEFAULT: pre-calibration default (ADC units; urban ~27, rural ~14)
// THRESHOLD_MG:     mg equivalent used at runtime
// THRESHOLD_MG_MIN: floor — too sensitive below this causes false triggers
// THRESHOLD_MG_MAX: ceiling — above this misses knocks
#define WAKEUP_THRESHOLD_DEFAULT   14u
#define THRESHOLD_MG 50
#define THRESHOLD_MG_MIN 100
#define THRESHOLD_MG_MAX 140

// EMA rate-based adaptive threshold
// LAMBDA_TARGET: target wakeup rate (interrupts/sec). ~14/hr = 1% duty cycle
//                at a 2.56-second sampling window.
// ALPHA_EMA:     smoothing factor. 0.2 tracks a ~5-event rolling mean.
#define LAMBDA_TARGET  (14.0f / 3600.0f)
#define ALPHA_EMA       0.2f

// Deep-sleep timer wakeup for 24-h hub sync
#define SYNC_INTERVAL_SEC (24UL * 3600UL)

// ADXL362 activity detector timing
// ACTIVITY_TIME_MS: minimum activity duration before interrupt fires (1 sample @ 200 Hz = 5 ms)
#define ACTIVITY_TIME_MS 1
#define INACTIVITY_TIME_MS 5000
#define MOTION_DIFF_MG 80.0f

#define SIGNAL_GAIN 1.0f

// Training phase durations
//
// Total = EXPLORING + TRAINING. Hub can override with ml_duration_ms > 0.
// Uncomment the set that matches the deployment scenario:

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

// Hardware pin definitions — ESP32-C3 Super Mini to ADXL362
//
//  ADXL362      ESP32-C3 Super Mini
//  VCC / VDD    3.3V
//  GND          GND
//  MOSI / SDA   GPIO 7
//  MISO / SDO   GPIO 2
//  SCLK / SCL   GPIO 6
//  CS           GPIO 10
//  INT1         GPIO 3

#define MY_SPI_HOST SPI2_HOST
#define MY_PIN_MOSI GPIO_NUM_7
#define MY_PIN_MISO GPIO_NUM_2
#define MY_PIN_SCLK GPIO_NUM_6
#define MY_PIN_CS GPIO_NUM_10
#define MY_PIN_INT1 GPIO_NUM_3  /* must be a valid GPIO for deep sleep wakeup */
#define MY_SPI_CLOCK 1000000    /* 1 MHz — reduced for breadboard reliability */

#ifdef CONFIG_IDF_TARGET_ESP32C3
#define MY_PIN_LED GPIO_NUM_8   /* active-LOW internal LED on ESP32-C3 Super Mini */
#endif

#endif // CONFIG_H
