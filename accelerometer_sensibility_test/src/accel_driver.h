#ifndef ACCEL_DRIVER_H
#define ACCEL_DRIVER_H

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>

// Pin Configuration for Heltec WiFi LoRa 32 V4
#define ACCEL_PIN_CS   4
#define ACCEL_PIN_MISO 3
#define ACCEL_PIN_MOSI 2
#define ACCEL_PIN_SCK  1
#define ACCEL_SPI_CLOCK_FREQ 1000000

// ADXL362 Register Addresses
#define ADXL362_REG_DEVID_AD    0x00
#define ADXL362_REG_DEVID_MST   0x01
#define ADXL362_REG_XDATA_L     0x0E
#define ADXL362_REG_XDATA_H     0x0F
#define ADXL362_REG_YDATA_L     0x10
#define ADXL362_REG_YDATA_H     0x11
#define ADXL362_REG_ZDATA_L     0x12
#define ADXL362_REG_ZDATA_H     0x13
#define ADXL362_REG_FILTER_CTL  0x2C
#define ADXL362_REG_POWER_CTL   0x2D

// ADXL362 SPI Commands
#define ADXL362_CMD_READ_REG    0x0B
#define ADXL362_CMD_WRITE_REG   0x0A

struct AccelSample {
    int16_t x;
    int16_t y;
    int16_t z;
};

class AccelDriver {
public:
    AccelDriver();
    
    // Initialize ADXL362 @ 512 Hz
    bool initialize();
    
    // Read single sample (non-blocking)
    AccelSample readSample();
    
    // Set sensitivity range: 2, 4, 8, 16 g
    bool setSensitivityRange(uint8_t range_g);
    
    // Get current range
    uint8_t getCurrentRange();
    
private:
    uint8_t current_range_g;
    
    uint8_t readRegister(uint8_t reg_addr);
    void writeRegister(uint8_t reg_addr, uint8_t value);
    int16_t readAxis(uint8_t lsb_reg);
};

#endif
