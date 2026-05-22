#ifndef CONFIG_H
#define CONFIG_H

/* Hardware Configuration */
#define BUZZER_PIN 23

/* MQTT API Configuration (Cloud)
 * CLOUD_MQTT_BROKER_IP must match the CN in the TLS server cert.
 * Run setup_network.py from the project root to auto-detect your LAN IP,
 * regenerate certs, and patch this value in one step. */
#define CLOUD_MQTT_BROKER_IP "10.121.0.111"
#define CLOUD_MQTT_URI        "mqtts://" CLOUD_MQTT_BROKER_IP ":8883"
#include "secrets.h"

#define CLOUD_MQTT_EDGE_ID        "esp32-01"
#define CLOUD_MQTT_TOPIC_COMMAND  "knockknock/edge/" CLOUD_MQTT_EDGE_ID "/command"
#define CLOUD_MQTT_TOPIC_RESPONSE "knockknock/edge/" CLOUD_MQTT_EDGE_ID "/response"
#define CLOUD_MQTT_TOPIC_STATUS   "knockknock/edge/" CLOUD_MQTT_EDGE_ID "/status"

/* UART Configuration for Serial Bridge (Hub <-> Gateway) */
#define HUB_UART_PORT   1
#define HUB_UART_TX_PIN 17
#define HUB_UART_RX_PIN 16
#define HUB_UART_BAUD   921600

#endif /* CONFIG_H */
