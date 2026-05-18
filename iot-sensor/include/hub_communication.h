#ifndef HUB_COMMUNICATION_H
#define HUB_COMMUNICATION_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configurazioni di sicurezza (Modifica con chiavi a 16 byte reali)
#define DEFAULT_ESP_NOW_PMK "pmk1234567890123"
#define DEFAULT_ESP_NOW_LMK "lmk1234567890123"
#define WIFI_CHANNEL 1

// Tipi di messaggi per multiplexing
typedef enum {
    MSG_TYPE_PAIR = 0x01,
    MSG_TYPE_ALARM = 0x02,
    MSG_TYPE_INFO_REQ = 0x03,
    MSG_TYPE_INFO_RESP = 0x04
} msg_type_t;

// Struttura per ricevere le informazioni dall'Hub
typedef struct {
    uint32_t timestamp;      // Data e ora (UNIX time)
    bool do_ml_training;     // Se effettuare machine learning
    uint32_t ml_duration_ms; // Durata del machine learning
    bool do_reset;           // Se il sensore deve riavviarsi
} hub_info_t;

// Struttura base del pacchetto inviato/ricevuto (packed per evitare padding)
typedef struct __attribute__((packed)) {
    uint8_t type; // Usa msg_type_t
    union {
        uint8_t alarm_code; // Per MSG_TYPE_ALARM (es. livello di gravità)
        struct {
            uint32_t timestamp;
            uint8_t do_ml_training;
            uint32_t ml_duration_ms;
            uint8_t do_reset;
        } info_resp; // Per MSG_TYPE_INFO_RESP
    } payload;
} esp_now_packet_t;


/**
 * @brief Inizializza NVS, Wi-Fi e ESP-NOW. Carica il MAC dell'Hub se esiste.
 */
esp_err_t hub_comm_init(void);

/**
 * @brief Verifica se il sensore ha già un Hub associato in memoria
 */
bool hub_comm_is_paired(void);

/**
 * @brief Accende la ricezione e attende un messaggio di pairing dall'Hub.
 *        Salva il MAC nell'NVS in caso di successo.
 * @param timeout_ms Tempo massimo di attesa.
 */
bool hub_comm_pair(uint32_t timeout_ms);

/**
 * @brief Invia un allarme all'Hub, con ritrasmissione in caso di fallimento ACK.
 * @param alarm_code Codice dell'allarme
 * @param max_retries Numero massimo di tentativi
 */
bool hub_comm_send_alarm(uint8_t alarm_code, uint8_t max_retries);

/**
 * @brief Richiede info all'Hub e attende la risposta, con ritentativi.
 * @param info Puntatore alla struttura da popolare.
 * @param timeout_ms Tempo massimo di attesa per la singola risposta.
 * @param max_retries Numero massimo di tentativi in caso di fallimento.
 */
bool hub_comm_get_information(hub_info_t *info, uint32_t timeout_ms, uint8_t max_retries);

#ifdef __cplusplus
}
#endif

#endif // HUB_COMMUNICATION_H