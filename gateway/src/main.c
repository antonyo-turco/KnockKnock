/**
 * @file main.c
 * @brief Entry point for the KnockKnock gateway firmware.
 *
 * Boot sequence:
 *   1. Initialise the encrypted NVS partition (secure_store).
 *   2. Start the gateway (ESP-NOW + serial bridge + orchestration task).
 */

#include "secure_store.h"
#include "gateway.h"

#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "KnockKnock Gateway booting...");

    /* Initialise encrypted NVS – must be the very first call. */
    ESP_ERROR_CHECK(secure_store_init());

    /* Start the gateway subsystem. */
    ESP_ERROR_CHECK(gateway_start());

    /* gateway_start() launches its own FreeRTOS task; app_main can exit. */
    ESP_LOGI(TAG, "Boot complete.");
}