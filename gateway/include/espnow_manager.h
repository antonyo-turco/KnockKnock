/**
 * @file espnow_manager.h
 * @brief ESP-NOW manager for the KnockKnock gateway.
 *
 * Manages Wi-Fi/ESP-NOW initialization, peer registration, sending and
 * receiving encrypted ESP-NOW frames from multiple sensors.
 */

#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Shared protocol definitions (must match the sensor firmware)              */
/* -------------------------------------------------------------------------- */

/** Wi-Fi channel used by all devices in the network. */
#define GATEWAY_WIFI_CHANNEL 1

/** Maximum number of sensors that can be paired simultaneously. */
#define ESPNOW_MAX_PEERS 10

/** Message type identifiers – must match hub_communication.h on the sensor. */
typedef enum {
    MSG_TYPE_PAIR      = 0x01, /**< Pairing request from sensor.           */
    MSG_TYPE_ALARM     = 0x02, /**< Alarm notification from sensor.        */
    MSG_TYPE_INFO_REQ  = 0x03, /**< Information request from sensor.       */
    MSG_TYPE_INFO_RESP = 0x04, /**< Information response sent to sensor.   */
    MSG_TYPE_PAIR_ACK  = 0x05, /**< Pairing acknowledgment from sensor.    */
} gw_msg_type_t;

/**
 * @brief Wire-level ESP-NOW packet layout (packed, no padding).
 *        Must be identical to esp_now_packet_t in the sensor firmware.
 */
typedef struct __attribute__((packed)) {
    uint8_t type; /**< One of gw_msg_type_t. */
    union {
        uint8_t alarm_code; /**< Payload for MSG_TYPE_ALARM.    */
        struct {
            uint32_t timestamp;
            uint8_t  do_ml_training;
            uint32_t ml_duration_ms;
            uint8_t  do_reset;
        } info_resp;        /**< Payload for MSG_TYPE_INFO_RESP. */
        struct {
            uint8_t target_mac[6];
        } pair_req;         /**< Payload for MSG_TYPE_PAIR. */
    } payload;
} gw_espnow_packet_t;

/* -------------------------------------------------------------------------- */
/*  Callback type                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Callback invoked from the ESP-NOW receive ISR context.
 *
 * @warning Do NOT call any blocking ESP-IDF function inside this callback.
 *          Post a message to a queue instead.
 *
 * @param src_mac  6-byte MAC address of the sending sensor.
 * @param packet   Pointer to the received packet (valid only during callback).
 * @param len      Length of the received packet in bytes.
 */
typedef void (*espnow_recv_cb_t)(const uint8_t *src_mac,
                                 const gw_espnow_packet_t *packet,
                                 int len);

/* -------------------------------------------------------------------------- */
/*  API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize Wi-Fi in STA mode and ESP-NOW.
 *
 * Loads PMK/LMK from the secure NVS partition.  Falls back to the compile-time
 * defaults if the keys are not yet provisioned, and persists them.
 *
 * @param recv_cb  Callback invoked whenever a frame is received (may be NULL).
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t espnow_manager_init(espnow_recv_cb_t recv_cb);

/**
 * @brief Register a sensor as an encrypted peer.
 *
 * Idempotent: calling it again for an already-registered MAC is a no-op.
 *
 * @param mac  6-byte MAC address of the sensor.
 * @return ESP_OK on success.
 */
esp_err_t espnow_manager_add_peer(const uint8_t *mac);
esp_err_t espnow_manager_pair_peer(const uint8_t *mac);
esp_err_t espnow_manager_send_pairing_req(const uint8_t *mac);
bool espnow_manager_is_pairing_active_for(const uint8_t *mac);
void espnow_manager_cancel_pairing(const uint8_t *mac);

/**
 * @brief Check whether a MAC is already registered as a peer.
 *
 * @param mac  6-byte MAC address.
 * @return true if registered, false otherwise.
 */
bool espnow_manager_is_peer(const uint8_t *mac);

/**
 * @brief Send an ESP-NOW packet to a specific sensor.
 *
 * Blocks until the MAC-layer ACK is received or the timeout elapses.
 *
 * @param dest_mac    6-byte destination MAC address.
 * @param packet      Pointer to the packet to transmit.
 * @param len         Number of bytes to transmit.
 * @param timeout_ms  Maximum time to wait for the send ACK.
 * @return true if the ACK was received, false on timeout or error.
 */
bool espnow_manager_send(const uint8_t *dest_mac,
                         const gw_espnow_packet_t *packet,
                         size_t len,
                         uint32_t timeout_ms);

/**
 * @brief De-initialize ESP-NOW and release Wi-Fi resources.
 */
void espnow_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_MANAGER_H */
