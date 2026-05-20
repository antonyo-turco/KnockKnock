idf_component_register(SRCS "main.c" "hub.c" "secure_store.c" "serial_bridge.c" "cloud_task.c"
                             "ca_crt_embedded.c"
                    INCLUDE_DIRS "../include"
                    REQUIRES nvs_flash mbedtls esp_timer driver esp_wifi esp_event)
