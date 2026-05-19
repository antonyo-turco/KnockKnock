idf_component_register(
    SRCS "serial_bridge.c"
    INCLUDE_DIRS
        "include"
        "$ENV{IDF_PATH}/components/mbedtls/mbedtls"
        "$ENV{IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/include"
        "$ENV{IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/include"
    REQUIRES
        esp_driver_uart
        nvs_flash
        mbedtls
        esp_hw_support
        secure_store
)
