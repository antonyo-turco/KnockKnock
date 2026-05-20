#ifndef SECRETS_H
#define SECRETS_H

/* MQTT API Configuration (Cloud) */
#define CLOUD_MQTT_USERNAME "testuser"
#define CLOUD_MQTT_PASSWORD "testpass"

/** NVS Keys for storing/loading security materials */
#define NVS_KEY_SERIAL_AES "serial_aes_key"

/** Secret Key for Secure UART/Serial link between Gateway and Hub. */
#define DEFAULT_SERIAL_KEY "ser1234567890123"

#endif /* SECRETS_H */
