/**
 * @file secure_store.c
 * @brief Secure NVS storage wrapper for the KnockKnock gateway.
 *
 * Initialization strategy:
 *   1. Look for the "nvs_keys" partition in the partition table.
 *   2. If found AND the keys are valid, use encrypted NVS (production path).
 *   3. If the partition exists but keys are uninitialised, generate new keys,
 *      persist them, and use encrypted NVS.
 *   4. If the partition is absent or any step fails (development / first-boot
 *      without flash encryption), fall back to plain nvs_flash_init().
 *
 * This allows the exact same firmware binary to run in both development
 * (plain NVS) and production (encrypted NVS + flash encryption) environments.
 */

#include "secure_store.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"

static const char *TAG = "SECURE_STORE";

#define NVS_NAMESPACE "secure_data"

/* -------------------------------------------------------------------------- */
/*  Initialization                                                             */
/* -------------------------------------------------------------------------- */

esp_err_t secure_store_init(void)
{
    esp_err_t err;

    /* ------------------------------------------------------------------ */
    /* Try the encrypted NVS path first (production).                      */
    /* ------------------------------------------------------------------ */
    const esp_partition_t *key_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
        "nvs_keys");

    if (key_part != NULL) {
        nvs_sec_cfg_t cfg;

        err = nvs_flash_read_security_cfg(key_part, &cfg);
        if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
            ESP_LOGI(TAG, "NVS keys not initialised – generating new keys.");
            err = nvs_flash_generate_keys(key_part, &cfg);
        }

        if (err == ESP_OK) {
            err = nvs_flash_secure_init(&cfg);
            if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
                err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_LOGW(TAG, "Encrypted NVS partition mismatch – erasing.");
                nvs_flash_erase();
                err = nvs_flash_secure_init(&cfg);
            }

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Secure (encrypted) NVS initialised.");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Encrypted NVS init failed (%s) – falling back.",
                     esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "Could not load/generate NVS keys (%s) – falling back.",
                     esp_err_to_name(err));
        }
    } else {
        /* Development build: nvs_keys partition not present. */
        ESP_LOGW(TAG,
                 "nvs_keys partition not found – using plain NVS "
                 "(development mode, enable flash encryption for production).");
    }

    /* ------------------------------------------------------------------ */
    /* Fallback: plain (unencrypted) NVS.                                  */
    /* ------------------------------------------------------------------ */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition mismatch – erasing.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Plain NVS initialised (development mode).");
    } else {
        ESP_LOGE(TAG, "NVS initialisation failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* -------------------------------------------------------------------------- */
/*  Write / Read (blob)                                                        */
/* -------------------------------------------------------------------------- */

esp_err_t secure_store_write(const char *key, const uint8_t *data, size_t len)
{
    if (!key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, data, len);
        if (err == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t secure_store_read(const char *key, uint8_t **data_out, size_t *len_out)
{
    if (!key || !data_out || !len_out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t required = 0;
    err = nvs_get_blob(handle, key, NULL, &required);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    uint8_t *buf = malloc(required);
    if (!buf) { nvs_close(handle); return ESP_ERR_NO_MEM; }

    err = nvs_get_blob(handle, key, buf, &required);
    nvs_close(handle);

    if (err == ESP_OK) {
        *data_out = buf;
        *len_out  = required;
    } else {
        free(buf);
    }
    return err;
}

/* -------------------------------------------------------------------------- */
/*  Write / Read (string)                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t secure_store_write_string(const char *key, const char *str)
{
    if (!key || !str) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key, str);
        if (err == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t secure_store_read_string(const char *key, char **str_out)
{
    if (!key || !str_out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t required = 0;
    err = nvs_get_str(handle, key, NULL, &required);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    char *buf = malloc(required);
    if (!buf) { nvs_close(handle); return ESP_ERR_NO_MEM; }

    err = nvs_get_str(handle, key, buf, &required);
    nvs_close(handle);

    if (err == ESP_OK) {
        *str_out = buf;
    } else {
        free(buf);
    }
    return err;
}

/* -------------------------------------------------------------------------- */
/*  Utility                                                                    */
/* -------------------------------------------------------------------------- */

void secure_store_deobfuscate(const uint8_t *obfuscated_data,
                              size_t         len,
                              uint8_t        xor_key,
                              uint8_t       *out)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = obfuscated_data[i] ^ xor_key;
    }
}
