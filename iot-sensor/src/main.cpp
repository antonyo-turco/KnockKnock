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
extern "C" {
#include "secure_store.h"
#include "esp_now_comm.h"
}
#include "Arduino.h"

static const char *TAG = "MAIN";

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

/* =========================================================
 *  Threshold config - tuned for KNOCK ON TABLE detection
 * ========================================================= */
#define THRESHOLD_MG          150   /* 0.150g change from baseline - good for table knocks */
#define ACTIVITY_TIME_MS        1   /* 1 sample @ 100Hz = 10ms - catches brief impulses */
#define INACTIVITY_TIME_MS   5000   /* ms of no motion before going back to sleep */
#define MOTION_DIFF_MG        80.0f /* mg change between samples to count as motion */

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