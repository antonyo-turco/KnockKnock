#include "secure_store.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "aes/esp_aes.h"
#include "esp_system.h"
#include "esp_random.h"

static const char *TAG = "SECURE_STORE";

#define NVS_NAMESPACE "secure_data"

// 32-byte AES-256 key, obfuscated with XOR key 0x5A
// Original key: "MySuperSecretHardwareAesKey12345" (32 chars)
static const uint8_t obfuscated_aes_key[32] = {
    'M'^0x5A, 'y'^0x5A, 'S'^0x5A, 'u'^0x5A, 'p'^0x5A, 'e'^0x5A, 'r'^0x5A, 'S'^0x5A,
    'e'^0x5A, 'c'^0x5A, 'r'^0x5A, 'e'^0x5A, 't'^0x5A, 'H'^0x5A, 'a'^0x5A, 'r'^0x5A,
    'd'^0x5A, 'w'^0x5A, 'a'^0x5A, 'r'^0x5A, 'e'^0x5A, 'A'^0x5A, 'e'^0x5A, 's'^0x5A,
    'K'^0x5A, 'e'^0x5A, 'y'^0x5A, '1'^0x5A, '2'^0x5A, '3'^0x5A, '4'^0x5A, '5'^0x5A
};
#define XOR_KEY 0x5A

static uint8_t aes_key_plain[32];
static bool key_initialized = false;

void secure_store_deobfuscate(const uint8_t *obfuscated_data, size_t len, uint8_t xor_key, uint8_t *out) {
    for (size_t i = 0; i < len; i++) {
        out[i] = obfuscated_data[i] ^ xor_key;
    }
}

esp_err_t secure_store_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (!key_initialized) {
        secure_store_deobfuscate(obfuscated_aes_key, sizeof(obfuscated_aes_key), XOR_KEY, aes_key_plain);
        key_initialized = true;
    }
    
    return ret;
}

// PKCS#7 padding
static size_t apply_pkcs7_padding(uint8_t *buffer, size_t data_len, size_t block_size) {
    uint8_t padding_val = block_size - (data_len % block_size);
    for (size_t i = 0; i < padding_val; i++) {
        buffer[data_len + i] = padding_val;
    }
    return data_len + padding_val;
}

static size_t remove_pkcs7_padding(uint8_t *buffer, size_t padded_len, size_t block_size) {
    if (padded_len == 0 || padded_len % block_size != 0) return 0;
    uint8_t padding_val = buffer[padded_len - 1];
    if (padding_val == 0 || padding_val > block_size) return 0;
    
    for (size_t i = 0; i < padding_val; i++) {
        if (buffer[padded_len - 1 - i] != padding_val) return 0; // Invalid padding
    }
    return padded_len - padding_val;
}

esp_err_t secure_store_write(const char *key, const uint8_t *data, size_t len) {
    if (!key || !data || len == 0 || !key_initialized) return ESP_ERR_INVALID_ARG;

    // IV (16 bytes) + padded data
    size_t padded_len = len + (16 - (len % 16));
    size_t total_len = 16 + padded_len;
    
    uint8_t *enc_buf = malloc(total_len);
    if (!enc_buf) return ESP_ERR_NO_MEM;

    // Generate random IV
    esp_fill_random(enc_buf, 16);
    
    // Copy data and pad
    uint8_t *padded_data = malloc(padded_len);
    if (!padded_data) {
        free(enc_buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(padded_data, data, len);
    apply_pkcs7_padding(padded_data, len, 16);

    // MbedTLS AES context
    esp_aes_context aes_ctx;
    esp_aes_init(&aes_ctx);
    esp_aes_setkey(&aes_ctx, aes_key_plain, 256);

    // CBC mode modifies IV, so we use a copy of the IV to encrypt
    uint8_t iv_copy[16];
    memcpy(iv_copy, enc_buf, 16);

    // Encrypt
    esp_aes_crypt_cbc(&aes_ctx, ESP_AES_ENCRYPT, padded_len, iv_copy, padded_data, enc_buf + 16);

    esp_aes_free(&aes_ctx);
    free(padded_data);

    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, key, enc_buf, total_len);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }

    free(enc_buf);
    return err;
}

esp_err_t secure_store_read(const char *key, uint8_t **data_out, size_t *len_out) {
    if (!key || !data_out || !len_out || !key_initialized) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return err;

    size_t total_len = 0;
    err = nvs_get_blob(nvs_handle, key, NULL, &total_len);
    if (err != ESP_OK || total_len <= 16 || total_len % 16 != 0) {
        nvs_close(nvs_handle);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    uint8_t *enc_buf = malloc(total_len);
    if (!enc_buf) {
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(nvs_handle, key, enc_buf, &total_len);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        free(enc_buf);
        return err;
    }

    size_t padded_len = total_len - 16;
    uint8_t *dec_buf = malloc(padded_len);
    if (!dec_buf) {
        free(enc_buf);
        return ESP_ERR_NO_MEM;
    }

    uint8_t iv[16];
    memcpy(iv, enc_buf, 16);

    esp_aes_context aes_ctx;
    esp_aes_init(&aes_ctx);
    esp_aes_setkey(&aes_ctx, aes_key_plain, 256);

    esp_aes_crypt_cbc(&aes_ctx, ESP_AES_DECRYPT, padded_len, iv, enc_buf + 16, dec_buf);
    esp_aes_free(&aes_ctx);
    free(enc_buf);

    size_t actual_len = remove_pkcs7_padding(dec_buf, padded_len, 16);
    if (actual_len == 0) {
        // Padding error, possibly wrong key or corrupted data
        free(dec_buf);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *final_data = malloc(actual_len);
    if (!final_data) {
        free(dec_buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(final_data, dec_buf, actual_len);
    free(dec_buf);

    *data_out = final_data;
    *len_out = actual_len;
    return ESP_OK;
}

esp_err_t secure_store_write_string(const char *key, const char *str) {
    if (!str) return ESP_ERR_INVALID_ARG;
    return secure_store_write(key, (const uint8_t *)str, strlen(str) + 1); // include null terminator
}

esp_err_t secure_store_read_string(const char *key, char **str_out) {
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t err = secure_store_read(key, &data, &len);
    if (err == ESP_OK) {
        // Ensure null termination just in case
        if (len == 0 || data[len - 1] != '\0') {
            char *safe_str = malloc(len + 1);
            if (safe_str) {
                memcpy(safe_str, data, len);
                safe_str[len] = '\0';
                free(data);
                *str_out = safe_str;
            } else {
                free(data);
                err = ESP_ERR_NO_MEM;
            }
        } else {
            *str_out = (char *)data;
        }
    }
    return err;
}
