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
    return; // Pacchetto troppo piccolo

  // Recupera il MAC address dalla nuova struct
  const uint8_t *mac_addr = esp_now_info->src_addr;

  esp_now_packet_t *packet = (esp_now_packet_t *)data;

  // Salva il MAC del mittente
  memcpy(s_last_recv_mac, mac_addr, ESP_NOW_ETH_ALEN);

  if (packet->type == MSG_TYPE_PAIR) {
    xEventGroupSetBits(s_espnow_event_group, EVENT_RECV_PAIR);
  } else if (packet->type == MSG_TYPE_INFO_RESP) {
    // Copia il payload per poterlo leggere nel task principale
    memcpy(&s_last_recv_packet, packet, len);
    xEventGroupSetBits(s_espnow_event_group, EVENT_RECV_INFO);
  }
}

// --- FUNZIONI DI UTILITA' INTERNE ---

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

// --- IMPLEMENTAZIONE API PUBBLICHE ---

esp_err_t hub_comm_init(void) {
  s_espnow_event_group = xEventGroupCreate();

  // 1. Inizializza NVS in modo sicuro
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

  // 2. Inizializza Wi-Fi in modalità Station
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Imposta il canale Wi-Fi (fondamentale per ESP-NOW)
  ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

  // 3. Inizializza ESP-NOW
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)on_data_sent));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

  // Imposta la Primary Master Key (PMK) caricata dall'NVS
  ESP_ERROR_CHECK(esp_now_set_pmk(s_esp_now_pmk));

  // 4. Carica il MAC da NVS
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

  // Aspetta finché non riceve un pacchetto MSG_TYPE_PAIR (dal Gateway) o va in
  // timeout
  EventBits_t bits =
      xEventGroupWaitBits(s_espnow_event_group, EVENT_RECV_PAIR, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));

  esp_now_del_peer(bcast_mac);

  if (bits & EVENT_RECV_PAIR) {
    // Salviamo il MAC in modo sicuro
    memcpy(s_hub_mac, s_last_recv_mac, ESP_NOW_ETH_ALEN);
    secure_store_write(NVS_KEY_HUB_MAC, s_hub_mac, ESP_NOW_ETH_ALEN);

    s_is_paired = true;
    add_hub_peer(s_hub_mac); // Aggiunge come peer cifrato
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

    // Aspetta ACK (livello MAC)
    EventBits_t bits = xEventGroupWaitBits(s_espnow_event_group,
                                           EVENT_SEND_SUCCESS | EVENT_SEND_FAIL,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(100));

    if (bits & EVENT_SEND_SUCCESS) {
      ESP_LOGI(TAG_HUB_COMM, "Allarme inviato con successo (ACK ricevuto).");
      return true;
    }

    ESP_LOGW(TAG_HUB_COMM, "Fallimento invio allarme, ritento... (%d/%d)",
             i + 1, max_retries);
    // Piccolo backoff prima di ritentare
    vTaskDelay(pdMS_TO_TICKS(10 + (i * 10)));
  }

  ESP_LOGE(TAG_HUB_COMM, "Impossibile inviare l'allarme dopo %d tentativi.",
           max_retries);
  return false;
}

bool hub_comm_get_information(hub_info_t *info, uint32_t timeout_ms,
                              uint8_t max_retries) {
  if (!s_is_paired || !info)
    return false;

  esp_now_packet_t req_packet = {.type = MSG_TYPE_INFO_REQ};

  for (uint8_t i = 0; i < max_retries; i++) {
    // Pulisce i flag prima di ogni tentativo
    xEventGroupClearBits(s_espnow_event_group, EVENT_RECV_INFO |
                                                   EVENT_SEND_SUCCESS |
                                                   EVENT_SEND_FAIL);

    // 1. Tenta l'invio
    esp_err_t err = esp_now_send(s_hub_mac, (uint8_t *)&req_packet,
                                 sizeof(req_packet.type));
    if (err != ESP_OK) {
      ESP_LOGW(TAG_HUB_COMM, "Errore interno ESP-NOW, ritento... (%d/%d)",
               i + 1, max_retries);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // 2. Attende l'ACK fisico (il pacchetto ha raggiunto l'antenna dell'Hub?)
    // Diamo un timeout molto breve (es. 50ms) per l'ACK di livello MAC
    EventBits_t send_bits = xEventGroupWaitBits(
        s_espnow_event_group, EVENT_SEND_SUCCESS | EVENT_SEND_FAIL, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(50));

    if (!(send_bits & EVENT_SEND_SUCCESS)) {
      ESP_LOGW(TAG_HUB_COMM, "Tentativo %d: ACK non ricevuto dall'Hub.", i + 1);
      vTaskDelay(
          pdMS_TO_TICKS(10 + (i * 10))); // Backoff incrementale (10ms, 20ms...)
      continue;                          // Salta il resto e riprova
    }

    // 3. ACK ricevuto! Ora attende la risposta applicativa con le informazioni
    EventBits_t recv_bits =
        xEventGroupWaitBits(s_espnow_event_group, EVENT_RECV_INFO, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (recv_bits & EVENT_RECV_INFO) {
      // Successo totale: popola la struttura per l'utente
      info->timestamp = s_last_recv_packet.payload.info_resp.timestamp;
      info->do_ml_training =
          s_last_recv_packet.payload.info_resp.do_ml_training;
      info->ml_duration_ms =
          s_last_recv_packet.payload.info_resp.ml_duration_ms;
      info->do_reset = s_last_recv_packet.payload.info_resp.do_reset;

      // --- INIZIO SINCRONIZZAZIONE AUTOMATICA OROLOGIO ---
      // Un timestamp > 1000000000 assicura che sia una data valida (successiva
      // al 2001)
      if (info->timestamp > 1000000000) {
        // Imposta il fuso orario italiano
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();

        // Aggiorna l'orologio di sistema (RTC)
        struct timeval tv;
        tv.tv_sec = info->timestamp;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        ESP_LOGI(TAG_HUB_COMM, "Orologio sincronizzato internamente a: %lu",
                 info->timestamp);
      } else {
        ESP_LOGW(
            TAG_HUB_COMM,
            "Timestamp ricevuto non valido (%lu), orologio non aggiornato.",
            info->timestamp);
      }
      // --- FINE SINCRONIZZAZIONE AUTOMATICA OROLOGIO ---

      ESP_LOGI(TAG_HUB_COMM, "Info ricevute con successo al tentativo %d",
               i + 1);
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