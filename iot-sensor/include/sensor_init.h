#ifndef SENSOR_INIT_H
#define SENSOR_INIT_H

#include "adxl362.h"

/* Configure the ADXL362 at the given ODR and return a ready handle.
 * Returns NULL on SPI or device init failure. */
adxl362_handle_t sensor_init(float frequency_hz);

#endif // SENSOR_INIT_H
