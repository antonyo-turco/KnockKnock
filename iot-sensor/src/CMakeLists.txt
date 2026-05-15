FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES adxl362 driver esp_common freertos fft_lib esp-dsp esp_now_comm secure_store
)
