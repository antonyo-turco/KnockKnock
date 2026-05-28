#include "adxl362.h"
#include "config.h"
#include "freertos/FreeRTOS.h"



static const char *TAG1 = "ADXL362_UTILS";

/* =========================================================
 *  Helper: configure the ADXL362 and start measurement
 * ========================================================= */
static adxl362_handle_t sensor_init(float frequency_hz) {
  adxl362_handle_t sensor = NULL;

  adxl362_pins_t pins = {
      .spi_host = MY_SPI_HOST,
      .pin_mosi = MY_PIN_MOSI,
      .pin_miso = MY_PIN_MISO,
      .pin_sclk = MY_PIN_SCLK,
      .pin_cs = MY_PIN_CS,
      .spi_clock_hz = MY_SPI_CLOCK,
  };

  esp_err_t ret = adxl362_init(&sensor, &pins);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG1, "ADXL362 init failed: %s - check wiring!",
             esp_err_to_name(ret));
    return NULL;
  }

  adxl362_set_range(sensor, ADXL362_RANGE_2G);


  switch ((int)frequency_hz) {
    case 12: adxl362_set_odr(sensor, ADXL362_ODR_12_5_HZ); break;
    case 25: adxl362_set_odr(sensor, ADXL362_ODR_25_HZ); break;
    case 50: adxl362_set_odr(sensor, ADXL362_ODR_50_HZ); break;
    case 100: adxl362_set_odr(sensor, ADXL362_ODR_100_HZ); break;
    case 200: adxl362_set_odr(sensor, ADXL362_ODR_200_HZ); break;
    case 400: adxl362_set_odr(sensor, ADXL362_ODR_400_HZ); break;
    default:
      ESP_LOGW(TAG1, "Unsupported ODR %d Hz - defaulting to 100 Hz", (int)frequency_hz);
      adxl362_set_odr(sensor, ADXL362_ODR_100_HZ);
      frequency_hz = 100;
      break;
  }
  adxl362_set_odr(sensor, ADXL362_ODR_100_HZ);

  adxl362_set_activity_threshold(sensor, THRESHOLD_MG, ACTIVITY_TIME_MS, true);
  // adxl362_set_inactivity_threshold(sensor, THRESHOLD_MG, INACTIVITY_TIME_MS,
  // true);

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