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

#include "config.h"
#include "ADXL362_utils.h"
#include "ml_controller.h"

extern "C" {
#include "secure_store.h"
#include "esp_now_comm.h"
}
#include "Arduino.h"

static const char *TAG = "MAIN";






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