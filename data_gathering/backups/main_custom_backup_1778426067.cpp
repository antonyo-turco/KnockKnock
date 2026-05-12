/* main_custom_pins.cpp
   Firmware minimo: legge ADXL362 via SPI e stampa X,Y,Z a 100 Hz.
   Usa pin custom: CS=4, MISO=3, MOSI=2, SCLK=1
*/

#include <Arduino.h>
#include <SPI.h>

#define PIN_CS   4
#define PIN_MISO 3
#define PIN_MOSI 2
#define PIN_SCK  1
#define SPI_CLOCK_FREQ 1000000

#define SAMPLING_RATE_HZ 100
#define SAMPLE_PERIOD_US (1000000UL / SAMPLING_RATE_HZ)

// ADXL362 commands/registers
#define ADXL362_CMD_READ_REG    0x0B
#define ADXL362_REG_XDATA_L     0x0E
#define ADXL362_REG_DEVID_AD    0x00
#define ADXL362_REG_DEVID_MST   0x01
#define ADXL362_REG_POWER_CTL   0x2D

uint8_t adxl_read_reg_cs(uint8_t reg, int cs){
  SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  SPI.transfer(ADXL362_CMD_READ_REG);
  SPI.transfer(reg);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
  return v;
}

void adxl_write_reg_cs(uint8_t reg, uint8_t val, int cs){
  SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  SPI.transfer(0x0A);
  SPI.transfer(reg);
  SPI.transfer(val);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
}

void adxl_read_xyz_cs(int16_t &x, int16_t &y, int16_t &z, int cs){
  SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  SPI.transfer(ADXL362_CMD_READ_REG);
  SPI.transfer(ADXL362_REG_XDATA_L);
  uint8_t xl = SPI.transfer(0x00);
  uint8_t xh = SPI.transfer(0x00);
  uint8_t yl = SPI.transfer(0x00);
  uint8_t yh = SPI.transfer(0x00);
  uint8_t zl = SPI.transfer(0x00);
  uint8_t zh = SPI.transfer(0x00);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
  x = (int16_t)((xh << 8) | xl);
  y = (int16_t)((yh << 8) | yl);
  z = (int16_t)((zh << 8) | zl);
}

void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ADXL362 custom-pins print-only (100Hz) ===");

  // configure CS pin
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  // SPI init with chosen pins
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  delay(50);

  uint8_t ad = adxl_read_reg_cs(ADXL362_REG_DEVID_AD, PIN_CS);
  uint8_t am = adxl_read_reg_cs(ADXL362_REG_DEVID_MST, PIN_CS);
  Serial.printf("DevID read on CS %d: 0x%02X 0x%02X\n", PIN_CS, ad, am);

  adxl_write_reg_cs(ADXL362_REG_POWER_CTL, 0x02, PIN_CS);
  delay(20);
}

void loop(){
  static unsigned long next_us = micros();
  unsigned long now = micros();
  if ((long)(next_us - now) > 0){
    unsigned long wait = next_us - now;
    if (wait > 2000) delay((wait/1000)-1);
    else if (wait > 0) delayMicroseconds(wait);
    now = micros();
  }

  int16_t x, y, z;
  adxl_read_xyz_cs(x, y, z, PIN_CS);
  Serial.printf("%lu, %d, %d, %d\n", millis(), x, y, z);

  next_us += SAMPLE_PERIOD_US;
}
