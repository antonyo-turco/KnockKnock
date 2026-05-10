idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    REQUIRES adxl362 driver esp_common freertos esp_driver_gpio
)
