idf_component_register(
    SRCS "adxl362.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_common freertos
)
