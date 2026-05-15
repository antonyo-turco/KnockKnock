#include "esp_now_comm.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "secure_store.h"

static const char *TAG = "ESP_NOW_COMM";
#define CHANNEL_NVS_KEY "wifi_channel"

static SemaphoreHandle_t s_send_sem = NULL;
static esp_now_send_status_t s_send_status = ESP_NOW_SEND_FAIL;
static uint8_t s_gateway_mac[6];
static uint8_t s_current_channel = 1;
static int s_fail_count = 0;

// Obfuscated PMK (Primary Master Key) - 16 bytes
// Original: "MyEspNowPmkKey12"
static const uint8_t obfuscated_pmk[16] = {
    'M'^0x7B, 'y'^0x7B, 'E'^0x7B, 's'^0x7B, 'p'^0x7B, 'N'^0x7B, 'o'^0x7B, 'w'^0x7B,
    'P'^0x7B, 'm'^0x7B, 'k'^0x7B, 'K'^0x7B, 'e'^0x7B, 'y'^0x7B, '1'^0x7B, '2'^0x7B
};

// Obfuscated LMK (Local Master Key) - 16 bytes
// Original: "MyEspNowLmkKey34"
static const uint8_t obfuscated_lmk[16] = {
    'M'^0x4C, 'y'^0x4C, 'E'^0x4C, 's'^0x4C, 'p'^0x4C, 'N'^0x4C, 'o'^0x4C, 'w'^0x4C,
    'L'^0x4C, 'm'^0x4C, 'k'^0x4C, 'K'^0x4C, 'e'^0x4C, 'y'^0x4C, '3'^0x4C, '4'^0x4C
};

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void esp_now_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
#else
static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
    s_send_status = status;
    if (s_send_sem) {
        xSemaphoreGive(s_send_sem);
    }
}

static esp_err_t add_peer(uint8_t channel) {
    if (esp_now_is_peer_exist(s_gateway_mac)) {
        esp_now_del_peer(s_gateway_mac);
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, s_gateway_mac, 6);
    peerInfo.channel = channel;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = true;

    uint8_t lmk_plain[16];
    secure_store_deobfuscate(obfuscated_lmk, 16, 0x4C, lmk_plain);
    memcpy(peerInfo.lmk, lmk_plain, 16);

    return esp_now_add_peer(&peerInfo);
}

esp_err_t esp_now_comm_init(const uint8_t *gateway_mac, uint8_t default_channel) {
    if (!gateway_mac) return ESP_ERR_INVALID_ARG;
    memcpy(s_gateway_mac, gateway_mac, 6);
    
    // Attempt to load saved channel from secure_store
    char *saved_channel_str = NULL;
    if (secure_store_read_string(CHANNEL_NVS_KEY, &saved_channel_str) == ESP_OK && saved_channel_str) {
        s_current_channel = (uint8_t)atoi(saved_channel_str);
        free(saved_channel_str);
        if (s_current_channel < 1 || s_current_channel > 13) {
            s_current_channel = default_channel;
        }
    } else {
        s_current_channel = default_channel;
    }

    ESP_LOGI(TAG, "Initializing Wi-Fi for ESP-NOW. Channel: %d", s_current_channel);

    ESP_ERROR_CHECK(esp_netif_init());
    // Note: esp_event_loop_create_default() should be called in main.c
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());

    uint8_t pmk_plain[16];
    secure_store_deobfuscate(obfuscated_pmk, 16, 0x7B, pmk_plain);
    ESP_ERROR_CHECK(esp_now_set_pmk(pmk_plain));

    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));

    s_send_sem = xSemaphoreCreateBinary();
    
    return add_peer(s_current_channel);
}

static esp_err_t do_send_and_wait(const uint8_t *data, size_t len) {
    esp_err_t err = esp_now_send(s_gateway_mac, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Wait for callback up to 500ms
    if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_send_status == ESP_NOW_SEND_SUCCESS) {
            return ESP_OK;
        } else {
            return ESP_FAIL;
        }
    }
    
    return ESP_ERR_TIMEOUT;
}

static esp_err_t channel_hop_and_ping(const uint8_t *data, size_t len) {
    ESP_LOGW(TAG, "Starting channel hopping...");
    
    for (uint8_t ch = 1; ch <= 13; ch++) {
        ESP_LOGI(TAG, "Testing channel %d...", ch);
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        add_peer(ch);
        
        esp_err_t res = do_send_and_wait(data, len);
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "Channel %d is working! Saving...", ch);
            s_current_channel = ch;
            s_fail_count = 0;
            
            // Save to NVS
            char ch_str[8];
            snprintf(ch_str, sizeof(ch_str), "%d", ch);
            secure_store_write_string(CHANNEL_NVS_KEY, ch_str);
            
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    ESP_LOGE(TAG, "Channel hopping failed. Target unreachable.");
    // Revert to original
    esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
    add_peer(s_current_channel);
    return ESP_FAIL;
}

esp_err_t esp_now_comm_send(const uint8_t *data, size_t len) {
    esp_err_t res = do_send_and_wait(data, len);
    if (res == ESP_OK) {
        s_fail_count = 0;
        return ESP_OK;
    }
    
    s_fail_count++;
    ESP_LOGW(TAG, "Send failed. Consecutive failures: %d", s_fail_count);
    
    if (s_fail_count >= MAX_ESP_NOW_FAILURES) {
        return channel_hop_and_ping(data, len);
    }
    
    return res;
}
