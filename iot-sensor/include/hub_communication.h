#ifndef HUB_COMMUNICATION_H
#define HUB_COMMUNICATION_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "secrets.h"
#define WIFI_CHANNEL 1

// Type of message used
typedef enum {
  MSG_TYPE_PAIR = 0x01,
  MSG_TYPE_ALARM = 0x02,
  MSG_TYPE_INFO_REQ = 0x03,
  MSG_TYPE_INFO_RESP = 0x04
} msg_type_t;

// Structure for receive information from Hub
typedef struct {
  uint32_t timestamp;      // Data e ora (UNIX time)
  bool do_ml_training;     // If we need to do machine learning
  uint32_t ml_duration_ms; // Duration of machine learning
  bool do_reset;           // If the sensor needs to restart
} hub_info_t;

// Base structure of the packet sent/received (packed to avoid padding)
typedef struct __attribute__((packed)) {
  uint8_t type; // Use msg_type_t
  union {
    uint8_t alarm_code; // For MSG_TYPE_ALARM
    struct {
      uint32_t timestamp;
      uint8_t do_ml_training;
      uint32_t ml_duration_ms;
      uint8_t do_reset;
    } info_resp; // For MSG_TYPE_INFO_RESP
  } payload;
} esp_now_packet_t;

/**
 * @brief Initialize NVS, Wi-Fi and ESP-NOW. Load the Hub MAC if it exists.
 */
esp_err_t hub_comm_init(void);

/**
 * @brief Check if the sensor has an associated Hub in memory
 */
bool hub_comm_is_paired(void);

/**
 * @brief Turn on the receiver and wait for a pairing message from the Hub.
 *        Salva il MAC nell'NVS in caso di successo.
 * @param timeout_ms Maximum waiting time.
 */
bool hub_comm_pair(uint32_t timeout_ms);

/**
 * @brief Send an alarm to the Hub, with retransmission in case of ACK failure.
 * @param alarm_code Alarm code
 * @param max_retries Maximum number of attempts
 */
bool hub_comm_send_alarm(uint8_t alarm_code, uint8_t max_retries);

/**
 * @brief Request information from the Hub and wait for the response, with
 * retries.
 * @param info Pointer to the structure to be populated.
 * @param timeout_ms Maximum waiting time for the single response.
 * @param max_retries Maximum number of attempts in case of failure.
 */
bool hub_comm_get_information(hub_info_t *info, uint32_t timeout_ms,
                              uint8_t max_retries);

#ifdef __cplusplus
}
#endif

#endif // HUB_COMMUNICATION_H