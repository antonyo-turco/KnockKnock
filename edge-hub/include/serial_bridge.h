#ifndef SERIAL_BRIDGE_H
#define SERIAL_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message Types */
typedef enum {
    GW_TO_HUB_ALARM      = 0x10,
    GW_TO_HUB_PAIR_NOTIF = 0x11,
    GW_TO_HUB_STATUS     = 0x12,
} gw_to_hub_msg_t;

typedef enum {
    HUB_TO_GW_INFO_RESP  = 0x20,
    HUB_TO_GW_UNPAIR     = 0x21,
    HUB_TO_GW_REKEY      = 0x22,
} hub_to_gw_msg_t;

/* Payloads */
typedef struct __attribute__((packed)) {
    uint8_t  sensor_mac[6];
    uint8_t  alarm_code;
} sb_alarm_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t sensor_mac[6];
} sb_pair_notif_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t  sensor_mac[6];
    uint32_t timestamp;
    uint8_t  do_ml_training;
    uint32_t ml_duration_ms;
    uint8_t  do_reset;
} sb_info_resp_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t new_pmk[16];
    uint8_t new_lmk[16];
} sb_rekey_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t sensor_mac[6];
} sb_unpair_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t  num_peers;
    uint32_t uptime_s;
} sb_status_payload_t;

/* Callback Type */
typedef void (*serial_bridge_recv_cb_t)(uint8_t msg_type, const uint8_t *payload, size_t len);

typedef struct {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    serial_bridge_recv_cb_t recv_cb;
} serial_bridge_config_t;

esp_err_t serial_bridge_init(const serial_bridge_config_t *config);
esp_err_t serial_bridge_send(uint8_t msg_type, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_BRIDGE_H */
