/**
 * @file serial_bridge.h
 * @brief Secure UART bridge between the gateway and the Hub.
 *
 * All frames exchanged over the serial link are encrypted with AES-128-GCM,
 * leveraging the ESP32-C3 hardware cryptographic accelerator via mbedTLS.
 *
 * Frame format (before encryption / after decryption):
 *
 *   [ MAGIC (2B) | LEN (2B, LE) | TYPE (1B) | PAYLOAD (N bytes) ]
 *
 * Wire frame (after encryption):
 *
 *   [ MAGIC (2B) | LEN (2B, LE) | IV (12B) | CIPHERTEXT (N+1 bytes) | TAG (16B) ]
 *
 * The TYPE byte is included in the CIPHERTEXT so that it is authenticated and
 * encrypted together with the payload.
 */

#ifndef SERIAL_BRIDGE_H
#define SERIAL_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Frame type identifiers (Hub ↔ Gateway application-level protocol)        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Messages sent FROM the Gateway TO the Hub.
 */
typedef enum {
    GW_TO_HUB_ALARM     = 0x10, /**< Forwarded alarm from a sensor.           */
    GW_TO_HUB_PAIR_NOTIF = 0x11, /**< A new sensor has paired with the gateway. */
    GW_TO_HUB_STATUS    = 0x12, /**< Periodic gateway heartbeat / status.     */
} gw_to_hub_msg_t;

/**
 * @brief Messages sent FROM the Hub TO the Gateway.
 */
typedef enum {
    HUB_TO_GW_INFO_RESP  = 0x20, /**< Info response to forward to a sensor.   */
    HUB_TO_GW_UNPAIR     = 0x21, /**< Instruct the gateway to drop a peer.    */
    HUB_TO_GW_REKEY      = 0x22, /**< Deliver new LMK/PMK to the gateway.     */
} hub_to_gw_msg_t;

/* -------------------------------------------------------------------------- */
/*  Payload structures                                                         */
/* -------------------------------------------------------------------------- */

/** Alarm notification payload (Gateway → Hub). */
typedef struct __attribute__((packed)) {
    uint8_t  sensor_mac[6]; /**< MAC of the sensor that triggered the alarm. */
    uint8_t  alarm_code;    /**< Alarm severity / code.                      */
} sb_alarm_payload_t;

/** Pair notification payload (Gateway → Hub). */
typedef struct __attribute__((packed)) {
    uint8_t sensor_mac[6]; /**< MAC of the newly paired sensor.              */
} sb_pair_notif_payload_t;

/** Info response payload (Hub → Gateway → Sensor). */
typedef struct __attribute__((packed)) {
    uint8_t  sensor_mac[6];    /**< Target sensor MAC.                       */
    uint32_t timestamp;        /**< UNIX time.                               */
    uint8_t  do_ml_training;   /**< Request ML training session.             */
    uint32_t ml_duration_ms;   /**< Duration of the ML session.              */
    uint8_t  do_reset;         /**< Request sensor reboot.                   */
} sb_info_resp_payload_t;

/** Re-key payload (Hub → Gateway). */
typedef struct __attribute__((packed)) {
    uint8_t new_pmk[16]; /**< New ESP-NOW Primary Master Key.                */
    uint8_t new_lmk[16]; /**< New ESP-NOW Local Master Key.                  */
} sb_rekey_payload_t;

/** Unpair payload (Hub → Gateway). */
typedef struct __attribute__((packed)) {
    uint8_t sensor_mac[6]; /**< Sensor to remove from the peer list.         */
} sb_unpair_payload_t;

/** Gateway status heartbeat payload (Gateway → Hub). */
typedef struct __attribute__((packed)) {
    uint8_t  num_peers;       /**< Current number of paired sensors.         */
    uint32_t uptime_s;        /**< Gateway uptime in seconds.                */
} sb_status_payload_t;

/* -------------------------------------------------------------------------- */
/*  Callbacks                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Called when a valid, authenticated frame arrives from the Hub.
 *
 * @param msg_type  One of hub_to_gw_msg_t.
 * @param payload   Pointer to the decrypted payload (valid only during cb).
 * @param len       Payload length in bytes.
 */
typedef void (*serial_bridge_recv_cb_t)(uint8_t        msg_type,
                                        const uint8_t *payload,
                                        size_t         len);

/* -------------------------------------------------------------------------- */
/*  Configuration                                                              */
/* -------------------------------------------------------------------------- */

/** Serial bridge initialisation parameters. */
typedef struct {
    int      uart_port;   /**< UART port number (e.g. UART_NUM_1).           */
    int      tx_pin;      /**< GPIO pin for UART TX.                         */
    int      rx_pin;      /**< GPIO pin for UART RX.                         */
    int      baud_rate;   /**< Baud rate (e.g. 115200 or 921600).            */
    serial_bridge_recv_cb_t recv_cb; /**< Callback for frames from the Hub.  */
} serial_bridge_config_t;

/* -------------------------------------------------------------------------- */
/*  API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialise the serial bridge.
 *
 * Configures the UART driver, loads or generates the AES-128 key from secure
 * NVS, and starts the receive task.
 *
 * @param config  Pointer to initialisation parameters.
 * @return ESP_OK on success.
 */
esp_err_t serial_bridge_init(const serial_bridge_config_t *config);

/**
 * @brief Send an encrypted frame to the Hub.
 *
 * Internally generates a fresh 12-byte random IV for every frame and appends
 * the 16-byte GCM authentication tag.
 *
 * @param msg_type  One of gw_to_hub_msg_t.
 * @param payload   Pointer to the plaintext payload.
 * @param len       Payload length in bytes.
 * @return ESP_OK on success.
 */
esp_err_t serial_bridge_send(uint8_t        msg_type,
                             const uint8_t *payload,
                             size_t         len);

/**
 * @brief De-initialise the serial bridge and free resources.
 */
void serial_bridge_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_BRIDGE_H */
