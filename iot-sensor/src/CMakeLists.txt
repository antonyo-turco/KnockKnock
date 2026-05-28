idf_component_register(
    SRCS
        main.c
        feature_extraction.c
        hub_communication.c
        led_indicator.c
        sensor_init.c
        sliding_window.c
        tinyml_training.c
    INCLUDE_DIRS "."
    REQUIRES adxl362 driver esp_common freertos fft_lib esp-dsp secure_store
)
