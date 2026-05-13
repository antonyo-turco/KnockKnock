#include "accel_driver.h"

AccelDriver::AccelDriver() : current_range_g(2) {}

bool AccelDriver::initialize() {
    // Initialize SPI
    SPI.begin(ACCEL_PIN_SCK, ACCEL_PIN_MISO, ACCEL_PIN_MOSI, ACCEL_PIN_CS);
    pinMode(ACCEL_PIN_CS, OUTPUT);
    digitalWrite(ACCEL_PIN_CS, HIGH);
    
    delay(100);
    
    // Check device ID
    uint8_t devid_ad = readRegister(ADXL362_REG_DEVID_AD);
    uint8_t devid_mst = readRegister(ADXL362_REG_DEVID_MST);
    
    Serial.printf("ADXL362 Device IDs: AD=0x%02X MST=0x%02X\n", devid_ad, devid_mst);
    
    if (devid_ad != 0xAD || devid_mst != 0x1D) {
        Serial.println("ERROR: ADXL362 not detected!");
        return false;
    }
    
    Serial.println("✓ ADXL362 detected");
    
    // Configure for 100 Hz sampling (ADXL362 supported rates: 12.5, 25, 50, 100, 200, 400 Hz)
    // FILTER_CTL register: bits 5-4 = ODR (Output Data Rate)
    // 00 = 12.5 Hz, 01 = 25 Hz, 10 = 50 Hz, 11 = 100 Hz
    uint8_t filter_ctl = 0x13;  // ODR = 100 Hz (bits 5-4 = 11), Half-bandwidth, measurement mode
    writeRegister(ADXL362_REG_FILTER_CTL, filter_ctl);
    
    // POWER_CTL: enable measurement mode
    uint8_t power_ctl = 0x02;  // Measurement mode enabled
    writeRegister(ADXL362_REG_POWER_CTL, power_ctl);
    
    delay(10);
    
    current_range_g = 2;
    Serial.println("✓ ADXL362 initialized @ 100 Hz");
    
    return true;
}

AccelSample AccelDriver::readSample() {
    AccelSample sample;
    sample.x = readAxis(ADXL362_REG_XDATA_L);
    sample.y = readAxis(ADXL362_REG_YDATA_L);
    sample.z = readAxis(ADXL362_REG_ZDATA_L);
    return sample;
}

bool AccelDriver::setSensitivityRange(uint8_t range_g) {
    if (range_g != 2 && range_g != 4 && range_g != 8 && range_g != 16) {
        Serial.println("ERROR: Range must be 2, 4, 8, or 16 g");
        return false;
    }
    
    // Range bits in FILTER_CTL register (bits 7-6)
    // 00 = ±2g, 01 = ±4g, 10 = ±8g, 11 = ±16g
    uint8_t range_bits = 0;
    if (range_g == 4) range_bits = 0x40;
    else if (range_g == 8) range_bits = 0x80;
    else if (range_g == 16) range_bits = 0xC0;
    
    uint8_t filter_ctl = readRegister(ADXL362_REG_FILTER_CTL);
    filter_ctl = (filter_ctl & 0x3F) | range_bits;  // Clear range bits, set new ones
    writeRegister(ADXL362_REG_FILTER_CTL, filter_ctl);
    
    current_range_g = range_g;
    Serial.printf("✓ Sensitivity set to ±%d g\n", range_g);
    
    return true;
}

uint8_t AccelDriver::getCurrentRange() {
    return current_range_g;
}

uint8_t AccelDriver::readRegister(uint8_t reg_addr) {
    SPI.beginTransaction(SPISettings(ACCEL_SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(ACCEL_PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_READ_REG);
    SPI.transfer(reg_addr);
    uint8_t value = SPI.transfer(0x00);
    digitalWrite(ACCEL_PIN_CS, HIGH);
    SPI.endTransaction();
    return value;
}

void AccelDriver::writeRegister(uint8_t reg_addr, uint8_t value) {
    SPI.beginTransaction(SPISettings(ACCEL_SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(ACCEL_PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_WRITE_REG);
    SPI.transfer(reg_addr);
    SPI.transfer(value);
    digitalWrite(ACCEL_PIN_CS, HIGH);
    SPI.endTransaction();
}

int16_t AccelDriver::readAxis(uint8_t lsb_reg) {
    uint8_t lsb = readRegister(lsb_reg);
    uint8_t msb = readRegister(lsb_reg + 1);
    return ((int16_t)msb << 8) | lsb;
}
