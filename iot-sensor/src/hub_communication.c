#include "hub_communication.h"
#include "esp_idf_version.h"
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

// Callbacks of ESP-NOW (Conditional signature for ESP-IDF versions)

static void on_data_sent(const uint8_t *mac_addr,
                         esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    xEventGroupSetBits(s_espnow_event_group, EVENT_SEND_SUCCESS);
  } else {
    xEventGroupSetBits(s_espnow_event_group, EVENT_SEND_FAIL);
  }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_data_recv(const esp_now_recv_info_t *esp_now_info,
                         const uint8_t *data, int len) {
  if (len < sizeof(uint8_t))
    return; // Packet too small
  const uint8_t *mac_addr = esp_now_info->src_addr;
#else
static void on_data_recv(const uint8_t *mac_addr,
                         const uint8_t *data, int len) {
  if (len < sizeof(uint8_t))
    return; // Packet too small
#endif

  esp_now_packet_t *packet = (esp_now_packet_t *)data;

  ESP_LOGI(TAG_HUB_COMM, "RAW RECV: len=%d, type=0x%02X da %02X:%02X:%02X:%02X:%02X:%02X", 
           len, packet->type, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

  // Save the MAC address of the sender
  memcpy(s_last_recv_mac, mac_addr, ESP_NOW_ETH_ALEN);

  if (packet->type == MSG_TYPE_PAIR_ACK) {
    ESP_LOGI(TAG_HUB_COMM, "Ricevuto PAIR ACK dal Gateway %02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    // Check if the ACK contains our MAC in the payload to ensure it's meant for us.
    // IMPORTANT: Always use WIFI_IF_STA — this is the MAC address the gateway sees
    // as src_addr when we send broadcasts, regardless of APSTA mode on C3.
    uint8_t my_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);
    
    if (len >= sizeof(packet->type) + 6 && 
        memcmp(packet->payload.pair_req.target_mac, my_mac, 6) == 0) {
      
      esp_now_peer_info_t gw_peer = {0};
      gw_peer.channel = WIFI_CHANNEL;
#ifndef C3_BUILD
      gw_peer.ifidx = WIFI_IF_STA;
#else
      gw_peer.ifidx = WIFI_IF_AP;
#endif
      gw_peer.encrypt = false;
      memcpy(gw_peer.peer_addr, mac_addr, 6);
      if (esp_now_is_peer_exist(mac_addr)) esp_now_mod_peer(&gw_peer);
      else esp_now_add_peer(&gw_peer);

      xEventGroupSetBits(s_espnow_event_group, EVENT_RECV_PAIR);
    }
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
#ifndef C3_BUILD
  peer_info.ifidx = WIFI_IF_STA;
#else
  peer_info.ifidx = WIFI_IF_AP;
#endif
  peer_info.encrypt = true;
  memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);
  memcpy(peer_info.lmk, s_esp_now_lmk, 16);

  if (!esp_now_is_peer_exist(mac)) {
    esp_now_add_peer(&peer_info);
  }
}

// --- PUBLIC API IMPLEMENTATION ---

esp_err_t hub_comm_init(void) {
  // Guard against double-init: PATH B calls this for clock sync, then
  // again from ml_processor_task if a DEVIATION triggers an alarm.
  static bool s_initialized = false;
  if (s_initialized) {
    return ESP_OK;
  }

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
#ifndef C3_BUILD
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#else
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
#endif
  
  // Disabilita il risparmio energetico Wi-Fi per evitare di perdere pacchetti ESP-NOW
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

#ifndef C3_BUILD
  // Forza il protocollo standard B/G/N per evitare mismatch tra versioni diverse di ESP-IDF
  ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
#else
  ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
#endif

  ESP_ERROR_CHECK(esp_wifi_start());
  
#ifdef C3_BUILD
  esp_wifi_set_max_tx_power(34); // 34 is equivalent to +20dBm
#endif

#ifndef C3_BUILD
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
#endif

  // Set the Wi-Fi channel (essential for ESP-NOW)
  ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

  // 3. Initialize ESP-NOW
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)on_data_sent));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

  // Set the Primary Master Key (PMK) loaded from NVS
  // ESP_ERROR_CHECK(esp_now_set_pmk(s_esp_now_pmk));

  // Add unencrypted broadcast peer for pairing discovery
  esp_now_peer_info_t bcast_peer = {0};
  bcast_peer.channel = WIFI_CHANNEL;
  bcast_peer.ifidx = WIFI_IF_STA;
  bcast_peer.encrypt = false;
  memset(bcast_peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
  esp_err_t add_err = esp_now_add_peer(&bcast_peer);
  if (add_err != ESP_OK && add_err != ESP_ERR_ESPNOW_EXIST) {
      ESP_LOGW(TAG_HUB_COMM, "Failed to add STA broadcast peer: %s", esp_err_to_name(add_err));
  } else {
      ESP_LOGI(TAG_HUB_COMM, "STA Broadcast peer registered (unencrypted).");
  }

#ifdef C3_BUILD
  // Also add broadcast peer on AP interface for sending compatibility under C3_BUILD
  esp_now_peer_info_t bcast_peer_ap = {0};
  bcast_peer_ap.channel = WIFI_CHANNEL;
  bcast_peer_ap.ifidx = WIFI_IF_AP;
  bcast_peer_ap.encrypt = false;
  memset(bcast_peer_ap.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
  add_err = esp_now_add_peer(&bcast_peer_ap);
  if (add_err != ESP_OK && add_err != ESP_ERR_ESPNOW_EXIST) {
      ESP_LOGW(TAG_HUB_COMM, "Failed to add AP broadcast peer: %s", esp_err_to_name(add_err));
  } else {
      ESP_LOGI(TAG_HUB_COMM, "AP Broadcast peer registered (unencrypted) for C3 sending.");
  }
#endif

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

  s_initialized = true;
  return ESP_OK;
}

bool hub_comm_is_paired(void) { return s_is_paired; }

bool hub_comm_pair(uint32_t timeout_ms) {
  ESP_LOGI(TAG_HUB_COMM, "Avvio pairing in ricezione passiva %s...", 
           (timeout_ms == portMAX_DELAY) ? "(attesa infinita)" : "");

  uint32_t elapsed_ms = 0;
  const uint32_t wait_interval_ms = 1000;

  uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  esp_now_packet_t req_packet = {.type = MSG_TYPE_PAIR};
  // Il Sensor usa il proprio MAC nel payload o zero, non importa, il Gateway usa src_mac

  while (timeout_ms == portMAX_DELAY || elapsed_ms < timeout_ms) {
    // Clear BEFORE sending so we don't wipe out a legitimately-set bit
    // that arrives between esp_now_send and the wait.
    xEventGroupClearBits(s_espnow_event_group, EVENT_RECV_PAIR);

    if (elapsed_ms % 5000 == 0) { // Invia il broadcast ogni 5 secondi
      ESP_LOGI(TAG_HUB_COMM, "Inviando richiesta di pairing al Gateway (Broadcast)...");
      esp_now_send(bcast_mac, (uint8_t *)&req_packet, sizeof(req_packet));
    }

    EventBits_t bits =
        xEventGroupWaitBits(s_espnow_event_group, EVENT_RECV_PAIR, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(wait_interval_ms));

    if (bits & EVENT_RECV_PAIR) {
      memcpy(s_hub_mac, s_last_recv_mac, ESP_NOW_ETH_ALEN);
      secure_store_write(NVS_KEY_HUB_MAC, s_hub_mac, ESP_NOW_ETH_ALEN);

      s_is_paired = true;
      add_hub_peer(s_hub_mac); 
      
      esp_now_packet_t ack_packet = {.type = MSG_TYPE_PAIR_ACK};
      esp_now_send(s_hub_mac, (uint8_t *)&ack_packet, sizeof(ack_packet.type));

      ESP_LOGI(TAG_HUB_COMM, "Pairing avvenuto con successo e ACK inviato in chiaro.");

      // Ora che abbiamo mandato l'ACK, possiamo "promuovere" la connessione con il Gateway a cifrata
      esp_now_peer_info_t secure_peer = {0};
      secure_peer.channel = WIFI_CHANNEL;
#ifndef C3_BUILD
      secure_peer.ifidx = WIFI_IF_STA;
#else
      secure_peer.ifidx = WIFI_IF_AP;
#endif
      secure_peer.encrypt = true;
      memcpy(secure_peer.peer_addr, s_last_recv_mac, 6);
      memcpy(secure_peer.lmk, s_esp_now_lmk, 16);
      esp_now_mod_peer(&secure_peer);

      ESP_LOGI(TAG_HUB_COMM, "Gateway registrato come peer cifrato.");

      return true;
    }

    elapsed_ms += wait_interval_ms;
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