/**
 * @file main.cpp
 * @brief ADXL362 motion-triggered deep sleep example for ESP32-C3.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "adxl362.h"
#include "fft_processor.h"
#include "esp_dsp.h"
#include "esp_event.h"
#include "tinyml_baseline_model.h"
#include "tinyml_training.h"
#include "feature_extraction.h" 


extern "C" {
#include "secure_store.h"
#include "esp_now_comm.h"
}
#include "Arduino.h"

static const char *TAG = "MAIN";

/*==========================================================
  Global variables
 * Sample structure for storing accelerometer data
 */
struct Sample { int16_t x, y, z; };

enum class SystemMode : uint8_t { EXPLORING, TRAINING, INFERENCE };

static Sample        g_window[SAMPLE_COUNT];
static int           g_window_index    = 0;
static uint32_t      g_window_seq      = 0;
static unsigned long g_last_sample_ms  = 0;
static unsigned long g_phase_start_ms  = 0;

static KMeansModel   g_model;
static SystemMode    g_mode;

/* =========================================================
 *  Pin definitions - change these to match your wiring
 * ========================================================= */
#define MY_SPI_HOST   SPI3_HOST
#define MY_PIN_MOSI   GPIO_NUM_23
#define MY_PIN_MISO   GPIO_NUM_19
#define MY_PIN_SCLK   GPIO_NUM_18
#define MY_PIN_CS     GPIO_NUM_5
#define MY_PIN_INT1   GPIO_NUM_33   /* must be an RTC GPIO for ext0 wakeup */
#define MY_SPI_CLOCK  1000000       /* 1 MHz - reduced for debugging on breadboard */




#define SIGNAL_GAIN  1.0f  // gain factor to apply to raw accelerometer data before feature extraction



/// ─────────────────────────────────────────────────────────────────────────────
///  Thresholds and timings - adjust these for your use case
/// ─────────────────────────────────────────────────────────────────────────────
#define THRESHOLD_MG          150   /* 0.150g change from baseline - good for table knocks */
#define ACTIVITY_TIME_MS        1   /* 1 sample @ 100Hz = 10ms - catches brief impulses */
#define INACTIVITY_TIME_MS   5000   /* ms of no motion before going back to sleep */
#define MOTION_DIFF_MG        80.0f /* mg change between samples to count as motion */




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

/* =========================================================
 *  Helper: configure the ADXL362 and start measurement
 * ========================================================= */
static adxl362_handle_t sensor_init(void)
{
    adxl362_handle_t sensor = NULL;

    adxl362_pins_t pins = {
        .spi_host     = MY_SPI_HOST,
        .pin_mosi     = MY_PIN_MOSI,
        .pin_miso     = MY_PIN_MISO,
        .pin_sclk     = MY_PIN_SCLK,
        .pin_cs       = MY_PIN_CS,
        .spi_clock_hz = MY_SPI_CLOCK,
    };

    esp_err_t ret = adxl362_init(&sensor, &pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADXL362 init failed: %s - check wiring!", esp_err_to_name(ret));
        return NULL;
    }

    adxl362_set_range(sensor, ADXL362_RANGE_2G);
    adxl362_set_odr(sensor, ADXL362_ODR_100_HZ);

    adxl362_set_activity_threshold(sensor, THRESHOLD_MG, ACTIVITY_TIME_MS, true);
    adxl362_set_inactivity_threshold(sensor, THRESHOLD_MG, INACTIVITY_TIME_MS, true);

    adxl362_start_measurement(sensor);

    vTaskDelay(pdMS_TO_TICKS(300));

    uint8_t status = 0;
    adxl362_get_status(sensor, &status);
    if (status & (1 << 4)) {
        ESP_LOGW(TAG, "Activity flag set during init settle - clearing it.");
        vTaskDelay(pdMS_TO_TICKS(200));
        adxl362_get_status(sensor, &status);
    }

    return sensor;
}

/* =========================================================
 *  Helper: go to deep sleep, wake on INT1 HIGH (activity)
 * ========================================================= */
static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Going to deep sleep... waiting for ADXL362 INT1 to wake up.");
    ESP_LOGI(TAG, "-----------------------------------------------------------");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MY_PIN_INT1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  /* keep LOW when idle */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int wait_ms = 0;
    while (gpio_get_level(MY_PIN_INT1) == 1 && wait_ms < 3000) {
        if (wait_ms == 0) {
            ESP_LOGW(TAG, "INT1 is HIGH - waiting for it to go LOW before sleeping...");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_ms += 20;
    }

    if (gpio_get_level(MY_PIN_INT1) == 1) {
        ESP_LOGE(TAG, "INT1 stuck HIGH after 3s - sleeping anyway (may wake immediately)");
    } else {
        ESP_LOGI(TAG, "INT1 is LOW - safe to sleep.");
    }

    rtc_gpio_init(MY_PIN_INT1);
    rtc_gpio_set_direction(MY_PIN_INT1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(MY_PIN_INT1);
    rtc_gpio_pullup_dis(MY_PIN_INT1);

    esp_sleep_enable_ext0_wakeup(MY_PIN_INT1, 1);
    esp_deep_sleep_start();
}






// ─────────────────────────────────────────────────────────────────────────────
//  Phase transitions
// ─────────────────────────────────────────────────────────────────────────────

static void transition_to_training() {


    bool ok = exploring_finalize(g_model);
    if (!ok) {
        printf("[SYSTEM] WARNING: novelty buffer sparse — model may need longer exploring.");
    }

    g_phase_start_ms = millis();
    g_mode = SystemMode::TRAINING;
    printf("[SYSTEM] EXPLORING complete -> TRAINING for %lu s.\n",
           (unsigned long)TRAINING_DURATION_MS / 1000UL);
    delay(1500);
}

static void transition_to_inference() {


    training_finalize(g_model);
    training_save(g_model);

    g_mode = SystemMode::INFERENCE;
    Serial.println("[SYSTEM] TRAINING complete -> INFERENCE mode.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Window processing
// ─────────────────────────────────────────────────────────────────────────────

static void process_window() {
    static int16_t xb[SAMPLE_COUNT], yb[SAMPLE_COUNT], zb[SAMPLE_COUNT];
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        xb[i] = g_window[i].x;
        yb[i] = g_window[i].y;
        zb[i] = g_window[i].z;
    }

    InferenceFeatures feat = compute_features(
        xb, yb, zb, SAMPLE_COUNT, SAMPLING_RATE_HZ);

    switch (g_mode) {

        // ── EXPLORING ────────────────────────────────────────────────────────
        case SystemMode::EXPLORING:
            exploring_update(g_model, feat);


            if (g_window_seq % 150 == 0)
                Serial.printf("[EXPLORE] win=%lu  n=%lu  novel=%u%%\n",
                              (unsigned long)g_window_seq,
                              (unsigned long)g_model.total_samples,
                              exploring_novelty_pct());

            if (millis() - g_phase_start_ms >= EXPLORING_DURATION_MS)
                transition_to_training();
            break;

        // ── TRAINING ─────────────────────────────────────────────────────────
        case SystemMode::TRAINING:
            training_update(g_model, feat);


            if (g_window_seq % 150 == 0) {
                uint32_t elapsed = millis() - g_phase_start_ms;
                Serial.printf("[TRAIN] win=%lu  n=%lu  %lu%%\n",
                              (unsigned long)g_window_seq,
                              (unsigned long)g_model.total_samples,
                              elapsed * 100 / TRAINING_DURATION_MS);
            }

            if (millis() - g_phase_start_ms >= TRAINING_DURATION_MS)
                transition_to_inference();
            break;

        // ── INFERENCE ────────────────────────────────────────────────────────
        case SystemMode::INFERENCE: {
            float dist    = 0.0f;
            int   cluster = -1;
            bool  ok      = training_is_baseline(g_model, feat, &dist, &cluster);

            Serial.printf("[INF] win=%lu  impact=%.4f  m_p99=%.2f  dist=%.4f  C%d  %s\n",
                        (unsigned long)g_window_seq,
                        feat.impact_score, feat.m_p99, dist,
                        cluster,
                        ok ? "BASELINE" : "*** DEVIATION ***");


            Serial.printf("[INF] win=%lu  impact=%.4f  m_p99=%.2f  dist=%.4f  %s\n",
                          (unsigned long)g_window_seq,
                          feat.impact_score, feat.m_p99, dist,
                          ok ? "BASELINE" : "*** DEVIATION ***");
            break;
        }
    }
}

/*===============
* ADXL362 sample
* ===============*/
static adxl362_handle_t sensor_h = NULL;
static bool read_sample(Sample &s) {
    if (sensor_h == NULL) return false;

    adxl362_raw_data_t raw;
    if (adxl362_read_raw(sensor_h, &raw) == ESP_OK) {
        // Applichiamo il SIGNAL_GAIN ai valori raw
        s.x = static_cast<int16_t>(raw.x * SIGNAL_GAIN);
        s.y = static_cast<int16_t>(raw.y * SIGNAL_GAIN);
        s.z = static_cast<int16_t>(raw.z * SIGNAL_GAIN);
        return true;
    }
    return false;
}

/* =========================================================
 *  app_main
 * ========================================================= */
extern "C" void app_main(void)
{
    initArduino();
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

    if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "=== Woken up by ADXL362 (motion detected!) ===");
        
        ESP_ERROR_CHECK(secure_store_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        
        uint8_t gw_mac[6] = {0x24, 0x6F, 0x28, 0xAE, 0x52, 0x10};
        ESP_ERROR_CHECK(esp_now_comm_init(gw_mac, 1));
        
        const char *msg = "KNOCK_DETECTED";
        esp_err_t res = esp_now_comm_send((const uint8_t*)msg, strlen(msg));
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "Alert sent successfully to gateway via ESP-NOW");
        } else {
            ESP_LOGE(TAG, "Failed to send alert via ESP-NOW");
        }
        
    } else {
        ESP_LOGI(TAG, "=== First boot / manual reset ===");
        ESP_LOGI(TAG, "No active session on first boot - configuring and sleeping.");

        adxl362_handle_t s = sensor_init();
        if (!s) {
            while (true) {
                ESP_LOGE(TAG, "ADXL362 not found! Check wiring:");
                ESP_LOGE(TAG, "  MOSI -> GPIO 23  |  MISO -> GPIO 19");
                ESP_LOGE(TAG, "  SCLK -> GPIO 18  |  CS   -> GPIO 5");
                ESP_LOGE(TAG, "  VDD  -> 3.3V      |  GND  -> GND");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        enter_deep_sleep();
        return;
    }

    adxl362_handle_t sensor = sensor_init();
    if (!sensor) {
        ESP_LOGE(TAG, "Could not init sensor after wake, restarting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    ESP_LOGI(TAG, "Printing acceleration data. Will sleep after %d seconds of no motion.",
             INACTIVITY_TIME_MS / 1000);
    ESP_LOGI(TAG, "-----------------------------------------------------------");

    adxl362_data_mg_t prev = {0};
    bool first_sample = true;
    TickType_t last_motion_tick = xTaskGetTickCount();

    const TickType_t grace = pdMS_TO_TICKS(600);
    TickType_t start_tick = xTaskGetTickCount();

    while (true) {
        adxl362_data_mg_t cur;
        if (adxl362_read_mg(sensor, &cur) == ESP_OK) {
            ESP_LOGI(TAG, "X: %7.1f mg  |  Y: %7.1f mg  |  Z: %7.1f mg",
                     cur.x_mg, cur.y_mg, cur.z_mg);

            if (!first_sample) {
                float dx = cur.x_mg - prev.x_mg; if (dx < 0) dx = -dx;
                float dy = cur.y_mg - prev.y_mg; if (dy < 0) dy = -dy;
                float dz = cur.z_mg - prev.z_mg; if (dz < 0) dz = -dz;

                if (dx + dy + dz > MOTION_DIFF_MG) {
                    last_motion_tick = xTaskGetTickCount();
                }
            } else {
                last_motion_tick = xTaskGetTickCount();
                first_sample = false;
            }

            prev = cur;
        }

        bool grace_done = (xTaskGetTickCount() - start_tick) >= grace;

        if (grace_done) {
            TickType_t idle_ms = (xTaskGetTickCount() - last_motion_tick) * portTICK_PERIOD_MS;
            if (idle_ms >= INACTIVITY_TIME_MS) {
                ESP_LOGI(TAG, "No motion for %d seconds - going to sleep.",
                         INACTIVITY_TIME_MS / 1000);
                enter_deep_sleep();
                return;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}