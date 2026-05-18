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

#include <stdbool.h>
#include "config.h"
#include "ADXL362_utils.h"

#include "secure_store.h"
#include "hub_communication.h"

static const char *TAG_MAIN = "MAIN";






/* =========================================================
 *  Helper: go to deep sleep, wake on INT1 HIGH (activity)
 * ========================================================= */
static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG_MAIN, "Going to deep sleep... waiting for ADXL362 INT1 to wake up.");
    ESP_LOGI(TAG_MAIN, "-----------------------------------------------------------");

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
            ESP_LOGW(TAG_MAIN, "INT1 is HIGH - waiting for it to go LOW before sleeping...");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_ms += 20;
    }

    if (gpio_get_level(MY_PIN_INT1) == 1) {
        ESP_LOGE(TAG_MAIN, "INT1 stuck HIGH after 3s - sleeping anyway (may wake immediately)");
    } else {
        ESP_LOGI(TAG_MAIN, "INT1 is LOW - safe to sleep.");
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

/* =========================================================
 *  app_main
 * ========================================================= */
void app_main(void)
{
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

    if (wakeup == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG_MAIN, "=== Woken up by ADXL362 (motion detected!) ===");
        
        ESP_ERROR_CHECK(hub_comm_init());
        
        if (!hub_comm_is_paired()) {
            ESP_LOGI(TAG_MAIN, "Not paired. Waiting for pairing from Hub (10 seconds)...");
            hub_comm_pair(10000);
        }

        if (hub_comm_is_paired()) {
            ESP_LOGI(TAG_MAIN, "Sending knock alarm...");
            bool success = hub_comm_send_alarm(1, 3); // Alarm code 1, up to 3 retries
            if (success) {
                ESP_LOGI(TAG_MAIN, "Knock alert sent successfully to Hub");
            } else {
                ESP_LOGE(TAG_MAIN, "Failed to send knock alert to Hub");
            }
        } else {
            ESP_LOGE(TAG_MAIN, "Device is not paired. Cannot send alarm.");
        }
    } else {
        ESP_LOGI(TAG_MAIN, "=== First boot / manual reset ===");
        ESP_LOGI(TAG_MAIN, "No active session on first boot - configuring and sleeping.");

        adxl362_handle_t s = sensor_init();
        if (!s) {
            while (true) {
                ESP_LOGE(TAG_MAIN, "ADXL362 not found! Check wiring:");
                ESP_LOGE(TAG_MAIN, "  MOSI -> GPIO 23  |  MISO -> GPIO 19");
                ESP_LOGE(TAG_MAIN, "  SCLK -> GPIO 18  |  CS   -> GPIO 5");
                ESP_LOGE(TAG_MAIN, "  VDD  -> 3.3V      |  GND  -> GND");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        enter_deep_sleep();
        return;
    }

    adxl362_handle_t sensor = sensor_init();
    if (!sensor) {
        ESP_LOGE(TAG_MAIN, "Could not init sensor after wake, restarting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    ESP_LOGI(TAG_MAIN, "Printing acceleration data. Will sleep after %d seconds of no motion.",
             INACTIVITY_TIME_MS / 1000);
    ESP_LOGI(TAG_MAIN, "-----------------------------------------------------------");

    adxl362_data_mg_t prev = {0};
    bool first_sample = true;
    TickType_t last_motion_tick = xTaskGetTickCount();

    const TickType_t grace = pdMS_TO_TICKS(600);
    TickType_t start_tick = xTaskGetTickCount();

    while (true) {
        adxl362_data_mg_t cur;
        if (adxl362_read_mg(sensor, &cur) == ESP_OK) {
            ESP_LOGI(TAG_MAIN, "X: %7.1f mg  |  Y: %7.1f mg  |  Z: %7.1f mg",
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
                ESP_LOGI(TAG_MAIN, "No motion for %d seconds - going to sleep.",
                         INACTIVITY_TIME_MS / 1000);
                enter_deep_sleep();
                return;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}