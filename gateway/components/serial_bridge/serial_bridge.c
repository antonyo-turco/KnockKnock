/**
 * @file serial_bridge.c
 * @brief Secure UART bridge implementation for the KnockKnock gateway.
 *
 * Security design:
 *   - Symmetric key:  AES-128 (16-byte key) stored in the encrypted NVS
 *                     partition.  Generated on first boot using the ESP32
 *                     hardware RNG and persisted; never leaves the device in
 *                     plaintext.
 *   - Cipher mode:    AES-128-GCM (AEAD) – provides both confidentiality and
 *                     authenticity.  The ESP32-C3 hardware AES/GCM accelerator
 *                     is used transparently through mbedTLS when
 *                     CONFIG_MBEDTLS_HARDWARE_AES is enabled in sdkconfig.
 *   - IV/Nonce:       12 bytes, generated fresh per frame from the hardware RNG
 *                     (esp_random()).  Transmitted in plaintext at the start of
 *                     each wire frame.
 *   - AAD:            The 5-byte frame header (MAGIC + LEN + TYPE) is used as
 *                     Additional Authenticated Data (AAD) – it is NOT encrypted
 *                     but IS authenticated, preventing header tampering.
 *   - Auth tag:       16 bytes (GCM default), appended after the ciphertext.
 *
 * Wire frame layout:
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       2     MAGIC  (0xBE, 0xEF)
 *   2       2     PAYLOAD_LEN  (little-endian, covers ciphertext only)
 *   4       1     MSG_TYPE  (plaintext – authenticated via AAD)
 *   5       12    IV / Nonce
 *   17      N     CIPHERTEXT  (AES-128-GCM encrypted payload)
 *   17+N    16    GCM AUTH TAG
 *
 * Total overhead per frame = 2 + 2 + 1 + 12 + 16 = 33 bytes.
 */

#include "serial_bridge.h"
#include "secure_store.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/gcm.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "SERIAL_BRIDGE";

/* -------------------------------------------------------------------------- */
/*  Frame protocol constants                                                   */
/* -------------------------------------------------------------------------- */

#define FRAME_MAGIC_0  0xBEu
#define FRAME_MAGIC_1  0xEFu

#define FRAME_MAGIC_SIZE      2u
#define FRAME_LEN_SIZE        2u
#define FRAME_TYPE_SIZE       1u
#define FRAME_IV_SIZE         12u
#define FRAME_TAG_SIZE        16u

/** Byte offset of each field in the wire frame. */
#define OFFSET_MAGIC          0u
#define OFFSET_LEN            2u
#define OFFSET_TYPE           4u
#define OFFSET_IV             5u
#define OFFSET_CIPHERTEXT     17u  /* OFFSET_IV + FRAME_IV_SIZE */

/** Header = MAGIC + LEN + TYPE (used as AAD). */
#define FRAME_HEADER_SIZE     (FRAME_MAGIC_SIZE + FRAME_LEN_SIZE + FRAME_TYPE_SIZE)

/** Maximum plaintext payload this driver will accept/produce (bytes). */
#define MAX_PAYLOAD_SIZE      240u

/** Maximum wire frame size. */
#define MAX_FRAME_SIZE        (FRAME_HEADER_SIZE + FRAME_IV_SIZE + MAX_PAYLOAD_SIZE + FRAME_TAG_SIZE)

/* -------------------------------------------------------------------------- */
/*  UART configuration                                                         */
/* -------------------------------------------------------------------------- */

#define UART_RX_BUF_SIZE   1024u
#define UART_TX_BUF_SIZE   0u    /* 0 = synchronous TX */
#define UART_QUEUE_SIZE    10u
#define RX_TASK_STACK_SIZE 4096u
#define RX_TASK_PRIORITY   5u

/** NVS key for the AES-128 serial link key (16 raw bytes). */
#define NVS_KEY_SERIAL_AES "serial_aes_key"

/* -------------------------------------------------------------------------- */
/*  Internal state                                                             */
/* -------------------------------------------------------------------------- */

static uart_port_t          s_uart_port = UART_NUM_1;
static serial_bridge_recv_cb_t s_recv_cb = NULL;
static TaskHandle_t         s_rx_task_handle = NULL;
static bool                 s_initialised = false;

/** AES-128 key (16 bytes) loaded from / stored in encrypted NVS. */
static uint8_t s_aes_key[16];

/** mbedTLS GCM context (re-initialised per operation). */
static mbedtls_gcm_context s_gcm_ctx;

/* -------------------------------------------------------------------------- */
/*  Internal: key management                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Load the AES key from NVS, or generate and persist a new one.
 */
static esp_err_t load_or_generate_aes_key(void)
{
    uint8_t *stored_key = NULL;
    size_t   key_len    = 0;

    esp_err_t err = secure_store_read(NVS_KEY_SERIAL_AES, &stored_key, &key_len);
    if (err == ESP_OK && key_len == sizeof(s_aes_key)) {
        memcpy(s_aes_key, stored_key, sizeof(s_aes_key));
        free(stored_key);
        ESP_LOGI(TAG, "AES-128 serial key loaded from secure NVS.");
        return ESP_OK;
    }

    if (stored_key) free(stored_key);

    /* Generate a new 128-bit key from the hardware RNG. */
    ESP_LOGW(TAG, "Generating new AES-128 serial key via HW RNG...");
    for (int i = 0; i < (int)sizeof(s_aes_key); i += 4) {
        uint32_t rnd = esp_random();
        memcpy(&s_aes_key[i], &rnd, 4);
    }

    err = secure_store_write(NVS_KEY_SERIAL_AES, s_aes_key, sizeof(s_aes_key));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist AES key: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "New AES-128 serial key generated and persisted.");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Internal: AES-128-GCM encryption / decryption                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Encrypt plaintext with AES-128-GCM.
 *
 * @param iv           12-byte random IV (caller provides).
 * @param aad          Additional authenticated data.
 * @param aad_len      AAD length in bytes.
 * @param plaintext    Input plaintext.
 * @param pt_len       Plaintext length.
 * @param ciphertext   Output buffer (must be at least pt_len bytes).
 * @param tag          Output buffer for the 16-byte auth tag.
 * @return 0 on success, non-zero mbedTLS error code otherwise.
 */
static int aes_gcm_encrypt(const uint8_t *iv,
                            const uint8_t *aad,    size_t aad_len,
                            const uint8_t *plaintext, size_t pt_len,
                            uint8_t       *ciphertext,
                            uint8_t       *tag)
{
    mbedtls_gcm_init(&s_gcm_ctx);

    int ret = mbedtls_gcm_setkey(&s_gcm_ctx, MBEDTLS_CIPHER_ID_AES,
                                  s_aes_key, 128);
    if (ret != 0) goto cleanup;

    ret = mbedtls_gcm_crypt_and_tag(&s_gcm_ctx,
                                     MBEDTLS_GCM_ENCRYPT,
                                     pt_len,
                                     iv,    FRAME_IV_SIZE,
                                     aad,   aad_len,
                                     plaintext,
                                     ciphertext,
                                     FRAME_TAG_SIZE, tag);
cleanup:
    mbedtls_gcm_free(&s_gcm_ctx);
    return ret;
}

/**
 * @brief Decrypt ciphertext with AES-128-GCM and verify the auth tag.
 *
 * @param iv           12-byte IV (read from the wire frame).
 * @param aad          Additional authenticated data.
 * @param aad_len      AAD length in bytes.
 * @param ciphertext   Input ciphertext.
 * @param ct_len       Ciphertext length.
 * @param tag          16-byte auth tag to verify.
 * @param plaintext    Output plaintext buffer (must be at least ct_len bytes).
 * @return 0 on success, MBEDTLS_ERR_GCM_AUTH_FAILED on authentication error.
 */
static int aes_gcm_decrypt(const uint8_t *iv,
                            const uint8_t *aad,        size_t aad_len,
                            const uint8_t *ciphertext, size_t ct_len,
                            const uint8_t *tag,
                            uint8_t       *plaintext)
{
    mbedtls_gcm_init(&s_gcm_ctx);

    int ret = mbedtls_gcm_setkey(&s_gcm_ctx, MBEDTLS_CIPHER_ID_AES,
                                  s_aes_key, 128);
    if (ret != 0) goto cleanup;

    ret = mbedtls_gcm_auth_decrypt(&s_gcm_ctx,
                                    ct_len,
                                    iv,    FRAME_IV_SIZE,
                                    aad,   aad_len,
                                    tag,   FRAME_TAG_SIZE,
                                    ciphertext,
                                    plaintext);
cleanup:
    mbedtls_gcm_free(&s_gcm_ctx);
    return ret;
}

/* -------------------------------------------------------------------------- */
/*  Internal: frame parsing                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Try to parse and authenticate a complete wire frame.
 *
 * @param raw      Pointer to the raw frame buffer.
 * @param raw_len  Length of the raw frame.
 * @return true if the frame was valid and the callback was invoked.
 */
static bool parse_and_dispatch_frame(const uint8_t *raw, size_t raw_len)
{
    /* Minimum frame: header + IV + 0-byte payload + tag */
    const size_t MIN_FRAME = FRAME_HEADER_SIZE + FRAME_IV_SIZE + FRAME_TAG_SIZE;

    if (raw_len < MIN_FRAME) {
        ESP_LOGW(TAG, "Frame too short (%u bytes).", (unsigned)raw_len);
        return false;
    }

    /* Verify magic bytes. */
    if (raw[0] != FRAME_MAGIC_0 || raw[1] != FRAME_MAGIC_1) {
        ESP_LOGW(TAG, "Bad magic bytes: 0x%02X 0x%02X", raw[0], raw[1]);
        return false;
    }

    /* Decode the ciphertext length (little-endian). */
    uint16_t ct_len = (uint16_t)(raw[OFFSET_LEN] | (raw[OFFSET_LEN + 1] << 8));
    uint8_t  msg_type = raw[OFFSET_TYPE];

    size_t expected_total = FRAME_HEADER_SIZE + FRAME_IV_SIZE + ct_len + FRAME_TAG_SIZE;
    if (raw_len < expected_total) {
        ESP_LOGW(TAG, "Incomplete frame: expected %u got %u bytes.",
                 (unsigned)expected_total, (unsigned)raw_len);
        return false;
    }

    if (ct_len > MAX_PAYLOAD_SIZE) {
        ESP_LOGW(TAG, "Ciphertext length %u exceeds maximum.", ct_len);
        return false;
    }

    const uint8_t *iv         = raw + OFFSET_IV;
    const uint8_t *ciphertext = raw + OFFSET_CIPHERTEXT;
    const uint8_t *auth_tag   = ciphertext + ct_len;

    /* The 5-byte header is the AAD (authenticated, not encrypted). */
    const uint8_t *aad     = raw;
    size_t         aad_len = FRAME_HEADER_SIZE;

    uint8_t plaintext[MAX_PAYLOAD_SIZE];
    int ret = aes_gcm_decrypt(iv, aad, aad_len,
                               ciphertext, ct_len,
                               auth_tag,
                               plaintext);
    if (ret != 0) {
        ESP_LOGW(TAG, "AES-GCM authentication FAILED (ret=%d). Frame dropped.", ret);
        return false;
    }

    if (s_recv_cb != NULL) {
        s_recv_cb(msg_type, plaintext, ct_len);
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/*  RX task                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Receive task: reads raw bytes from UART, assembles frames, dispatches.
 *
 * State machine:
 *   WAIT_MAGIC_0 → WAIT_MAGIC_1 → READ_HEADER → READ_PAYLOAD → DISPATCH
 */
static void rx_task(void *arg)
{
    typedef enum {
        ST_WAIT_MAGIC_0,
        ST_WAIT_MAGIC_1,
        ST_READ_HEADER,
        ST_READ_PAYLOAD,
    } rx_state_t;

    static uint8_t frame_buf[MAX_FRAME_SIZE];
    rx_state_t state       = ST_WAIT_MAGIC_0;
    size_t     frame_pos   = 0;
    size_t     need_bytes  = 0;

    uint8_t byte_buf[1];

    while (true) {
        int bytes_read = uart_read_bytes(s_uart_port, byte_buf, 1,
                                         pdMS_TO_TICKS(20));
        if (bytes_read <= 0) continue;

        uint8_t b = byte_buf[0];

        switch (state) {
        case ST_WAIT_MAGIC_0:
            if (b == FRAME_MAGIC_0) {
                frame_pos = 0;
                frame_buf[frame_pos++] = b;
                state = ST_WAIT_MAGIC_1;
            }
            break;

        case ST_WAIT_MAGIC_1:
            if (b == FRAME_MAGIC_1) {
                frame_buf[frame_pos++] = b;
                /* Now collect: LEN (2B) + TYPE (1B) = 3 more bytes. */
                need_bytes = FRAME_LEN_SIZE + FRAME_TYPE_SIZE;
                state = ST_READ_HEADER;
            } else {
                /* False start – check if this is a new magic byte. */
                frame_pos = 0;
                if (b == FRAME_MAGIC_0) {
                    frame_buf[frame_pos++] = b;
                    state = ST_WAIT_MAGIC_1;
                } else {
                    state = ST_WAIT_MAGIC_0;
                }
            }
            break;

        case ST_READ_HEADER:
            frame_buf[frame_pos++] = b;
            need_bytes--;
            if (need_bytes == 0) {
                /* We have MAGIC + LEN + TYPE; decode payload size. */
                uint16_t ct_len = (uint16_t)(frame_buf[OFFSET_LEN] |
                                             (frame_buf[OFFSET_LEN + 1] << 8));
                /* IV + CIPHERTEXT + TAG */
                need_bytes = FRAME_IV_SIZE + ct_len + FRAME_TAG_SIZE;

                if (need_bytes > (MAX_FRAME_SIZE - FRAME_HEADER_SIZE)) {
                    ESP_LOGW(TAG, "Declared payload too large (%u), resetting.", ct_len);
                    state = ST_WAIT_MAGIC_0;
                    break;
                }
                state = ST_READ_PAYLOAD;
            }
            break;

        case ST_READ_PAYLOAD:
            frame_buf[frame_pos++] = b;
            need_bytes--;
            if (need_bytes == 0) {
                parse_and_dispatch_frame(frame_buf, frame_pos);
                state = ST_WAIT_MAGIC_0;
                frame_pos = 0;
            }
            break;
        }
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t serial_bridge_init(const serial_bridge_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (s_initialised) return ESP_ERR_INVALID_STATE;

    s_uart_port = (uart_port_t)config->uart_port;
    s_recv_cb   = config->recv_cb;

    /* ------------------------------------------------------------------ */
    /* 1. Load / generate the AES-128 key                                  */
    /* ------------------------------------------------------------------ */
    esp_err_t err = load_or_generate_aes_key();
    if (err != ESP_OK) return err;

    /* ------------------------------------------------------------------ */
    /* 2. Configure UART driver                                            */
    /* ------------------------------------------------------------------ */
    uart_config_t uart_cfg = {
        .baud_rate  = config->baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_driver_install(s_uart_port,
                              UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                              UART_QUEUE_SIZE,  NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(s_uart_port, &uart_cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(s_uart_port,
                       config->tx_pin, config->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    /* ------------------------------------------------------------------ */
    /* 3. Start receive task                                               */
    /* ------------------------------------------------------------------ */
    BaseType_t task_ok = xTaskCreate(rx_task, "serial_rx",
                                      RX_TASK_STACK_SIZE, NULL,
                                      RX_TASK_PRIORITY, &s_rx_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task.");
        return ESP_ERR_NO_MEM;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "Serial bridge initialised (UART%d, %d baud, AES-128-GCM).",
             config->uart_port, config->baud_rate);
    return ESP_OK;
}

esp_err_t serial_bridge_send(uint8_t        msg_type,
                             const uint8_t *payload,
                             size_t         len)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;
    if (payload == NULL && len > 0) return ESP_ERR_INVALID_ARG;
    if (len > MAX_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "Payload too large (%u > %u).", (unsigned)len, MAX_PAYLOAD_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    /* ------------------------------------------------------------------ */
    /* 1. Build the 5-byte header that will serve as AAD.                  */
    /* ------------------------------------------------------------------ */
    uint8_t header[FRAME_HEADER_SIZE];
    header[0] = FRAME_MAGIC_0;
    header[1] = FRAME_MAGIC_1;
    header[2] = (uint8_t)(len & 0xFFu);        /* LEN low byte  */
    header[3] = (uint8_t)((len >> 8) & 0xFFu); /* LEN high byte */
    header[4] = msg_type;

    /* ------------------------------------------------------------------ */
    /* 2. Generate a fresh 12-byte IV from the hardware RNG.               */
    /* ------------------------------------------------------------------ */
    uint8_t iv[FRAME_IV_SIZE];
    for (int i = 0; i < (int)FRAME_IV_SIZE; i += 4) {
        uint32_t rnd = esp_random();
        int copy_len = (int)FRAME_IV_SIZE - i;
        if (copy_len > 4) copy_len = 4;
        memcpy(&iv[i], &rnd, copy_len);
    }

    /* ------------------------------------------------------------------ */
    /* 3. Encrypt.                                                         */
    /* ------------------------------------------------------------------ */
    uint8_t ciphertext[MAX_PAYLOAD_SIZE];
    uint8_t auth_tag[FRAME_TAG_SIZE];

    int ret = aes_gcm_encrypt(iv,
                               header, FRAME_HEADER_SIZE,
                               payload, len,
                               ciphertext,
                               auth_tag);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES-GCM encryption failed (ret=%d).", ret);
        return ESP_FAIL;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Build and transmit the wire frame in three parts to avoid        */
    /*    an extra copy: HEADER | IV | CIPHERTEXT | TAG.                  */
    /* ------------------------------------------------------------------ */
    uart_write_bytes(s_uart_port, (const char *)header,     FRAME_HEADER_SIZE);
    uart_write_bytes(s_uart_port, (const char *)iv,         FRAME_IV_SIZE);
    uart_write_bytes(s_uart_port, (const char *)ciphertext, len);
    uart_write_bytes(s_uart_port, (const char *)auth_tag,   FRAME_TAG_SIZE);

    ESP_LOGD(TAG, "Sent frame type=0x%02X, payload=%u bytes.", msg_type, (unsigned)len);
    return ESP_OK;
}

void serial_bridge_deinit(void)
{
    if (!s_initialised) return;

    if (s_rx_task_handle != NULL) {
        vTaskDelete(s_rx_task_handle);
        s_rx_task_handle = NULL;
    }

    uart_driver_delete(s_uart_port);
    s_initialised = false;
    ESP_LOGI(TAG, "Serial bridge de-initialised.");
}
