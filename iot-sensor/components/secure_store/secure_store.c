#include "secure_store.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"

static const char *TAG = "SECURE_STORE";

#define NVS_NAMESPACE "secure_data"

esp_err_t secure_store_init(void) {
    esp_err_t err;
    nvs_sec_cfg_t cfg;
    
    const esp_partition_t *key_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, "nvs_keys");

    if (key_part == NULL) {
        ESP_LOGE(TAG, "NVS Key partition 'nvs_keys' not found in partition table!");
        return ESP_ERR_NOT_FOUND;
    }

    // Attempt to load the NVS keys from the 'nvs_keys' partition
    err = nvs_flash_read_security_cfg(key_part, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "NVS keys not initialized. Generating new keys...");
        err = nvs_flash_generate_keys(key_part, &cfg);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load or generate NVS keys: %s", esp_err_to_name(err));
        return err;
    }
    
    // Initialize NVS using the secure keys
    err = nvs_flash_secure_init(&cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition format mismatch or full, erasing...");
        nvs_flash_erase();
        err = nvs_flash_secure_init(&cfg);
    }
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Secure NVS initialized successfully.");
    } else {
        ESP_LOGE(TAG, "Secure NVS initialization failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t secure_store_write(const char *key, const uint8_t *data, size_t len) {
    if (!key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, key, data, len);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    return err;
}

esp_err_t secure_store_read(const char *key, uint8_t **data_out, size_t *len_out) {
    if (!key || !data_out || !len_out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return err;

    size_t required_size = 0;
    err = nvs_get_blob(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    uint8_t *buffer = malloc(required_size);
    if (!buffer) {
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(nvs_handle, key, buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        *data_out = buffer;
        *len_out = required_size;
    } else {
        free(buffer);
    }

    return err;
}

esp_err_t secure_store_write_string(const char *key, const char *str) {
    if (!str) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, key, str);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    return err;
}

esp_err_t secure_store_read_string(const char *key, char **str_out) {
    if (!key || !str_out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return err;

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    char *buffer = malloc(required_size);
    if (!buffer) {
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs_handle, key, buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        *str_out = buffer;
    } else {
        free(buffer);
    }

    return err;
}
