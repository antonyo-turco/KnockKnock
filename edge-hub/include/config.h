#ifndef CONFIG_H
#define CONFIG_H

/* Hardware Configuration */
#define BUZZER_PIN 25

/* MQTT API Configuration (Cloud) */
#define CLOUD_MQTT_URI "mqtt://test.mosquitto.org:1883"
#define CLOUD_MQTT_USERNAME "testuser"
#define CLOUD_MQTT_PASSWORD "testpass"

/* UART Configuration for Serial Bridge (Hub <-> Gateway) */
#define HUB_UART_PORT   1
#define HUB_UART_TX_PIN 17
#define HUB_UART_RX_PIN 16
#define HUB_UART_BAUD   921600

#endif /* CONFIG_H */
