#ifndef ESP_NOW_COMM_H
#define ESP_NOW_COMM_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Define max failures before channel hopping
#define MAX_ESP_NOW_FAILURES 5

/**
 * @brief Initialize ESP-NOW communication.
 *        This initializes Wi-Fi (if not already), sets the PMK/LMK for security,
 *        and registers the peer (gateway). It also loads the last known channel
 *        from secure_store, or uses the default.
 * @param gateway_mac The MAC address of the receiver (6 bytes).
 * @param default_channel The default Wi-Fi channel (1-13) to use if none is saved.
 * @return ESP_OK on success.
 */
esp_err_t esp_now_comm_init(const uint8_t *gateway_mac, uint8_t default_channel);

/**
 * @brief Send data to the registered gateway via ESP-NOW.
 *        This function is blocking and waits for the ACK.
 *        If ACK fails for MAX_ESP_NOW_FAILURES consecutive times,
 *        it will automatically start scanning channels (1-13) and save the
 *        new working channel to secure_store.
 * @param data Pointer to the payload.
 * @param len Length of the payload (max 250 bytes).
 * @return ESP_OK if the packet was sent AND acknowledged by the peer.
 */
esp_err_t esp_now_comm_send(const uint8_t *data, size_t len);

#endif // ESP_NOW_COMM_H
