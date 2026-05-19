idf_component_register(SRCS "secure_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash mbedtls esp_timer)
