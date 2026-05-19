#include "esp_log.h"
#include "nvs_flash.h"
#include "secure_store.h"
#include "cloud_task.h"
#include "hub.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "Initializing secure NVS...");
    ESP_ERROR_CHECK(secure_store_init());

    ESP_LOGI(TAG, "Initializing Cloud Tasks (WiFi + MQTT)...");
    cloud_task_init();

    ESP_LOGI(TAG, "Starting Hub Manager...");
    hub_start();
}
