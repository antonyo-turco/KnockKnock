#include "adxl362.h"
#include "config.h"
#include "freertos/FreeRTOS.h"





/* =========================================================
 *  Pin definitions - change these to match your wiring
 * ========================================================= */
#define MY_SPI_HOST   SPI3_HOST
#define MY_PIN_MOSI   GPIO_NUM_23
#define MY_PIN_MISO   GPIO_NUM_19
#define MY_PIN_SCLK   GPIO_NUM_18
#define MY_PIN_CS     GPIO_NUM_5
#define MY_PIN_INT1   GPIO_NUM_33   /* must be an RTC GPIO for ext0 wakeup */
#define MY_SPI_CLOCK  1000000       /* 1 MHz - reduced for debugging on breadboard */




static const char *TAG1 = "ADXL362_UTILS";







/* =========================================================
 *  Helper: configure the ADXL362 and start measurement
 * ========================================================= */
static adxl362_handle_t sensor_init(void)
{
    adxl362_handle_t sensor = NULL;

    adxl362_pins_t pins = {
        .spi_host     = MY_SPI_HOST,
        .pin_mosi     = MY_PIN_MOSI,
        .pin_miso     = MY_PIN_MISO,
        .pin_sclk     = MY_PIN_SCLK,
        .pin_cs       = MY_PIN_CS,
        .spi_clock_hz = MY_SPI_CLOCK,
    };

    esp_err_t ret = adxl362_init(&sensor, &pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG1, "ADXL362 init failed: %s - check wiring!", esp_err_to_name(ret));
        return NULL;
    }

    adxl362_set_range(sensor, ADXL362_RANGE_2G);
    adxl362_set_odr(sensor, ADXL362_ODR_100_HZ);

    adxl362_set_activity_threshold(sensor, THRESHOLD_MG, ACTIVITY_TIME_MS, true);
    adxl362_set_inactivity_threshold(sensor, THRESHOLD_MG, INACTIVITY_TIME_MS, true);

    adxl362_start_measurement(sensor);

    vTaskDelay(pdMS_TO_TICKS(300));

    uint8_t status = 0;
    adxl362_get_status(sensor, &status);
    if (status & (1 << 4)) {
        ESP_LOGW(TAG1, "Activity flag set during init settle - clearing it.");
        vTaskDelay(pdMS_TO_TICKS(200));
        adxl362_get_status(sensor, &status);
    }

    return sensor;
}