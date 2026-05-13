/**
 * @file adxl362.c
 * @brief ADXL362 accelerometer driver implementation for ESP32 (ESP-IDF, SPI).
 *
 * The ADXL362 communicates over SPI Mode 0.
 * Every transaction follows this format:
 *   [CMD byte] [register address] [data bytes...]
 *
 * For reads: the first 2 bytes of the RX buffer are garbage (CMD echo),
 * actual data starts from byte index 2.
 *
 * For burst reads (like reading all 3 axes at once), we just
 * keep reading consecutive bytes - the chip auto-increments the address.
 *
 * I tried to keep the SPI functions simple and reusable.
 * malloc/free are used to handle variable-length transactions without
 * needing a big static buffer.
 */

#include "adxl362.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADXL362";

/* =========================================================
 *  Internal device struct
 *  Callers only see the opaque adxl362_handle_t pointer.
 *  Keeping the internals here avoids polluting the header.
 * ========================================================= */
struct adxl362_dev_t {
    spi_device_handle_t spi_dev;    /* ESP-IDF SPI device handle */
    adxl362_range_t     range;      /* current measurement range (needed for mg conversion) */
    adxl362_odr_t       odr;        /* current ODR (needed for inactivity time calculation) */
    uint8_t             filter_ctl; /* shadow copy of FILTER_CTL - avoids unnecessary reads */
};

/* =========================================================
 *  Private SPI helper functions
 *  These do the actual low-level communication with the chip.
 *  I made them static so they're not visible outside this file.
 * ========================================================= */

/**
 * Write a single byte to a register.
 * Format: [0x0A] [reg] [value]
 */
static esp_err_t _write_reg(spi_device_handle_t spi, uint8_t reg, uint8_t value)
{
    uint8_t buf[3] = { ADXL362_CMD_WRITE_REG, reg, value };

    spi_transaction_t t = {
        .length    = 8 * 3,   /* length in bits */
        .tx_buffer = buf,
        .rx_buffer = NULL,    /* don't care about RX for writes */
    };

    return spi_device_polling_transmit(spi, &t);
}

/**
 * Write multiple bytes starting at a register address.
 * The chip auto-increments the address for burst writes.
 * Allocates a temporary buffer to prepend the CMD and REG bytes.
 */
static esp_err_t _write_regs(spi_device_handle_t spi,
                              uint8_t reg,
                              const uint8_t *data,
                              size_t len)
{
    uint8_t *buf = malloc(2 + len);
    if (!buf) return ESP_ERR_NO_MEM;

    buf[0] = ADXL362_CMD_WRITE_REG;
    buf[1] = reg;
    memcpy(&buf[2], data, len);

    spi_transaction_t t = {
        .length    = 8 * (2 + len),
        .tx_buffer = buf,
        .rx_buffer = NULL,
    };

    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    free(buf);
    return ret;
}

/**
 * Read one or more bytes starting at a register address.
 * Format: [0x0B] [reg] [dummy...] -> RX: [echo] [echo] [data...]
 * The first 2 bytes in rx are the echoed CMD and REG, so we skip them.
 *
 * Both tx and rx buffers need to be the same length for full-duplex SPI.
 */
static esp_err_t _read_regs(spi_device_handle_t spi,
                             uint8_t reg,
                             uint8_t *data,
                             size_t len)
{
    size_t total = 2 + len;  /* 2 header bytes + actual data */
    uint8_t *tx = calloc(total, 1);  /* calloc so dummy bytes are 0x00 */
    uint8_t *rx = calloc(total, 1);
    if (!tx || !rx) {
        free(tx);
        free(rx);
        return ESP_ERR_NO_MEM;
    }

    tx[0] = ADXL362_CMD_READ_REG;
    tx[1] = reg;
    /* remaining tx bytes are 0x00 (dummy) */

    spi_transaction_t t = {
        .length    = 8 * total,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    if (ret == ESP_OK) {
        memcpy(data, &rx[2], len);  /* skip first 2 bytes (CMD+REG echo) */
    }

    free(tx);
    free(rx);
    return ret;
}

/* =========================================================
 *  Public API implementation
 * ========================================================= */

esp_err_t adxl362_init(adxl362_handle_t *out_handle,
                       const adxl362_pins_t *pins)
{
    esp_err_t ret;

    if (!out_handle || !pins) return ESP_ERR_INVALID_ARG;

    /* allocate the internal device struct */
    struct adxl362_dev_t *dev = calloc(1, sizeof(struct adxl362_dev_t));
    if (!dev) return ESP_ERR_NO_MEM;

    /* set sensible defaults */
    dev->range      = ADXL362_RANGE_2G;
    dev->odr        = ADXL362_ODR_100_HZ;
    dev->filter_ctl = 0x13;  /* 100 Hz ODR, +-2g range, half-bandwidth filter off */

    /* initialize the SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = pins->pin_mosi,
        .miso_io_num     = pins->pin_miso,
        .sclk_io_num     = pins->pin_sclk,
        .quadwp_io_num   = -1,   /* not used */
        .quadhd_io_num   = -1,   /* not used */
        .max_transfer_sz = 64,   /* max bytes per transaction */
    };

    ret = spi_bus_initialize(pins->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE just means the bus was already initialized,
         * which is fine if multiple devices share the same bus */
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    /* register the ADXL362 as a device on the SPI bus */
    spi_device_interface_config_t dev_cfg = {
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
        .mode           = 0,   /* Mode 0: CPOL=0, CPHA=0 - required by ADXL362 */
        .clock_speed_hz = pins->spi_clock_hz > 0
                              ? pins->spi_clock_hz
                              : 4000000,  /* default to 4 MHz if not specified */
        .spics_io_num   = pins->pin_cs,
        .queue_size     = 1,
        .flags          = 0,
    };

    ret = spi_bus_add_device(pins->spi_host, &dev_cfg, &dev->spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(pins->spi_host);
        free(dev);
        return ret;
    }

    /* do a soft reset first so we start from a known state */
    ret = _write_reg(dev->spi_dev, ADXL362_REG_SOFT_RESET, 0x52);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "soft reset failed: %s", esp_err_to_name(ret));
        spi_bus_remove_device(dev->spi_dev);
        spi_bus_free(pins->spi_host);
        free(dev);
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  /* wait for the chip to come back up after reset */

    /* verify the chip is actually an ADXL362 by reading the ID registers */
    uint8_t id[3] = {0};
    ret = _read_regs(dev->spi_dev, ADXL362_REG_DEVID_AD, id, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to read chip ID: %s", esp_err_to_name(ret));
        spi_bus_remove_device(dev->spi_dev);
        spi_bus_free(pins->spi_host);
        free(dev);
        return ret;
    }

    ESP_LOGI(TAG, "chip ID -> DEVID_AD=0x%02X  DEVID_MST=0x%02X  PARTID=0x%02X",
             id[0], id[1], id[2]);

    /* expected: 0xAD, 0x1D, 0xF2 - if not, something is wrong with the wiring */
    if (id[0] != 0xAD || id[1] != 0x1D || id[2] != 0xF2) {
        ESP_LOGE(TAG, "chip ID mismatch! Check your wiring and CS pin.");
        spi_bus_remove_device(dev->spi_dev);
        spi_bus_free(pins->spi_host);
        free(dev);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "ADXL362 initialized successfully.");
    *out_handle = dev;
    return ESP_OK;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_start_measurement(adxl362_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return _write_reg(handle->spi_dev,
                      ADXL362_REG_POWER_CTL,
                      ADXL362_POWER_CTL_MEASURE);
}

/* --------------------------------------------------------- */

esp_err_t adxl362_stop_measurement(adxl362_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return _write_reg(handle->spi_dev,
                      ADXL362_REG_POWER_CTL,
                      ADXL362_POWER_CTL_STANDBY);
}

/* --------------------------------------------------------- */

esp_err_t adxl362_read_raw(adxl362_handle_t handle,
                           adxl362_raw_data_t *data)
{
    if (!handle || !data) return ESP_ERR_INVALID_ARG;

    /*
     * Read 6 bytes in a single burst starting at XDATA_L:
     *   [XDATA_L] [XDATA_H] [YDATA_L] [YDATA_H] [ZDATA_L] [ZDATA_H]
     *
     * Each axis is 12-bit signed (two's complement).
     * The value is in bits [11:0] - bits [15:12] of the H register are unused.
     * After combining LSB+MSB we need to sign-extend from 12 to 16 bits.
     */
    uint8_t raw[6] = {0};
    esp_err_t ret = _read_regs(handle->spi_dev,
                                ADXL362_REG_XDATA_L,
                                raw,
                                6);
    if (ret != ESP_OK) return ret;

    /* combine LSB and MSB for each axis */
    int16_t x = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)(((uint16_t)raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)(((uint16_t)raw[5] << 8) | raw[4]);

    /*
     * Sign extension: bit 11 is the sign bit.
     * If it's set, fill the upper 4 bits with 1s to get a proper
     * negative int16_t value. Without this, negative values would
     * appear as large positive numbers.
     */
    if (x & 0x0800) x |= (int16_t)0xF000;
    if (y & 0x0800) y |= (int16_t)0xF000;
    if (z & 0x0800) z |= (int16_t)0xF000;

    data->x = x;
    data->y = y;
    data->z = z;

    return ESP_OK;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_read_mg(adxl362_handle_t handle,
                          adxl362_data_mg_t *data)
{
    if (!handle || !data) return ESP_ERR_INVALID_ARG;

    adxl362_raw_data_t raw;
    esp_err_t ret = adxl362_read_raw(handle, &raw);
    if (ret != ESP_OK) return ret;

    /*
     * Sensitivity factor depends on the configured range:
     *   +-2g -> 1 mg/LSB
     *   +-4g -> 2 mg/LSB
     *   +-8g -> 4 mg/LSB
     * (from ADXL362 datasheet, Table 1)
     */
    float scale;
    switch (handle->range) {
        case ADXL362_RANGE_4G: scale = 2.0f; break;
        case ADXL362_RANGE_8G: scale = 4.0f; break;
        default:               scale = 1.0f; break;  /* default is 2g */
    }

    data->x_mg = (float)raw.x * scale;
    data->y_mg = (float)raw.y * scale;
    data->z_mg = (float)raw.z * scale;

    return ESP_OK;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_set_activity_threshold(adxl362_handle_t handle,
                                          uint16_t threshold,
                                          uint8_t  time_ms,
                                          bool     referenced)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* threshold is 11 bits max, mask off anything higher */
    threshold &= 0x07FF;

    /*
     * Write threshold and time in a single burst (3 registers):
     *   THRESH_ACT_L -> lower 8 bits
     *   THRESH_ACT_H -> upper 3 bits [10:8]
     *   TIME_ACT     -> duration in samples
     */
    uint8_t data[3] = {
        (uint8_t)(threshold & 0xFF),
        (uint8_t)((threshold >> 8) & 0x07),
        time_ms,
    };

    esp_err_t ret = _write_regs(handle->spi_dev,
                                 ADXL362_REG_THRESH_ACT_L,
                                 data,
                                 3);
    if (ret != ESP_OK) return ret;

    /* enable activity detection in ACT_INACT_CTL, preserve other bits */
    uint8_t act_ctl;
    ret = _read_regs(handle->spi_dev, ADXL362_REG_ACT_INACT_CTL, &act_ctl, 1);
    if (ret != ESP_OK) return ret;

    act_ctl |= ADXL362_ACT_EN;
    if (referenced)
        act_ctl |= ADXL362_ACT_REF;
    else
        act_ctl &= ~ADXL362_ACT_REF;

    ret = _write_reg(handle->spi_dev, ADXL362_REG_ACT_INACT_CTL, act_ctl);
    if (ret != ESP_OK) return ret;

    /* route the activity event to INT1 pin (active high) */
    ret = adxl362_config_int1(handle, ADXL362_INT_ACT, false);

    ESP_LOGI(TAG, "activity threshold set: %u LSB, time: %u ms, referenced: %s",
             threshold, time_ms, referenced ? "yes" : "no");
    return ret;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_set_inactivity_threshold(adxl362_handle_t handle,
                                            uint16_t threshold,
                                            uint16_t time_ms,
                                            bool     referenced)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    threshold &= 0x07FF;  /* 11 bits max */

    /*
     * TIME_INACT is a 16-bit sample counter, not directly in milliseconds.
     * The actual duration is: time = TIME_INACT / ODR
     * So: TIME_INACT = time_ms * ODR / 1000
     *
     * This means the ODR must be set correctly before calling this function.
     */
    uint16_t odr_hz;
    switch (handle->odr) {
        case ADXL362_ODR_12_5_HZ: odr_hz =  13; break;
        case ADXL362_ODR_25_HZ:   odr_hz =  25; break;
        case ADXL362_ODR_50_HZ:   odr_hz =  50; break;
        case ADXL362_ODR_100_HZ:  odr_hz = 100; break;
        case ADXL362_ODR_200_HZ:  odr_hz = 200; break;
        case ADXL362_ODR_400_HZ:  odr_hz = 400; break;
        default:                  odr_hz = 100; break;
    }

    uint16_t time_count = (uint16_t)((uint32_t)time_ms * odr_hz / 1000U);
    if (time_count == 0) time_count = 1;  /* minimum 1 sample */

    uint8_t data[4] = {
        (uint8_t)(threshold & 0xFF),
        (uint8_t)((threshold >> 8) & 0x07),
        (uint8_t)(time_count & 0xFF),
        (uint8_t)((time_count >> 8) & 0xFF),
    };

    esp_err_t ret = _write_regs(handle->spi_dev,
                                 ADXL362_REG_THRESH_INACT_L,
                                 data,
                                 4);
    if (ret != ESP_OK) return ret;

    /* enable inactivity detection in ACT_INACT_CTL, preserve other bits */
    uint8_t act_ctl;
    ret = _read_regs(handle->spi_dev, ADXL362_REG_ACT_INACT_CTL, &act_ctl, 1);
    if (ret != ESP_OK) return ret;

    act_ctl |= ADXL362_INACT_EN;
    if (referenced)
        act_ctl |= ADXL362_INACT_REF;
    else
        act_ctl &= ~ADXL362_INACT_REF;

    ret = _write_reg(handle->spi_dev, ADXL362_REG_ACT_INACT_CTL, act_ctl);
    if (ret != ESP_OK) return ret;

    /* route inactivity event to INT2 pin (active high) */
    ret = adxl362_config_int2(handle, ADXL362_INT_INACT, false);

    ESP_LOGI(TAG, "inactivity threshold set: %u LSB, time_count: %u samples (@ %u Hz)",
             threshold, time_count, odr_hz);
    return ret;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_set_odr(adxl362_handle_t handle, adxl362_odr_t odr)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /*
     * ODR is in bits [2:0] of FILTER_CTL.
     * We keep a shadow copy to avoid reading the register every time.
     * Clear the ODR bits first, then set the new value.
     */
    handle->filter_ctl = (handle->filter_ctl & 0xF8) | (uint8_t)(odr & 0x07);
    handle->odr = odr;

    esp_err_t ret = _write_reg(handle->spi_dev,
                                ADXL362_REG_FILTER_CTL,
                                handle->filter_ctl);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ODR set to 0x%02X", odr);
    }
    return ret;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_set_range(adxl362_handle_t handle, adxl362_range_t range)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /*
     * Range is in bits [7:6] of FILTER_CTL.
     * Clear range bits, then OR in the new value.
     */
    handle->filter_ctl = (handle->filter_ctl & 0x3F) | (uint8_t)(range & 0xC0);
    handle->range = range;

    esp_err_t ret = _write_reg(handle->spi_dev,
                                ADXL362_REG_FILTER_CTL,
                                handle->filter_ctl);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "range set to 0x%02X", range);
    }
    return ret;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_config_int1(adxl362_handle_t handle,
                               uint8_t int_flags,
                               bool    active_low)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* if active_low is true, set bit 7 to invert the pin polarity */
    uint8_t val = int_flags;
    if (active_low) val |= ADXL362_INT_LOW;

    return _write_reg(handle->spi_dev, ADXL362_REG_INTMAP1, val);
}

/* --------------------------------------------------------- */

esp_err_t adxl362_config_int2(adxl362_handle_t handle,
                               uint8_t int_flags,
                               bool    active_low)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    uint8_t val = int_flags;
    if (active_low) val |= ADXL362_INT_LOW;

    return _write_reg(handle->spi_dev, ADXL362_REG_INTMAP2, val);
}

/* --------------------------------------------------------- */

esp_err_t adxl362_get_status(adxl362_handle_t handle, uint8_t *status)
{
    if (!handle || !status) return ESP_ERR_INVALID_ARG;
    return _read_regs(handle->spi_dev, ADXL362_REG_STATUS, status, 1);
}

/* --------------------------------------------------------- */

esp_err_t adxl362_soft_reset(adxl362_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* writing 0x52 to SOFT_RESET triggers a reset (it's the ASCII code for 'R') */
    esp_err_t ret = _write_reg(handle->spi_dev, ADXL362_REG_SOFT_RESET, 0x52);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));  /* give the chip time to reset */
        ESP_LOGI(TAG, "soft reset done.");
    }
    return ret;
}

/* --------------------------------------------------------- */

esp_err_t adxl362_deinit(adxl362_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    adxl362_stop_measurement(handle);     /* put chip in standby before disconnecting */
    spi_bus_remove_device(handle->spi_dev);
    free(handle);

    ESP_LOGI(TAG, "device freed.");
    return ESP_OK;
}
