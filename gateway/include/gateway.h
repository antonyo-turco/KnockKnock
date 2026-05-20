/**
 * @file gateway.h
 * @brief Gateway orchestrator – public interface.
 *
 * The gateway orchestrator ties together the ESP-NOW manager and the serial
 * bridge: it forwards alarms and pair notifications from sensors to the Hub,
 * and dispatches info-response and control frames from the Hub to the correct
 * sensor over ESP-NOW.
 */

#ifndef GATEWAY_H
#define GATEWAY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise and start the gateway.
 *
 * Must be called once from app_main after the NVS secure store is ready.
 * This function starts the internal FreeRTOS tasks and does not return.
 *
 * @return ESP_OK on success.
 */
esp_err_t gateway_start(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_H */
