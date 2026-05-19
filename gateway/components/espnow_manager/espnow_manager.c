/**
 * @file espnow_manager.c
 * @brief ESP-NOW manager implementation for the KnockKnock gateway.
 *
 * Handles Wi-Fi STA initialisation, ESP-NOW lifecycle, peer management
 * and thread-safe send/receive primitives.
 */

#include "espnow_manager.h"
#include "secure_store.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "ESPNOW_MGR";

/* -------------------------------------------------------------------------- */
/*  NVS keys for ESP-NOW security material                                    */
/* -------------------------------------------------------------------------- */

#define NVS_KEY_PMK "esp_now_pmk"
#define NVS_KEY_LMK "esp_now_lmk"

#include "../../include/secrets.h"

/* -------------------------------------------------------------------------- */
/*  Internal state                                                             */
/* -------------------------------------------------------------------------- */

/** FreeRTOS event bits used to synchronise the send callback. */
#define SEND_ACK_OK   BIT0
#define SEND_ACK_FAIL BIT1

static EventGroupHandle_t s_send_event_group = NULL;
static espnow_recv_cb_t   s_user_recv_cb     = NULL;

static uint8_t s_pmk[16];
static uint8_t s_lmk[16];

/* -------------------------------------------------------------------------- */
/*  Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Load a 16-byte key from secure NVS.  If not found, write the default
 *        value and use it instead.
 */
static void load_or_create_key(const char *nvs_key,
                               const char *default_val,
                               uint8_t    *out_buf)
{
    char *val = NULL;
    if (secure_store_read_string(nvs_key, &val) == ESP_OK) {
        memcpy(out_buf, val, 16);
        free(val);
        ESP_LOGI(TAG, "Loaded key '%s' from secure NVS.", nvs_key);
    } else {
        memcpy(out_buf, default_val, 16);
        secure_store_write_string(nvs_key, default_val);
        ESP_LOGW(TAG, "Key '%s' not found – using and persisting default.", nvs_key);
    }
}

/* -------------------------------------------------------------------------- */
/*  ESP-NOW callbacks                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send-done callback (runs at ISR level – keep it minimal).
 */
static void on_data_sent(const esp_now_send_info_t *tx_info,
                         esp_now_send_status_t status)
{
    (void)tx_info;
    BaseType_t woken = pdFALSE;
    if (status == ESP_NOW_SEND_SUCCESS) {
        xEventGroupSetBitsFromISR(s_send_event_group, SEND_ACK_OK, &woken);
    } else {
        xEventGroupSetBitsFromISR(s_send_event_group, SEND_ACK_FAIL, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Receive callback (ESP-IDF v5 signature).
 *        Forwards the frame to the user-registered callback.
 */
static void on_data_recv(const esp_now_recv_info_t *info,
                         const uint8_t             *data,
                         int                        len)
{
    if (len < (int)sizeof(uint8_t)) {
        ESP_LOGW(TAG, "Received packet too short (%d bytes), discarding.", len);
        return;
    }

    if (s_user_recv_cb != NULL) {
        s_user_recv_cb(info->src_addr, (const gw_espnow_packet_t *)data, len);
    }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t espnow_manager_init(espnow_recv_cb_t recv_cb)
{
    esp_err_t err;

    s_user_recv_cb   = recv_cb;
    s_send_event_group = xEventGroupCreate();
    if (s_send_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create send event group.");
        return ESP_ERR_NO_MEM;
    }

    /* ------------------------------------------------------------------ */
    /* 1. Load security keys from encrypted NVS                            */
    /* ------------------------------------------------------------------ */
    load_or_create_key(NVS_KEY_PMK, DEFAULT_PMK, s_pmk);
    load_or_create_key(NVS_KEY_LMK, DEFAULT_LMK, s_lmk);

    /* ------------------------------------------------------------------ */
    /* 2. Initialise Wi-Fi in STA mode                                     */
    /* ------------------------------------------------------------------ */
    err = esp_netif_init();
    if (err != ESP_OK) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    /* Fix channel – all nodes must operate on the same channel. */
    err = esp_wifi_set_channel(GATEWAY_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) return err;

    /* ------------------------------------------------------------------ */
    /* 3. Initialise ESP-NOW                                               */
    /* ------------------------------------------------------------------ */
    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_send_cb(on_data_sent);
    if (err != ESP_OK) return err;

    err = esp_now_register_recv_cb(on_data_recv);
    if (err != ESP_OK) return err;

    /* Set Primary Master Key (PMK) – used to derive per-peer session keys. */
    err = esp_now_set_pmk(s_pmk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_set_pmk failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW manager initialised on channel %d.", GATEWAY_WIFI_CHANNEL);
    return ESP_OK;
}

esp_err_t espnow_manager_add_peer(const uint8_t *mac)
{
    if (mac == NULL) return ESP_ERR_INVALID_ARG;

    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK; /* Already registered – idempotent. */
    }

    // --- Send unencrypted PAIR ACK back to sensor ---
    esp_now_peer_info_t peer = {0};
    peer.channel = GATEWAY_WIFI_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&peer);

    gw_espnow_packet_t pkt = { .type = MSG_TYPE_PAIR };
    esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt.type));
    vTaskDelay(pdMS_TO_TICKS(50)); // Give time for packet to be sent

    esp_now_del_peer(mac);
    // ------------------------------------------------

    peer.encrypt = true;
    memcpy(peer.lmk,       s_lmk, 16);

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer %02X:%02X:%02X:%02X:%02X:%02X: %s",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Peer added: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return err;
}

bool espnow_manager_is_peer(const uint8_t *mac)
{
    if (mac == NULL) return false;
    return esp_now_is_peer_exist(mac);
}

bool espnow_manager_send(const uint8_t         *dest_mac,
                         const gw_espnow_packet_t *packet,
                         size_t                 len,
                         uint32_t               timeout_ms)
{
    if (dest_mac == NULL || packet == NULL || len == 0) return false;

    /* Clear stale ACK bits before transmitting. */
    xEventGroupClearBits(s_send_event_group, SEND_ACK_OK | SEND_ACK_FAIL);

    esp_err_t err = esp_now_send(dest_mac, (const uint8_t *)packet, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send error: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_send_event_group,
        SEND_ACK_OK | SEND_ACK_FAIL,
        pdTRUE,   /* Clear bits on exit. */
        pdFALSE,  /* Wait for any one bit. */
        pdMS_TO_TICKS(timeout_ms));

    if (bits & SEND_ACK_OK) {
        return true;
    }

    ESP_LOGW(TAG, "Send to %02X:%02X:%02X:%02X:%02X:%02X failed (timeout or NACK).",
             dest_mac[0], dest_mac[1], dest_mac[2],
             dest_mac[3], dest_mac[4], dest_mac[5]);
    return false;
}

void espnow_manager_deinit(void)
{
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_send_event_group != NULL) {
        vEventGroupDelete(s_send_event_group);
        s_send_event_group = NULL;
    }

    ESP_LOGI(TAG, "ESP-NOW manager de-initialised.");
}
