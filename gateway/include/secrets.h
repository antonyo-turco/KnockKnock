#ifndef SECRETS_H
#define SECRETS_H

/** NVS Keys for storing/loading security materials */
#define NVS_KEY_PMK        "esp_now_pmk"
#define NVS_KEY_LMK        "esp_now_lmk"
#define NVS_KEY_SERIAL_AES "serial_aes_key"

/** Compile-time fallback keys – 16 bytes each. Replace in production. */
#define DEFAULT_PMK "pmk1234567890123"
#define DEFAULT_LMK "lmk1234567890123"

/** Secret Key for Secure UART/Serial link between Gateway and Hub. */
#define DEFAULT_SERIAL_KEY "ser1234567890123"

#endif /* SECRETS_H */
