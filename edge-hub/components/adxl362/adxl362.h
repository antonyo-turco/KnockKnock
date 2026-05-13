/**
 * @file adxl362.h
 * @brief Driver for the ADXL362 3-axis accelerometer (SPI interface, ESP-IDF).
 *
 * This is a simple driver I wrote for the ADXL362 accelerometer.
 * It handles chip init, reading acceleration data on X/Y/Z axes,
 * configuring the activity threshold (INT1) and inactivity threshold (INT2),
 * and setting the output data rate (ODR).
 *
 * The ADXL362 talks over SPI in Mode 0 (CPOL=0, CPHA=0), CS active low.
 * Max SPI clock is 8 MHz, but I use 4 MHz to be safe.
 *
 * Target: ESP32-C3 with ESP-IDF framework (PlatformIO).
 */

#ifndef ADXL362_H
#define ADXL362_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 *  SPI command bytes (from the datasheet, page 17)
 *  Every transaction starts with one of these
 * ========================================================= */
#define ADXL362_CMD_WRITE_REG   0x0A   /* write to a register */
#define ADXL362_CMD_READ_REG    0x0B   /* read from a register */
#define ADXL362_CMD_READ_FIFO   0x0D   /* read from the FIFO buffer */

/* =========================================================
 *  Register map
 *  I copied these from the ADXL362 datasheet (Table 6).
 *  The first 4 are read-only ID registers useful for testing
 *  the SPI connection before doing anything else.
 * ========================================================= */
#define ADXL362_REG_DEVID_AD        0x00  /* should always read 0xAD (Analog Devices ID) */
#define ADXL362_REG_DEVID_MST       0x01  /* should always read 0x1D (MEMS device ID) */
#define ADXL362_REG_PARTID          0x02  /* should always read 0xF2 (part number) */
#define ADXL362_REG_REVID           0x03  /* silicon revision - not critical */
#define ADXL362_REG_XDATA           0x08  /* X-axis 8-bit sample (less precise) */
#define ADXL362_REG_YDATA           0x09  /* Y-axis 8-bit sample */
#define ADXL362_REG_ZDATA           0x0A  /* Z-axis 8-bit sample */
#define ADXL362_REG_STATUS          0x0B  /* status flags: data ready, activity, etc. */
#define ADXL362_REG_FIFO_ENTRIES_L  0x0C  /* number of samples in FIFO (low byte) */
#define ADXL362_REG_FIFO_ENTRIES_H  0x0D  /* number of samples in FIFO (high byte) */
#define ADXL362_REG_XDATA_L         0x0E  /* X-axis 12-bit sample, LSB */
#define ADXL362_REG_XDATA_H         0x0F  /* X-axis 12-bit sample, MSB */
#define ADXL362_REG_YDATA_L         0x10  /* Y-axis 12-bit sample, LSB */
#define ADXL362_REG_YDATA_H         0x11  /* Y-axis 12-bit sample, MSB */
#define ADXL362_REG_ZDATA_L         0x12  /* Z-axis 12-bit sample, LSB */
#define ADXL362_REG_ZDATA_H         0x13  /* Z-axis 12-bit sample, MSB */
#define ADXL362_REG_TEMP_L          0x14  /* temperature LSB (not used here but useful) */
#define ADXL362_REG_TEMP_H          0x15  /* temperature MSB */
#define ADXL362_REG_SOFT_RESET      0x1F  /* write 0x52 here to reset the chip */
#define ADXL362_REG_THRESH_ACT_L    0x20  /* activity threshold, lower 8 bits */
#define ADXL362_REG_THRESH_ACT_H    0x21  /* activity threshold, upper 3 bits [10:8] */
#define ADXL362_REG_TIME_ACT        0x22  /* how long the accel must exceed threshold */
#define ADXL362_REG_THRESH_INACT_L  0x23  /* inactivity threshold, lower 8 bits */
#define ADXL362_REG_THRESH_INACT_H  0x24  /* inactivity threshold, upper 3 bits */
#define ADXL362_REG_TIME_INACT_L    0x25  /* inactivity timer, low byte */
#define ADXL362_REG_TIME_INACT_H    0x26  /* inactivity timer, high byte */
#define ADXL362_REG_ACT_INACT_CTL   0x27  /* enables activity/inactivity detection */
#define ADXL362_REG_FIFO_CONTROL    0x28  /* FIFO mode configuration */
#define ADXL362_REG_FIFO_SAMPLES    0x29  /* FIFO watermark level */
#define ADXL362_REG_INTMAP1         0x2A  /* which events trigger INT1 pin */
#define ADXL362_REG_INTMAP2         0x2B  /* which events trigger INT2 pin */
#define ADXL362_REG_FILTER_CTL      0x2C  /* ODR and measurement range config */
#define ADXL362_REG_POWER_CTL       0x2D  /* standby vs measurement mode */
#define ADXL362_REG_SELF_TEST       0x2E  /* self-test register (not used here) */

/* =========================================================
 *  POWER_CTL register bits
 * ========================================================= */
#define ADXL362_POWER_CTL_MEASURE   0x02  /* starts continuous measurement */
#define ADXL362_POWER_CTL_STANDBY   0x00  /* low-power standby mode */

/* =========================================================
 *  ACT_INACT_CTL register bit masks
 *  These control which detection features are enabled
 *  and whether they use absolute or referenced mode.
 *
 *  Referenced mode compares against a saved baseline,
 *  which helps ignore constant gravity on one axis.
 * ========================================================= */
#define ADXL362_ACT_EN           (1 << 0)  /* enable activity detection */
#define ADXL362_ACT_REF         (1 << 1)  /* referenced mode for activity */
#define ADXL362_INACT_EN         (1 << 2)  /* enable inactivity detection */
#define ADXL362_INACT_REF        (1 << 3)  /* referenced mode for inactivity */
#define ADXL362_LINKLOOP_DEFAULT (0 << 4)  /* activity and inactivity work independently */
#define ADXL362_LINKLOOP_LINKED  (1 << 4)  /* inactivity must happen before activity */
#define ADXL362_LINKLOOP_LOOP    (3 << 4)  /* loop between activity and inactivity */

/* =========================================================
 *  INTMAP1 / INTMAP2 bit masks
 *  OR these together to route multiple events to one pin.
 *  For example: ADXL362_INT_ACT | ADXL362_INT_DATA_READY
 * ========================================================= */
#define ADXL362_INT_DATA_READY      (1 << 0)  /* new data available */
#define ADXL362_INT_FIFO_READY      (1 << 1)  /* at least one sample in FIFO */
#define ADXL362_INT_FIFO_WATERMARK  (1 << 2)  /* FIFO reached the watermark level */
#define ADXL362_INT_FIFO_OVERRUN    (1 << 3)  /* FIFO is full and overflowing */
#define ADXL362_INT_ACT             (1 << 4)  /* activity detected */
#define ADXL362_INT_INACT           (1 << 5)  /* inactivity detected */
#define ADXL362_INT_AWAKE           (1 << 6)  /* awake bit (linked to activity state) */
#define ADXL362_INT_LOW             (1 << 7)  /* set this to invert the pin (active low) */

/* =========================================================
 *  Output Data Rate (ODR) options
 *  These go into bits [2:0] of FILTER_CTL.
 *  Higher ODR = more samples per second but also more power.
 *  100 Hz is usually a good default for motion detection.
 * ========================================================= */
typedef enum {
    ADXL362_ODR_12_5_HZ = 0x00,  /* 12.5 samples/sec - very low power */
    ADXL362_ODR_25_HZ   = 0x01,  /* 25 samples/sec */
    ADXL362_ODR_50_HZ   = 0x02,  /* 50 samples/sec */
    ADXL362_ODR_100_HZ  = 0x03,  /* 100 samples/sec (default) */
    ADXL362_ODR_200_HZ  = 0x04,  /* 200 samples/sec */
    ADXL362_ODR_400_HZ  = 0x05,  /* 400 samples/sec - highest ODR */
} adxl362_odr_t;

/* =========================================================
 *  Measurement range options
 *  These go into bits [7:6] of FILTER_CTL.
 *  Smaller range = better resolution but saturates earlier.
 *  1 LSB = 1 mg at +-2g, 2 mg at +-4g, 4 mg at +-8g.
 * ========================================================= */
typedef enum {
    ADXL362_RANGE_2G = 0x00,  /* +-2g range, 1 mg/LSB (default) */
    ADXL362_RANGE_4G = 0x40,  /* +-4g range, 2 mg/LSB */
    ADXL362_RANGE_8G = 0x80,  /* +-8g range, 4 mg/LSB */
} adxl362_range_t;

/* =========================================================
 *  Pin configuration struct
 *  Pass this to adxl362_init() to tell the driver
 *  which GPIO pins to use for the SPI bus.
 * ========================================================= */
typedef struct {
    spi_host_device_t spi_host;   /* SPI peripheral: SPI2_HOST or SPI3_HOST */
    gpio_num_t        pin_mosi;   /* Master Out Slave In */
    gpio_num_t        pin_miso;   /* Master In Slave Out */
    gpio_num_t        pin_sclk;   /* SPI clock */
    gpio_num_t        pin_cs;     /* Chip select (active low) */
    int               spi_clock_hz; /* SPI clock speed in Hz, max 8 MHz */
} adxl362_pins_t;

/* =========================================================
 *  Raw accelerometer data (12-bit signed integers)
 *  Values are in LSB units - use adxl362_read_mg() if you
 *  want milligrams directly.
 * ========================================================= */
typedef struct {
    int16_t x;  /* X-axis raw value (sign-extended to 16 bits) */
    int16_t y;  /* Y-axis raw value */
    int16_t z;  /* Z-axis raw value */
} adxl362_raw_data_t;

/* =========================================================
 *  Acceleration data converted to milli-g (mg)
 *  Easier to work with than raw values.
 *  At +-2g: 1000 mg = 1g = normal gravity.
 * ========================================================= */
typedef struct {
    float x_mg;  /* X-axis acceleration in milli-g */
    float y_mg;  /* Y-axis acceleration in milli-g */
    float z_mg;  /* Z-axis acceleration in milli-g */
} adxl362_data_mg_t;

/* =========================================================
 *  Device handle (opaque pointer)
 *  The internal struct is defined in adxl362.c - callers
 *  just use this pointer and don't touch the internals.
 * ========================================================= */
typedef struct adxl362_dev_t * adxl362_handle_t;

/* =========================================================
 *  Public API
 * ========================================================= */

/**
 * @brief Initialize the ADXL362 driver.
 *
 * Sets up the SPI bus, registers the device, does a soft reset,
 * and checks the chip ID registers to make sure the hardware is
 * actually there and responding correctly.
 * After this call the chip is in standby mode - call
 * adxl362_start_measurement() when you're ready to read data.
 *
 * @param[out] out_handle  Pointer to store the created device handle.
 * @param[in]  pins        SPI pin configuration.
 * @return ESP_OK on success.
 *         ESP_ERR_NOT_FOUND if the chip ID doesn't match (check wiring!).
 *         Other ESP_ERR_* codes if SPI setup fails.
 */
esp_err_t adxl362_init(adxl362_handle_t *out_handle,
                       const adxl362_pins_t *pins);

/**
 * @brief Start continuous measurement mode.
 *
 * The chip starts sampling at the configured ODR.
 * Call this after init and any other configuration.
 *
 * @param handle  Device handle from adxl362_init().
 * @return ESP_OK on success.
 */
esp_err_t adxl362_start_measurement(adxl362_handle_t handle);

/**
 * @brief Put the chip back into standby mode.
 *
 * Stops sampling and reduces power consumption.
 * You can reconfigure the chip while in standby.
 *
 * @param handle  Device handle.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_stop_measurement(adxl362_handle_t handle);

/**
 * @brief Read raw 12-bit acceleration values for all 3 axes.
 *
 * Reads 6 bytes in a single burst starting at XDATA_L.
 * The values are sign-extended to int16_t but only 12 bits are valid.
 *
 * @param handle      Device handle.
 * @param[out] data   Struct to store X, Y, Z raw values.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_read_raw(adxl362_handle_t handle,
                           adxl362_raw_data_t *data);

/**
 * @brief Read acceleration in milli-g for all 3 axes.
 *
 * Internally calls adxl362_read_raw() and multiplies by the
 * sensitivity factor that depends on the current range setting.
 * This is what you probably want to use most of the time.
 *
 * @param handle      Device handle.
 * @param[out] data   Struct to store X, Y, Z in mg.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_read_mg(adxl362_handle_t handle,
                          adxl362_data_mg_t *data);

/**
 * @brief Set the activity threshold for the INT1 interrupt.
 *
 * INT1 fires when acceleration on any axis exceeds this threshold
 * for at least time_ms milliseconds. Useful for detecting movement.
 *
 * The threshold is in raw LSB units (at +-2g: 1 LSB = 1 mg,
 * so threshold=250 means about 250 mg of acceleration).
 *
 * If referenced=true, the chip compares against a saved snapshot
 * instead of absolute 0, which is handy to ignore gravity.
 *
 * @param handle      Device handle.
 * @param threshold   Threshold value, 0 to 2047 (11 bits).
 * @param time_ms     Minimum time the threshold must be exceeded (ms).
 * @param referenced  Use referenced mode (true) or absolute mode (false).
 * @return ESP_OK on success.
 */
esp_err_t adxl362_set_activity_threshold(adxl362_handle_t handle,
                                          uint16_t threshold,
                                          uint8_t  time_ms,
                                          bool     referenced);

/**
 * @brief Set the inactivity threshold for the INT2 interrupt.
 *
 * INT2 fires when acceleration stays below this threshold for at
 * least time_ms milliseconds. Useful for detecting when something
 * stops moving (e.g. a device is put down on a table).
 *
 * The time_ms value is converted to sample counts internally
 * based on the current ODR, so set the ODR before calling this.
 *
 * @param handle      Device handle.
 * @param threshold   Threshold value, 0 to 2047.
 * @param time_ms     Minimum inactivity time in milliseconds.
 * @param referenced  Use referenced mode (true) or absolute mode (false).
 * @return ESP_OK on success.
 */
esp_err_t adxl362_set_inactivity_threshold(adxl362_handle_t handle,
                                            uint16_t threshold,
                                            uint16_t time_ms,
                                            bool     referenced);

/**
 * @brief Set the output data rate (ODR).
 *
 * Controls how many samples per second the chip produces.
 * Higher ODR means better time resolution but more power draw.
 * Default after init is 100 Hz.
 *
 * Note: this also affects how the inactivity time is calculated,
 * so call this before adxl362_set_inactivity_threshold().
 *
 * @param handle  Device handle.
 * @param odr     Desired ODR (see adxl362_odr_t enum).
 * @return ESP_OK on success.
 */
esp_err_t adxl362_set_odr(adxl362_handle_t handle, adxl362_odr_t odr);

/**
 * @brief Set the measurement range (+-2g, +-4g, or +-8g).
 *
 * Larger range = can measure bigger forces but less precise.
 * Default is +-2g which gives the best resolution for slow motion.
 *
 * @param handle  Device handle.
 * @param range   Desired range (see adxl362_range_t enum).
 * @return ESP_OK on success.
 */
esp_err_t adxl362_set_range(adxl362_handle_t handle, adxl362_range_t range);

/**
 * @brief Configure which events are routed to the INT1 pin.
 *
 * Pass a combination of ADXL362_INT_* flags OR'd together.
 * The library uses ADXL362_INT_ACT by default when you call
 * adxl362_set_activity_threshold(), but you can override that here.
 *
 * @param handle      Device handle.
 * @param int_flags   Bitwise OR of ADXL362_INT_* flags.
 * @param active_low  true = pin goes LOW on event, false = goes HIGH.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_config_int1(adxl362_handle_t handle,
                               uint8_t int_flags,
                               bool    active_low);

/**
 * @brief Configure which events are routed to the INT2 pin.
 *
 * Same as adxl362_config_int1() but for INT2.
 * Default for inactivity is ADXL362_INT_INACT.
 *
 * @param handle      Device handle.
 * @param int_flags   Bitwise OR of ADXL362_INT_* flags.
 * @param active_low  true = pin goes LOW on event, false = goes HIGH.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_config_int2(adxl362_handle_t handle,
                               uint8_t int_flags,
                               bool    active_low);

/**
 * @brief Read the STATUS register.
 *
 * Useful for polling instead of using interrupt pins.
 * Bit 4 = activity detected, bit 5 = inactivity detected,
 * bit 0 = data ready. See datasheet Table 9 for all bits.
 *
 * @param handle        Device handle.
 * @param[out] status   Value of the STATUS register.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_get_status(adxl362_handle_t handle, uint8_t *status);

/**
 * @brief Perform a soft reset of the chip.
 *
 * Resets all registers to their default values.
 * The chip goes back to standby mode after reset.
 * There's a 10ms delay built in to wait for the chip to come back up.
 *
 * @param handle  Device handle.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_soft_reset(adxl362_handle_t handle);

/**
 * @brief Free resources and destroy the device handle.
 *
 * Puts the chip in standby, removes the SPI device, and frees memory.
 * Don't use the handle after calling this.
 *
 * @param handle  Device handle to destroy.
 * @return ESP_OK on success.
 */
esp_err_t adxl362_deinit(adxl362_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* ADXL362_H */
