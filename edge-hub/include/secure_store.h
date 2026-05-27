#ifndef SECURE_STORE_H
#define SECURE_STORE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t secure_store_init(void);
esp_err_t secure_store_write_string(const char *key, const char *value);
esp_err_t secure_store_read_string(const char *key, char *out_val, size_t max_len);
esp_err_t secure_store_write_blob(const char *key, const void *data, size_t length);
esp_err_t secure_store_read_blob(const char *key, void *out_data, size_t *length);

#ifdef __cplusplus
}
#endif

#endif /* SECURE_STORE_H */
