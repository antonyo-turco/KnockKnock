idf_component_register(
    SRCS "espnow_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        secure_store
)
