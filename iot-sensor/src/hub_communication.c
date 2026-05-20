#include "hub_communication.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "secure_store.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG_HUB_COMM = "HUB_COMM";

// NVS Keys
#define NVS_NAMESPACE "storage"
#define NVS_KEY_HUB_MAC "hub_mac"

// State variables
static uint8_t s_hub_mac[ESP_NOW_ETH_ALEN] = {0};
static bool s_is_paired = false;

static uint8_t s_esp_now_pmk[16];
static uint8_t s_esp_now_lmk[16];

// FreeRTOS Event Group for syncrhonize async callbacks with main task
static EventGroupHandle_t s_espnow_event_group;
#define EVENT_SEND_SUCCESS BIT0
#define EVENT_SEND_FAIL BIT1
#define EVENT_RECV_PAIR BIT2
#define EVENT_RECV_INFO BIT3

// Temporary global buffer for received data
static esp_now_packet_t s_last_recv_packet;
static uint8_t s_last_recv_mac[ESP_NOW_ETH_ALEN];

// Callbacks of ESP-NOW (Updated for ESP-IDF v5+)

static void on_data_sent(const uint8_t *mac_addr,
                         esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    xEventGroupSetBits(s_espnow_event_group, EVENT_SEND_SUCCESS);
  } else {
    xEventGroupSetBits(s_espnow_event_group, EVENT_SEND_FAIL);
  }
}

// In ESP-IDF v5, the first argument is no longer a pointer to the MAC, but a
// struct "esp_now_recv_info_t"
static void on_data_recv(const esp_now_recv_info_t *esp_now_info,
                         const uint8_t *data, int len) {
  if (len < sizeof(uint8_t))
    return; // Packet too small

  // Get the MAC address from the new struct
  const uint8_t *mac_addr = esp_now_info->src_addr;

  esp_now_packet_t *packet = (esp_now_packet_t *)data;

  // Save the MAC address of the sender
  memcpy(s_last_recv_mac, mac_addr, ESP_NOW_ETH_ALEN);

  if (packet->type == MSG_TYPE_PAIR) {
    xEventGroupSetBits(s_espnow_event_group, EVENT_RECV_PAIR);
  } else if (packet->type == MSG_TYPE_INFO_RESP) {
    // Copia il payload per poterlo leggere nel task principale
    memcpy(&s_last_recv_packet, packet, len);
    xEventGroupSetBits(s_espnow_event_group, EVENT_RECV_INFO);
  }
}

// --- INTERNAL HELPER FUNCTIONS ---

static void add_hub_peer(const uint8_t *mac) {
  esp_now_peer_info_t peer_info = {};
  peer_info.channel = WIFI_CHANNEL;
  peer_info.ifidx = WIFI_IF_STA;
  peer_info.encrypt = true;
  memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);
  memcpy(peer_info.lmk, s_esp_now_lmk, 16);

  if (!esp_now_is_peer_exist(mac)) {
    esp_now_add_peer(&peer_info);
  }
}

// --- PUBLIC API IMPLEMENTATION ---

esp_err_t hub_comm_init(void) {
  s_espnow_event_group = xEventGroupCreate();

  // 1. Initialize NVS in a secure way
  esp_err_t err = secure_store_init();
  ESP_ERROR_CHECK(err);

  // Carica le chiavi PMK e LMK dall'NVS sicuro, o usa i default
  char *pmk_str = NULL;
  char *lmk_str = NULL;

  if (secure_store_read_string("esp_now_pmk", &pmk_str) == ESP_OK) {
    memcpy(s_esp_now_pmk, pmk_str, 16);
    free(pmk_str);
  } else {
    memcpy(s_esp_now_pmk, DEFAULT_ESP_NOW_PMK, 16);
    secure_store_write_string("esp_now_pmk", DEFAULT_ESP_NOW_PMK);
  }

  if (secure_store_read_string("esp_now_lmk", &lmk_str) == ESP_OK) {
    memcpy(s_esp_now_lmk, lmk_str, 16);
    free(lmk_str);
  } else {
    memcpy(s_esp_now_lmk, DEFAULT_ESP_NOW_LMK, 16);
    secure_store_write_string("esp_now_lmk", DEFAULT_ESP_NOW_LMK);
  }

  // 2. Initialize Wi-Fi in Station mode
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Set the Wi-Fi channel (essential for ESP-NOW)
  ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

  // 3. Initialize ESP-NOW
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)on_data_sent));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

  // Set the Primary Master Key (PMK) loaded from NVS
  ESP_ERROR_CHECK(esp_now_set_pmk(s_esp_now_pmk));

  // 4. Load the MAC from NVS
  uint8_t *mac_data = NULL;
  size_t mac_len = 0;
  err = secure_store_read(NVS_KEY_HUB_MAC, &mac_data, &mac_len);
  if (err == ESP_OK && mac_len == ESP_NOW_ETH_ALEN) {
    memcpy(s_hub_mac, mac_data, ESP_NOW_ETH_ALEN);
    s_is_paired = true;
    add_hub_peer(s_hub_mac);
    ESP_LOGI(TAG_HUB_COMM,
             "Hub MAC caricato in modo sicuro: %02X:%02X:%02X:%02X:%02X:%02X",
             s_hub_mac[0], s_hub_mac[1], s_hub_mac[2], s_hub_mac[3],
             s_hub_mac[4], s_hub_mac[5]);
  }
  if (mac_data)
    free(mac_data);

  return ESP_OK;
}

bool hub_comm_is_paired(void) { return s_is_paired; }

bool hub_comm_pair(uint32_t timeout_ms) {
  ESP_LOGI(TAG_HUB_COMM, "Inviando richiesta di pairing (broadcast)...");

  uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peer_info = {};
  peer_info.channel = WIFI_CHANNEL;
  peer_info.ifidx = WIFI_IF_STA;
  peer_info.encrypt = false;
  memcpy(peer_info.peer_addr, bcast_mac, ESP_NOW_ETH_ALEN);
  esp_now_add_peer(&peer_info);

  esp_now_packet_t req_packet = {.type = MSG_TYPE_PAIR};
  xEventGroupClearBits(s_espnow_event_group, EVENT_RECV_PAIR);
  esp_now_send(bcast_mac, (uint8_t *)&req_packet, sizeof(req_packet.type));

  // Wait until a MSG_TYPE_PAIR packet is received (from Gateway) or timeout
  EventBits_t bits =
      xEventGroupWaitBits(s_espnow_event_group, EVENT_RECV_PAIR, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));

  esp_now_del_peer(bcast_mac);

  if (bits & EVENT_RECV_PAIR) {
    // Save the MAC address in a secure way
    memcpy(s_hub_mac, s_last_recv_mac, ESP_NOW_ETH_ALEN);
    secure_store_write(NVS_KEY_HUB_MAC, s_hub_mac, ESP_NOW_ETH_ALEN);

    s_is_paired = true;
    add_hub_peer(s_hub_mac); // Add as an encrypted peer
    ESP_LOGI(TAG_HUB_COMM, "Pairing avvenuto con successo.");
    return true;
  }

  ESP_LOGE(TAG_HUB_COMM, "Timeout pairing.");
  return false;
}

bool hub_comm_send_alarm(uint8_t alarm_code, uint8_t max_retries) {
  if (!s_is_paired)
    return false;

  esp_now_packet_t packet = {.type = MSG_TYPE_ALARM,
                             .payload.alarm_code = alarm_code};

  for (uint8_t i = 0; i < max_retries; i++) {
    xEventGroupClearBits(s_espnow_event_group,
                         EVENT_SEND_SUCCESS | EVENT_SEND_FAIL);

    esp_err_t err =
        esp_now_send(s_hub_mac, (uint8_t *)&packet,
                     sizeof(packet.type) + sizeof(packet.payload.alarm_code));
    if (err != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Wait for ACK (MAC level)
    EventBits_t bits = xEventGroupWaitBits(s_espnow_event_group,
                                           EVENT_SEND_SUCCESS | EVENT_SEND_FAIL,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(100));

    if (bits & EVENT_SEND_SUCCESS) {
      ESP_LOGI(TAG_HUB_COMM, "Allarme inviato con successo (ACK ricevuto).");
      return true;
    }

    ESP_LOGW(TAG_HUB_COMM, "Failed to send alarm, retrying... (%d/%d)", i + 1,
             max_retries);
    // Small backoff before retrying
    vTaskDelay(pdMS_TO_TICKS(10 + (i * 10)));
  }

  ESP_LOGE(TAG_HUB_COMM, "Unable to send alarm after %d attempts.",
           max_retries);
  return false;
}

bool hub_comm_get_information(hub_info_t *info, uint32_t timeout_ms,
                              uint8_t max_retries) {
  if (!s_is_paired || !info)
    return false;

  esp_now_packet_t req_packet = {.type = MSG_TYPE_INFO_REQ};

  for (uint8_t i = 0; i < max_retries; i++) {
    // Clear the flags before each attempt
    xEventGroupClearBits(s_espnow_event_group, EVENT_RECV_INFO |
                                                   EVENT_SEND_SUCCESS |
                                                   EVENT_SEND_FAIL);

    // 1. Attempt to send
    esp_err_t err = esp_now_send(s_hub_mac, (uint8_t *)&req_packet,
                                 sizeof(req_packet.type));
    if (err != ESP_OK) {
      ESP_LOGW(TAG_HUB_COMM, "Errore interno ESP-NOW, ritento... (%d/%d)",
               i + 1, max_retries);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // 2. Wait for ACK (MAC level - did the packet reach the Hub's antenna?)
    // Use a very short timeout ( 50ms) for the MAC-level ACK
    EventBits_t send_bits = xEventGroupWaitBits(
        s_espnow_event_group, EVENT_SEND_SUCCESS | EVENT_SEND_FAIL, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(50));

    if (!(send_bits & EVENT_SEND_SUCCESS)) {
      ESP_LOGW(TAG_HUB_COMM, "Attempt %d: No ACK received from Hub.", i + 1);
      vTaskDelay(
          pdMS_TO_TICKS(10 + (i * 10))); // Incremental backoff (10ms, 20ms...)
      continue;                          // Skip the rest and try again
    }

    // 3. ACK received! Now wait for the application response with the
    // information
    EventBits_t recv_bits =
        xEventGroupWaitBits(s_espnow_event_group, EVENT_RECV_INFO, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (recv_bits & EVENT_RECV_INFO) {
      // Success: populate the struct for the user
      info->timestamp = s_last_recv_packet.payload.info_resp.timestamp;
      info->do_ml_training =
          s_last_recv_packet.payload.info_resp.do_ml_training;
      info->ml_duration_ms =
          s_last_recv_packet.payload.info_resp.ml_duration_ms;
      info->do_reset = s_last_recv_packet.payload.info_resp.do_reset;

      // --- START OF AUTOMATIC CLOCK SYNCHRONIZATION ---
      // A timestamp > 1000000000 ensures that it is a valid date (after 2001)
      if (info->timestamp > 1000000000) {
        // Set the Italian time zone
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();

        // Updates the system clock (RTC)
        struct timeval tv;
        tv.tv_sec = info->timestamp;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        ESP_LOGI(TAG_HUB_COMM, "Orologio sincronizzato internamente a: %lu",
                 info->timestamp);
      } else {
        ESP_LOGW(TAG_HUB_COMM,
                 "Received timestamp is not valid (%lu), clock not updated.",
                 info->timestamp);
      }
      // --- END OF AUTOMATIC CLOCK SYNCHRONIZATION ---

      ESP_LOGI(TAG_HUB_COMM, "Info received successfully on attempt %d", i + 1);
      return true;
    } else {
      ESP_LOGW(TAG_HUB_COMM,
               "Tentativo %d: L'Hub non ha risposto in tempo (%lu ms).", i + 1,
               timeout_ms);
    }
  }

  ESP_LOGE(TAG_HUB_COMM, "Impossibile recuperare le info dopo %d tentativi.",
           max_retries);
  return false;
}