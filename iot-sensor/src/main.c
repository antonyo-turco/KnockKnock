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
 *  │  SENSOR WAKEUP (EXT0 – ADXL362 activity interrupt)          │
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

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ADXL362_utils.h"
#include "config.h"
#include "feature_extraction.h"
#include "hub_communication.h"
#include "secure_store.h"
#include "tinyml_baseline_model.h"
#include "tinyml_training.h"

static const char *TAG = "MAIN";

// ─────────────────────────────────────────────────────────────────────────────
//  RTC memory — persists across deep sleep, reset to 0 on power-on
// ─────────────────────────────────────────────────────────────────────────────

/** Current ADXL362 activity threshold (mg). Adaptively tuned at runtime. */
RTC_DATA_ATTR static uint16_t rtc_threshold_mg = THRESHOLD_MG;

/** Timestamp (seconds since epoch) of the last EXT0 wakeup. */
RTC_DATA_ATTR static int64_t rtc_last_sensor_wakeup_sec = 0;

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

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Erase hub pairing data from NVS and flag the device as unprovisioned.
 *        Called when the hub requests a full reset.
 */
static void handle_hub_reset(void) {
  ESP_LOGI(TAG, "Hub requested device reset — erasing pairing data.");
  nvs_handle_t nvs;
  if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_erase_key(nvs, "hub_mac");
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  training_erase_nvs();
  rtc_is_provisioned = false;
  rtc_is_trained = false;
  rtc_threshold_mg = THRESHOLD_MG;
  esp_restart();
}

/**
 * @brief Configure the ADXL362 with the current adaptive threshold and enter
 *        deep sleep.  Both EXT0 (sensor interrupt) and timer (24-h sync) are
 *        armed as wakeup sources.
 */
static void goto_deep_sleep(void) {
  ESP_LOGI(TAG, "Configuring deep sleep. ADXL362 threshold = %u mg",
           rtc_threshold_mg);

  if (g_sensor) {
    adxl362_set_activity_threshold(g_sensor, rtc_threshold_mg, ACTIVITY_TIME_MS,
                                   true);
    // Stop then restart so the new threshold takes effect
    adxl362_stop_measurement(g_sensor);
    adxl362_start_measurement(g_sensor);
  }

  // EXT0: ADXL362 INT1 line goes high on activity
  esp_sleep_enable_ext0_wakeup(MY_PIN_INT1, 1);

  // Timer: periodic 24-h hub sync
  esp_sleep_enable_timer_wakeup((uint64_t)SYNC_INTERVAL_SEC * 1000000ULL);

  esp_deep_sleep_start();
  // Never returns
}

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
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor sampler task  (Core 0 — tight real-time loop)
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
//  ML processor task  (Core 1 — waits for sampler, then runs inference)
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

  // ── Current time (for threshold adaptation) ───────────────────────────────
  struct timeval tv_now;
  gettimeofday(&tv_now, NULL);

  if (is_baseline) {
    // ── Adaptive threshold logic ──────────────────────────────────────────
    int64_t delta_sec =
        (rtc_last_sensor_wakeup_sec > 0)
            ? (tv_now.tv_sec - rtc_last_sensor_wakeup_sec)
            : (THRESHOLD_ADJUST_TIME_SEC + 1); // treat as "ok" on first event

    if (delta_sec < THRESHOLD_ADJUST_TIME_SEC) {
      // Waking up too frequently — raise threshold to reduce false wakes
      rtc_threshold_mg = (uint16_t)(rtc_threshold_mg + THRESHOLD_STEP_UP);
      if (rtc_threshold_mg > THRESHOLD_MG_MAX) {
        rtc_threshold_mg = THRESHOLD_MG_MAX;
      }
      ESP_LOGI(TAG, "[THRESHOLD] Too frequent (delta=%llds). Raised to %u mg.",
               (long long)delta_sec, rtc_threshold_mg);
    } else {
      // Normal timing — gently lower threshold to stay responsive
      if (rtc_threshold_mg > THRESHOLD_STEP_DOWN + THRESHOLD_MG_MIN) {
        rtc_threshold_mg = (uint16_t)(rtc_threshold_mg - THRESHOLD_STEP_DOWN);
      } else {
        rtc_threshold_mg = THRESHOLD_MG_MIN;
      }
      ESP_LOGI(TAG,
               "[THRESHOLD] Normal timing (delta=%llds). Lowered to %u mg.",
               (long long)delta_sec, rtc_threshold_mg);
    }

    // Remember when this sensor event happened
    rtc_last_sensor_wakeup_sec = tv_now.tv_sec;

  } else {
    // ── ANOMALY: send alarm and resync ────────────────────────────────────
    ESP_LOGW(TAG, "[ML] DEVIATION detected — sending alarm to hub.");

    ESP_ERROR_CHECK(hub_comm_init());

    hub_comm_send_alarm(1 /* alarm_code */, 3 /* max_retries */);

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

  goto_deep_sleep();
  // Never returns
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────

void app_main(void) {
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
  g_sensor = sensor_init();
  if (!g_sensor) {
    ESP_LOGE(TAG, "ADXL362 initialisation failed. Retrying in 5 s...");
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
  if (wakeup != ESP_SLEEP_WAKEUP_EXT0 && wakeup != ESP_SLEEP_WAKEUP_TIMER) {
    ESP_LOGI(TAG, "=== FIRST BOOT / RESET ===");

    // ── Step 1: Init hub comms ────────────────────────────────────────────
    ESP_ERROR_CHECK(hub_comm_init());

    // ── Step 2: Pairing ───────────────────────────────────────────────────
    if (!rtc_is_provisioned) {
      if (!hub_comm_is_paired()) {
        ESP_LOGI(TAG, "Not provisioned — waiting for hub pairing (15 s)...");
        if (!hub_comm_pair(15000)) {
          ESP_LOGE(TAG, "Pairing timeout. Sleeping and retrying on next boot.");
          goto_deep_sleep();
          // Never returns
        }
      }
      // hub_comm_pair() saved the MAC to NVS internally
      rtc_is_provisioned = true;
      rtc_threshold_mg = THRESHOLD_MG;
      ESP_LOGI(TAG, "Pairing successful. Device is now provisioned.");
    } else {
      ESP_LOGI(TAG, "Already provisioned — skipping pairing.");
    }

    // ── Step 3: Sync clock + query hub for instructions ───────────────────
    hub_info_t info;
    bool got_info = hub_comm_get_information(&info, 5000, 3);

    if (got_info) {
      // Clock sync is done inside hub_comm_get_information()
      if (info.do_reset) {
        handle_hub_reset(); // Never returns
      }
    }

    // ── Step 4: Training (if needed or hub requested it) ──────────────────
    bool need_training = !rtc_is_trained || (got_info && info.do_ml_training);

    if (need_training) {
      uint32_t exp_ms, trn_ms;
      if (got_info && info.ml_duration_ms > 0) {
        // Hub specified a total duration → split 50/50
        exp_ms = info.ml_duration_ms / 2;
        trn_ms = info.ml_duration_ms / 2;
      } else {
        exp_ms = EXPLORING_DURATION_MS;
        trn_ms = TRAINING_DURATION_MS;
      }
      run_training_phase(exp_ms, trn_ms);
    } else {
      ESP_LOGI(TAG,
               "Model already trained and hub did not request re-training.");
    }

    // ── Step 5: Deep sleep ────────────────────────────────────────────────
    goto_deep_sleep();
    // Never returns
  }

  // ═════════════════════════════════════════════════════════════════════════
  //  PATH B — Sensor wakeup (ADXL362 activity interrupt)
  // ═════════════════════════════════════════════════════════════════════════
  else if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
    ESP_LOGI(TAG, "=== SENSOR WAKEUP (knock detected) ===");

    // If the device was never trained, fall back to first-boot path
    if (!rtc_is_trained) {
      ESP_LOGW(TAG, "No trained model — need first-boot training. Restarting.");
      esp_restart();
    }

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
          ESP_LOGW(TAG, "Hub unreachable for clock sync — proceeding without valid time.");
        }
        // hub_comm_get_information() calls settimeofday() internally on success
      }
    }

    // Create the inter-task semaphore
    s_data_ready_sem = xSemaphoreCreateBinary();
    if (!s_data_ready_sem) {
      ESP_LOGE(TAG, "Failed to create semaphore. Rebooting.");
      esp_restart();
    }

    // Sampler on Core 0 (high priority to keep sample timing precise)
    xTaskCreatePinnedToCore(sensor_sampler_task, "sampler", 4096, NULL, 5, NULL,
                            0);

    // ML processor on Core 1 (waits on semaphore, heavy computation)
    xTaskCreatePinnedToCore(ml_processor_task, "ml_proc", 8192, NULL, 4, NULL,
                            1);

    // app_main must not return — suspend it while the tasks run
    vTaskSuspend(NULL);
  }

  // ═════════════════════════════════════════════════════════════════════════
  //  PATH C — Timer wakeup (24-h hub sync)
  // ═════════════════════════════════════════════════════════════════════════
  else { // wakeup == ESP_SLEEP_WAKEUP_TIMER
    ESP_LOGI(TAG, "=== 24-H HUB SYNC ===");

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