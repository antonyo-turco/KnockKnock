#ifndef SECURE_STORE_H
#define SECURE_STORE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Initialize the secure storage component.
 *        This initializes NVS if not already initialized.
 * @return ESP_OK on success.
 */
esp_err_t secure_store_init(void);

/**
 * @brief Write data securely to NVS.
 *        The data is encrypted using AES-256 before being stored.
 * @param key The NVS key (max 15 characters).
 * @param data Pointer to the data to encrypt and store.
 * @param len Length of the data.
 * @return ESP_OK on success.
 */
esp_err_t secure_store_write(const char *key, const uint8_t *data, size_t len);

/**
 * @brief Read and decrypt data from NVS.
 *        Memory is dynamically allocated for data_out, the caller MUST free it.
 * @param key The NVS key.
 * @param data_out Pointer to a pointer where the decrypted data will be stored.
 * @param len_out Pointer to store the length of the decrypted data.
 * @return ESP_OK on success.
 */
esp_err_t secure_store_read(const char *key, uint8_t **data_out, size_t *len_out);

/**
 * @brief Write a null-terminated string securely to NVS.
 * @param key The NVS key.
 * @param str The string to encrypt and store.
 * @return ESP_OK on success.
 */
esp_err_t secure_store_write_string(const char *key, const char *str);

/**
 * @brief Read a null-terminated string securely from NVS.
 *        Memory is dynamically allocated for str_out, the caller MUST free it.
 * @param key The NVS key.
 * @param str_out Pointer to a pointer where the decrypted string will be stored.
 * @return ESP_OK on success.
 */
esp_err_t secure_store_read_string(const char *key, char **str_out);

/**
 * @brief Utility to deobfuscate strings/keys at runtime.
 *        This allows keeping sensitive strings obfuscated in the binary.
 * @param obfuscated_data The XOR-obfuscated data array.
 * @param len Length of the array.
 * @param xor_key The 8-bit XOR key used during obfuscation.
 * @param out Buffer to store the plain result (must be at least len bytes).
 */
void secure_store_deobfuscate(const uint8_t *obfuscated_data, size_t len, uint8_t xor_key, uint8_t *out);

#endif // SECURE_STORE_H
