/**
 * @file main.c
 * @brief KnockKnock IoT Sensor — Main Entry Point
 *
 * Boot flow:
 *  ┌──────────────────────────────────────────────────────────────┐
 *  │  FIRST BOOT / RESET                                          │
 *  │   1. hub_comm_init()                                         │
 *  │   2. If not paired: hub_comm_pair() → save MAC in NVS        │
 *  │   3. hub_comm_get_information() → sync clock                 │
 *  │   4. If !trained (or hub requests): run_training_phase()     │
 *  │   5. training_save() → NVS                                   │
 *  │   6. goto_deep_sleep()                                       │
 *  ├──────────────────────────────────────────────────────────────┤
 *  │  SENSOR WAKEUP (GPIO – ADXL362 activity interrupt)          │
 *  │   1. Start sensor_sampler_task  (core 0, high priority)      │
 *  │   2. Start ml_processor_task   (core 1, waits on semaphore)  │
 *  │   3. ML inference:                                           │
 *  │       BASELINE  → adjust activity threshold, deep sleep      │
 *  │       DEVIATION → send alarm, sync clock, deep sleep         │
 *  ├──────────────────────────────────────────────────────────────┤
 *  │  TIMER WAKEUP (24-h hub sync)                                │
 *  │   1. hub_comm_init() → hub_comm_get_information()            │
 *  │   2. Handle reset / re-training requests from hub            │
 *  │   3. goto_deep_sleep()                                       │
 *  └──────────────────────────────────────────────────────────────┘
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ADXL362_utils.h"
#include "config.h"
#include "feature_extraction.h"
#include "hub_communication.h"
#include "secure_store.h"
#include "fft_processor.h"

#include "tinyml_training.h"

static const char *TAG = "MAIN";

// ─────────────────────────────────────────────────────────────────────────────
//  RTC memory — persists across deep sleep, reset to 0 on power-on
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    float    lambda_ema;     /* smoothed wakeup rate (interrupts/sec)      */
    int64_t  last_wake_sec;  /* epoch seconds of last sensor wakeup        */
    uint16_t thresh_act;     /* current ADXL362 activity threshold (mg)    */
    uint8_t  valid;          /* 0 on first boot, 1 after first wake cycle  */
} rtc_state_t;

RTC_DATA_ATTR static rtc_state_t rtc_state = {
    .lambda_ema    = LAMBDA_TARGET,
    .last_wake_sec = 0,
    .thresh_act    = THRESHOLD_MG,
    .valid         = 0,
};

/** Debounce counter — incremented on each anomaly, reset after alarm. */
RTC_DATA_ATTR static uint8_t rtc_alarm_counter = 0;

/** True once hub MAC has been saved to NVS and pairing is confirmed. */
RTC_DATA_ATTR static bool rtc_is_provisioned = false;

/** True once a model has been trained and saved to NVS. */
RTC_DATA_ATTR static bool rtc_is_trained = false;



// ─────────────────────────────────────────────────────────────────────────────
//  Task inter-communication
// ─────────────────────────────────────────────────────────────────────────────

/** Shared sample buffers — written by sampler task, read by ML task. */
static int16_t g_xb[SAMPLE_COUNT];
static int16_t g_yb[SAMPLE_COUNT];
static int16_t g_zb[SAMPLE_COUNT];

/** Binary semaphore: given when g_xb/g_yb/g_zb are ready. */
static SemaphoreHandle_t s_data_ready_sem = NULL;

/** Global sensor handle initialised in app_main. */
static adxl362_handle_t g_sensor = NULL;

static EventGroupHandle_t s_main_event_group = NULL;
#define EVENT_TRAINING_DONE (1 << 0)

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations for LED helpers (defined after goto_deep_sleep)
// ─────────────────────────────────────────────────────────────────────────────

static void led_sos_stop(void);
static void led_training_stop(void);
static inline void led_off(void);

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Update the EMA rate estimator and scale the activity threshold.
 *
 * @param now_sec   Current epoch time in seconds (from gettimeofday).
 * @param update_ts If true, advance last_wake_sec to now_sec (sensor wakes
 *                  only — timer wakes must NOT consume the quiet-period dt).
 */
static void ema_adapt_threshold(int64_t now_sec, bool update_ts) {
    if (rtc_state.valid && rtc_state.last_wake_sec > 0) {
        int64_t dt = now_sec - rtc_state.last_wake_sec;
        if (dt > 0) {
            float lambda_meas = 1.0f / (float)dt;
            rtc_state.lambda_ema = ALPHA_EMA * lambda_meas
                                 + (1.0f - ALPHA_EMA) * rtc_state.lambda_ema;
            float scale = rtc_state.lambda_ema / LAMBDA_TARGET;
            uint16_t t = (uint16_t)((float)rtc_state.thresh_act * scale);
            if (t < THRESHOLD_MG_MIN) t = THRESHOLD_MG_MIN;
            if (t > THRESHOLD_MG_MAX) t = THRESHOLD_MG_MAX;
            rtc_state.thresh_act = t;
            ESP_LOGI(TAG, "[THRESHOLD] λ_ema=%.5f  scale=%.3f  →  %u mg",
                     rtc_state.lambda_ema, scale, rtc_state.thresh_act);
        }
    } else {
        rtc_state.valid = 1;
        ESP_LOGI(TAG, "[THRESHOLD] First wakeup — init threshold %u mg",
                 rtc_state.thresh_act);
    }
    if (update_ts) {
        rtc_state.last_wake_sec = now_sec;
    }
}

/**
 * @brief Erase hub pairing data from NVS and flag the device as unprovisioned.
 *        Called when the hub requests a full reset.
 */
static void handle_hub_reset(void) {
  ESP_LOGI(TAG, "Hub requested device reset — erasing pairing data.");
  secure_store_erase("hub_mac");
  training_erase_nvs();
  rtc_is_provisioned = false;
  rtc_is_trained = false;
  rtc_alarm_counter = 0;
  rtc_state.thresh_act    = THRESHOLD_MG;
  rtc_state.lambda_ema    = LAMBDA_TARGET;
  rtc_state.last_wake_sec = 0;
  rtc_state.valid         = 0;
  esp_restart();
}

/**
 * @brief Configure the ADXL362 with the current adaptive threshold and enter
 *        deep sleep.  Both GPIO (sensor interrupt) and timer (24-h sync) are
 *        armed as wakeup sources.
 */
static void goto_deep_sleep(void) {
  ESP_LOGI(TAG, "Configuring deep sleep. ADXL362 threshold = %u mg",
           rtc_state.thresh_act);

  if (g_sensor) {
    adxl362_set_activity_threshold(g_sensor, rtc_state.thresh_act, ACTIVITY_TIME_MS,
                                   true);
    // Stop then restart so the new threshold takes effect
    adxl362_stop_measurement(g_sensor);
    adxl362_start_measurement(g_sensor);

    // Wait for the sensor to settle and take a few samples
    vTaskDelay(pdMS_TO_TICKS(50));

    // Clear any pending activity interrupt so we do not wake up immediately
    uint8_t dummy_status = 0;
    adxl362_get_status(g_sensor, &dummy_status);
  }

  // Ensure Wi-Fi is stopped before deep sleep to prevent crashes/high power draw
  esp_wifi_stop();

  // GPIO: ADXL362 INT1 line goes high on activity
  esp_deep_sleep_enable_gpio_wakeup(1ULL << MY_PIN_INT1,
                                    ESP_GPIO_WAKEUP_GPIO_HIGH);

  // Timer: periodic 24-h hub sync
  esp_sleep_enable_timer_wakeup((uint64_t)SYNC_INTERVAL_SEC * 1000000ULL);

  led_sos_stop();
  led_training_stop();
  led_off();

  esp_deep_sleep_start();
  // Never returns
}

// ─────────────────────────────────────────────────────────────────────────────
//  LED helpers  (GPIO 8, active-LOW on ESP32-C3 Super Mini)
//  Stubs are provided for other targets so callers need no #ifdefs.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef CONFIG_IDF_TARGET_ESP32C3

static esp_timer_handle_t s_train_blink_timer = NULL;
static TaskHandle_t       s_sos_task_handle   = NULL;

static inline void led_on(void)  { gpio_set_level(MY_PIN_LED, 0); }
static inline void led_off(void) { gpio_set_level(MY_PIN_LED, 1); }

/* periodic timer callback: toggles LED every 1 s during training */
static void train_blink_cb(void *arg) {
    static bool s = true;   /* starts ON; first tick flips to OFF */
    s = !s;
    gpio_set_level(MY_PIN_LED, s ? 0 : 1);
}

static void led_training_start(void) {
    if (s_train_blink_timer) return;
    const esp_timer_create_args_t args = { .callback = train_blink_cb,
                                           .name = "led_train" };
    esp_timer_create(&args, &s_train_blink_timer);
    led_on();
    esp_timer_start_periodic(s_train_blink_timer, 1000000ULL); /* 1 s */
}

static void led_training_stop(void) {
    if (!s_train_blink_timer) return;
    esp_timer_stop(s_train_blink_timer);
    esp_timer_delete(s_train_blink_timer);
    s_train_blink_timer = NULL;
    led_off();
}

/* SOS: ··· --- ··· + pause, loops until deleted */
#define SOS_DOT_ON_MS    200
#define SOS_DOT_OFF_MS   150
#define SOS_DASH_ON_MS   600
#define SOS_DASH_OFF_MS  150
#define SOS_LETTER_MS    300
#define SOS_PAUSE_MS    1500

static void sos_task(void *arg) {
    while (1) {
        for (int i = 0; i < 3; i++) {          /* S */
            led_on();  vTaskDelay(pdMS_TO_TICKS(SOS_DOT_ON_MS));
            led_off(); vTaskDelay(pdMS_TO_TICKS(SOS_DOT_OFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(SOS_LETTER_MS));
        for (int i = 0; i < 3; i++) {          /* O */
            led_on();  vTaskDelay(pdMS_TO_TICKS(SOS_DASH_ON_MS));
            led_off(); vTaskDelay(pdMS_TO_TICKS(SOS_DASH_OFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(SOS_LETTER_MS));
        for (int i = 0; i < 3; i++) {          /* S */
            led_on();  vTaskDelay(pdMS_TO_TICKS(SOS_DOT_ON_MS));
            led_off(); vTaskDelay(pdMS_TO_TICKS(SOS_DOT_OFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(SOS_PAUSE_MS));
    }
}

static void led_sos_start(void) {
    if (s_sos_task_handle) return;
    xTaskCreate(sos_task, "led_sos", 1024, NULL, 3, &s_sos_task_handle);
}

static void led_sos_stop(void) {
    if (!s_sos_task_handle) return;
    vTaskDelete(s_sos_task_handle);
    s_sos_task_handle = NULL;
    led_off();
}

#else  /* ── stubs for non-C3 targets ─────────────────────────────────────── */

static inline void led_off(void)            {}
static inline void led_training_start(void) {}
static inline void led_training_stop(void)  {}
static inline void led_sos_start(void)      {}
static inline void led_sos_stop(void)       {}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */

// ─────────────────────────────────────────────────────────────────────────────
//  Training phase
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Collect windows of accelerometer data and run them through the
 *        EXPLORING → TRAINING pipeline.
 *
 * @param exploring_ms  Duration of the EXPLORING phase in milliseconds.
 * @param training_ms   Duration of the TRAINING phase in milliseconds.
 *
 * On success the finalised model is saved to NVS and rtc_is_trained is set.
 */
static void run_training_phase(uint32_t exploring_ms, uint32_t training_ms) {
  ESP_LOGI(TAG, "=== TRAINING START: exploring=%lu s  training=%lu s ===",
           exploring_ms / 1000UL, training_ms / 1000UL);

  if (!g_sensor) {
    ESP_LOGE(TAG, "Sensor not initialised — aborting training.");
    return;
  }

  led_sos_stop();       /* cancel SOS if training follows an alarm */
  led_training_start(); /* 1 s on / 1 s off for the whole training phase */

  KMeansModel model;
  training_init(&model);

  // ── Phase 1: EXPLORING ────────────────────────────────────────────────────
  ESP_LOGI(TAG, "--- EXPLORING ---");
  int64_t deadline_us = esp_timer_get_time() + (int64_t)exploring_ms * 1000LL;
  uint32_t win_seq = 0;

  while (esp_timer_get_time() < deadline_us) {
    // Sample one window at the configured ODR
    TickType_t xLastWake = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(1000 / (int)SAMPLING_RATE_HZ);
    for (int i = 0; i < SAMPLE_COUNT; i++) {
      vTaskDelayUntil(&xLastWake, xPeriod);
      adxl362_raw_data_t raw;
      if (adxl362_read_raw(g_sensor, &raw) == ESP_OK) {
        g_xb[i] = raw.x;
        g_yb[i] = raw.y;
        g_zb[i] = raw.z;
      } else {
        g_xb[i] = 0;
        g_yb[i] = 0;
        g_zb[i] = 0;
      }
    }

    float time_sin, time_cos;
    get_time_features(&time_sin, &time_cos);
    InferenceFeatures feat = compute_features(
        g_xb, g_yb, g_zb, SAMPLE_COUNT, SAMPLING_RATE_HZ, time_sin, time_cos);

    exploring_update(&model, &feat);
    win_seq++;

    if (win_seq % 150 == 0) {
      uint32_t elapsed_ms =
          (uint32_t)((esp_timer_get_time() -
                      (deadline_us - (int64_t)exploring_ms * 1000LL)) /
                     1000LL);
      ESP_LOGI(TAG, "[EXPLORE] win=%lu  n=%lu  novel=%u%%  elapsed=%lu s",
               (unsigned long)win_seq, (unsigned long)model.total_samples,
               exploring_novelty_pct(), (unsigned long)(elapsed_ms / 1000UL));
    }
  }

  bool ok = exploring_finalize(&model);
  if (!ok) {
    ESP_LOGW(
        TAG,
        "[EXPLORE] WARNING: novelty buffer sparse — model may be imprecise.");
  }
  ESP_LOGI(TAG, "EXPLORING done after %lu windows.", (unsigned long)win_seq);

  // ── Phase 2: TRAINING ─────────────────────────────────────────────────────
  ESP_LOGI(TAG, "--- TRAINING ---");
  deadline_us = esp_timer_get_time() + (int64_t)training_ms * 1000LL;
  win_seq = 0;

  while (esp_timer_get_time() < deadline_us) {
    TickType_t xLastWake = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(1000 / (int)SAMPLING_RATE_HZ);
    for (int i = 0; i < SAMPLE_COUNT; i++) {
      vTaskDelayUntil(&xLastWake, xPeriod);
      adxl362_raw_data_t raw;
      if (adxl362_read_raw(g_sensor, &raw) == ESP_OK) {
        g_xb[i] = raw.x;
        g_yb[i] = raw.y;
        g_zb[i] = raw.z;
      } else {
        g_xb[i] = 0;
        g_yb[i] = 0;
        g_zb[i] = 0;
      }
    }

    float time_sin, time_cos;
    get_time_features(&time_sin, &time_cos);
    InferenceFeatures feat = compute_features(
        g_xb, g_yb, g_zb, SAMPLE_COUNT, SAMPLING_RATE_HZ, time_sin, time_cos);

    training_update(&model, &feat);
    win_seq++;

    if (win_seq % 150 == 0) {
      uint32_t elapsed_ms =
          (uint32_t)((esp_timer_get_time() -
                      (deadline_us - (int64_t)training_ms * 1000LL)) /
                     1000LL);
      ESP_LOGI(TAG, "[TRAIN] win=%lu  n=%lu  %lu%%", (unsigned long)win_seq,
               (unsigned long)model.total_samples,
               (unsigned long)(elapsed_ms * 100UL / training_ms));
    }
  }

  training_finalize(&model);
  ESP_LOGI(TAG, "TRAINING done after %lu windows.", (unsigned long)win_seq);

  // ── Save to NVS ───────────────────────────────────────────────────────────
  if (training_save(&model)) {
    rtc_is_trained = true;
    ESP_LOGI(TAG, "Model saved to NVS. rtc_is_trained = true.");
  } else {
    ESP_LOGE(TAG, "Failed to save model to NVS!");
  }

  led_training_stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Training task wrapper (for concurrent execution)
// ─────────────────────────────────────────────────────────────────────────────

static void training_task_wrapper(void *arg) {
  run_training_phase(EXPLORING_DURATION_MS, TRAINING_DURATION_MS);
  if (s_main_event_group) {
    xEventGroupSetBits(s_main_event_group, EVENT_TRAINING_DONE);
  }
  vTaskDelete(NULL);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor sampler task
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Fills g_xb / g_yb / g_zb with SAMPLE_COUNT samples at
 * SAMPLING_RATE_HZ, then signals the ML task via semaphore.
 */
static void sensor_sampler_task(void *arg) {
  ESP_LOGI(TAG, "[SAMPLER] Task started.");

  TickType_t xLastWake = xTaskGetTickCount();
  const TickType_t xPeriod = pdMS_TO_TICKS(1000 / (int)SAMPLING_RATE_HZ);

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    vTaskDelayUntil(&xLastWake, xPeriod);
    adxl362_raw_data_t raw;
    if (adxl362_read_raw(g_sensor, &raw) == ESP_OK) {
      g_xb[i] = raw.x;
      g_yb[i] = raw.y;
      g_zb[i] = raw.z;
    } else {
      g_xb[i] = 0;
      g_yb[i] = 0;
      g_zb[i] = 0;
    }
  }

  ESP_LOGI(TAG, "[SAMPLER] Window ready — releasing ML task.");
  xSemaphoreGive(s_data_ready_sem);
  vTaskDelete(NULL);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ML processor task
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Waits for the sampler to fill the buffer, computes features, runs
 *        inference, and either adjusts the threshold (baseline) or sends an
 *        alarm (deviation) before entering deep sleep.
 */
static void ml_processor_task(void *arg) {
  // Block until the window is complete
  xSemaphoreTake(s_data_ready_sem, portMAX_DELAY);
  ESP_LOGI(TAG, "[ML] Running inference...");

  // ── Clock validity check ───────────────────────────────────────────────
  // If the RTC was never synced (e.g. power loss wiped RTC memory but NVS
  // survived), request time from the hub before running ML inference.
  // A Unix timestamp < 1 000 000 000 means the clock is at its epoch default
  // (i.e., it has never been set — any date before year 2001 is invalid).
  struct timeval tv_check;
  gettimeofday(&tv_check, NULL);
  if (tv_check.tv_sec < 1000000000L) {
    ESP_LOGW(TAG, "RTC not synced (ts=%lld) — requesting time from hub.",
             (long long)tv_check.tv_sec);
    if (hub_comm_init() == ESP_OK) {
      hub_info_t sync_info;
      if (!hub_comm_get_information(&sync_info, 5000, 3)) {
        ESP_LOGW(TAG, "Hub unreachable for clock sync — proceeding without "
                      "valid time.");
      }
      // hub_comm_get_information() calls settimeofday() internally on success
    }
  }

  // ── Compute time features ─────────────────────────────────────────────────
  float time_sin, time_cos;
  get_time_features(&time_sin, &time_cos);

  // ── Feature extraction ────────────────────────────────────────────────────
  InferenceFeatures feat = compute_features(
      g_xb, g_yb, g_zb, SAMPLE_COUNT, SAMPLING_RATE_HZ, time_sin, time_cos);

  // ── Load model ────────────────────────────────────────────────────────────
  KMeansModel model;
  if (!training_load(&model)) {
    ESP_LOGE(TAG, "[ML] No valid model in NVS — going back to sleep.");
    goto_deep_sleep();
    // Never returns
  }

  // ── Inference ─────────────────────────────────────────────────────────────
  float dist = 0.0f;
  int cluster = -1;
  bool is_baseline = training_is_baseline(&model, &feat, &dist, &cluster);

  ESP_LOGI(TAG, "[ML] impact=%.4f  m_p99=%.2f  dist=%.4f  C%d  %s",
           feat.impact_score, feat.m_p99, dist, cluster,
           is_baseline ? "BASELINE" : "*** DEVIATION ***");

  // ── EMA rate-based threshold adaptation ──────────────────────────────────
  struct timeval tv_now;
  gettimeofday(&tv_now, NULL);
  ema_adapt_threshold(tv_now.tv_sec, true);

  




  if (!is_baseline) {
    rtc_alarm_counter++;
    ESP_LOGW(TAG, "[ML] DEVIATION detected (consecutive: %u/%d).", 
             rtc_alarm_counter, MIN_CONSECUTIVE);

    if (rtc_alarm_counter >= MIN_CONSECUTIVE) {
      // ── ANOMALY: send alarm and resync ────────────────────────────────────
      ESP_LOGW(TAG, "[ML] Alarm threshold reached — sending alarm to hub.");

      led_sos_start();

      ESP_ERROR_CHECK(hub_comm_init());

      hub_comm_send_alarm(1 /* alarm_code */, 100 /* max_retries */);

      rtc_alarm_counter = 0; // Reset after alarm

      hub_info_t info;
      if (hub_comm_get_information(&info, 5000, 3)) {
        // Clock already synced inside hub_comm_get_information()
        if (info.do_reset) {
          handle_hub_reset(); // Never returns
        }
        if (info.do_ml_training) {
          // Hub requested re-training after alarm
          uint32_t exp_ms = (info.ml_duration_ms > 0) ? info.ml_duration_ms / 2
                                                      : EXPLORING_DURATION_MS;
          uint32_t trn_ms = (info.ml_duration_ms > 0) ? info.ml_duration_ms / 2
                                                      : TRAINING_DURATION_MS;
          run_training_phase(exp_ms, trn_ms);
        }
      } else {
        ESP_LOGW(
            TAG,
            "[ML] Hub unreachable after alarm. Continuing with existing model.");
      }
    }
  } else {
    rtc_alarm_counter = 0; // Reset on baseline
    ESP_LOGI(TAG, "[ML] Baseline activity — no alarm sent.");
  }

  goto_deep_sleep();
  // Never returns
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────

void app_main(void) {
#if CONFIG_IDF_TARGET_ESP32C3
  // ── Turn ON internal LED (GPIO8 active low on Super Mini) ─────────────────
  gpio_reset_pin(GPIO_NUM_8);
  gpio_set_direction(GPIO_NUM_8, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_8, 0); 
#endif

  // ── NVS flash init (mandatory before any NVS/wifi/esp-now call) ───────────
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  // ── Determine wakeup cause ────────────────────────────────────────────────
  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

  // ── Sensor init (needed in all paths) ─────────────────────────────────────
  g_sensor = sensor_init(SAMPLING_RATE_HZ);
  if (!g_sensor) {
    ESP_LOGE(TAG, "ADXL362 initialisation failed. Retrying in 5 s...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
  }

  // ── Init FFT workspace ────────────────────────────────────────────────────
  if (fft_processor_init(FFT_SIZE) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize FFT processor.");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
  }

  // ── Restore RTC flags from NVS after a power cut ──────────────────────────
  // RTC_DATA_ATTR memory survives deep sleep but is zeroed on power-on reset.
  // Restore the flags with lightweight NVS probes (blob-size checks only).
  // The full model is loaded later, inside ml_processor_task, only when
  // inference is actually needed.  PMK/LMK/MAC are loaded lazily by
  // hub_comm_init(), which is called only when the hub must be contacted.
  {
    nvs_handle_t _nvs;

    // ── rtc_is_trained: check that the "kmeans" blob exists and is the right
    //    size — training_save() only writes a finalised model, so size match
    //    is sufficient proof that a valid model is present.
    if (!rtc_is_trained &&
        nvs_open("antitheft", NVS_READONLY, &_nvs) == ESP_OK) {
      size_t _len = 0;
      if (nvs_get_blob(_nvs, "kmeans", NULL, &_len) == ESP_OK &&
          _len == sizeof(KMeansModel)) {
        rtc_is_trained = true;
        ESP_LOGI(TAG, "NVS: model key found (%u B) — rtc_is_trained restored.",
                 (unsigned)_len);
      }
      nvs_close(_nvs);
    }

    // ── rtc_is_provisioned: check that the hub_mac blob (6 bytes) exists.
    //    No WiFi/ESP-NOW initialisation needed for this check.
    if (!rtc_is_provisioned &&
        nvs_open("storage", NVS_READONLY, &_nvs) == ESP_OK) {
      size_t _len = 0;
      if (nvs_get_blob(_nvs, "hub_mac", NULL, &_len) == ESP_OK && _len == 6) {
        rtc_is_provisioned = true;
        ESP_LOGI(TAG, "NVS: hub_mac found — rtc_is_provisioned restored.");
      }
      nvs_close(_nvs);
    }
  }

  // ═════════════════════════════════════════════════════════════════════════
  //  PATH A — First boot / manual reset
  // ═════════════════════════════════════════════════════════════════════════
  if (wakeup != ESP_SLEEP_WAKEUP_GPIO && wakeup != ESP_SLEEP_WAKEUP_TIMER) {
    ESP_LOGI(TAG, "=== FIRST BOOT / RESET ===");

    // ── Step 1: Init hub comms (Wi-Fi & ESP-NOW) ──────────────────────────
    ESP_ERROR_CHECK(hub_comm_init());

    // ── Print MAC Address ─────────────────────────────────────────────────────
    uint8_t base_mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, base_mac) == ESP_OK) {
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, " SENSOR MAC ADDRESS: %02X:%02X:%02X:%02X:%02X:%02X",
               base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
      ESP_LOGI(TAG, "========================================");
    }

    // ── Step 2 & 4 Concurrent: Pairing & Training ───────────────────────
    bool wait_for_training = false;

    // Start training in background if needed (e.g. first boot)
    bool need_training = !rtc_is_trained;
    if (need_training) {
      if (!s_main_event_group) {
        s_main_event_group = xEventGroupCreate();
      }
      wait_for_training = true;
      xTaskCreate(training_task_wrapper, "training_task", 8192, NULL, 4, NULL);
    }

    // Pairing (waits indefinitely)
    if (!rtc_is_provisioned) {
      if (!hub_comm_is_paired()) {
        ESP_LOGI(TAG, "Not provisioned — waiting indefinitely for hub pairing...");
        hub_comm_pair(portMAX_DELAY);
      }
      // hub_comm_pair() saved the MAC to NVS internally
      rtc_is_provisioned = true;
      rtc_state.thresh_act = THRESHOLD_MG;
      ESP_LOGI(TAG, "Pairing successful. Device is now provisioned.");
    } else {
      ESP_LOGI(TAG, "Already provisioned — skipping pairing.");
    }

    // ── Step 3: Sync clock + query hub for instructions ───────────────────
    hub_info_t info;
    bool got_info = hub_comm_get_information(&info, 5000, 3);

    if (got_info) {
      if (info.do_reset) {
        handle_hub_reset(); // Never returns
      }
      // If we didn't start training but hub requested it now
      if (!need_training && info.do_ml_training) {
        uint32_t exp_ms = (info.ml_duration_ms > 0) ? info.ml_duration_ms / 2 : EXPLORING_DURATION_MS;
        uint32_t trn_ms = (info.ml_duration_ms > 0) ? info.ml_duration_ms / 2 : TRAINING_DURATION_MS;
        run_training_phase(exp_ms, trn_ms);
      }
    } else {
      ESP_LOGW(TAG, "Failed to get info/sync clock from hub after pairing.");
    }

    if (wait_for_training) {
      ESP_LOGI(TAG, "Waiting for background ML training task to complete...");
      xEventGroupWaitBits(s_main_event_group, EVENT_TRAINING_DONE, pdFALSE, pdFALSE, portMAX_DELAY);
      ESP_LOGI(TAG, "Background ML training task completed.");
    }

    // ── Step 5: Deep sleep ────────────────────────────────────────────────
    goto_deep_sleep();
    // Never returns
  }

  // ═════════════════════════════════════════════════════════════════════════
  //  PATH B — Sensor wakeup (ADXL362 activity interrupt)
  // ═════════════════════════════════════════════════════════════════════════
  else if (wakeup == ESP_SLEEP_WAKEUP_GPIO) {
    ESP_LOGI(TAG, "=== SENSOR WAKEUP (knock detected) ===");

    // If the device was never trained, fall back to first-boot path
    if (!rtc_is_trained) {
      ESP_LOGW(TAG, "No trained model — need first-boot training. Restarting.");
      esp_restart();
    }

    // Create the inter-task semaphore
    s_data_ready_sem = xSemaphoreCreateBinary();
    if (!s_data_ready_sem) {
      ESP_LOGE(TAG, "Failed to create semaphore. Rebooting.");
      esp_restart();
    }

    // Sampler (high priority to keep sample timing precise)
    xTaskCreate(sensor_sampler_task, "sampler", 4096, NULL, 5, NULL);

    // ML processor (waits on semaphore, heavy computation)
    xTaskCreate(ml_processor_task, "ml_proc", 8192, NULL, 4, NULL);

    // app_main must not return — suspend it while the tasks run
    vTaskSuspend(NULL);
  }

  // ═════════════════════════════════════════════════════════════════════════
  //  PATH C — Timer wakeup (24-h hub sync)
  // ═════════════════════════════════════════════════════════════════════════
  else { // wakeup == ESP_SLEEP_WAKEUP_TIMER
    ESP_LOGI(TAG, "=== 24-H HUB SYNC ===");

    // EMA decay: large dt since last sensor wake → λ_meas << λ_target → threshold drops
    {
      struct timeval tv_now;
      gettimeofday(&tv_now, NULL);
      ema_adapt_threshold(tv_now.tv_sec, false);
    }

    ESP_ERROR_CHECK(hub_comm_init());

    hub_info_t info;
    if (hub_comm_get_information(&info, 5000, 3)) {
      // Clock sync done inside hub_comm_get_information()
      if (info.do_reset) {
        handle_hub_reset(); // Never returns
      }
      if (info.do_ml_training) {
        uint32_t exp_ms = (info.ml_duration_ms > 0) ? info.ml_duration_ms / 2
                                                    : EXPLORING_DURATION_MS;
        uint32_t trn_ms = (info.ml_duration_ms > 0) ? info.ml_duration_ms / 2
                                                    : TRAINING_DURATION_MS;
        run_training_phase(exp_ms, trn_ms);
      }
    } else {
      ESP_LOGW(TAG, "Hub unreachable during 24-h sync. Skipping.");
    }

    goto_deep_sleep();
    // Never returns
  }
}