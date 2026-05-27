// ─────────────────────────────────────────────────────────────────────────────
//  data_collector.ino
//  Sketch standalone per raccolta dati grezzi ADXL362 → CSV su Serial
//
//  Output: una riga CSV per campione
//    millis,x,y,z
//  dove x/y/z sono int16 grezzi (LSB, scala ±2g @ 1mg/LSB)
//
//  Baud rate: 921600
//  ODR:       400 Hz (SAMPLE_PERIOD_US = 2500 µs)
//  Hardware:  Heltec ESP32-C3 V4
//               CS   → GPIO 4
//               MISO → GPIO 3
//               MOSI → GPIO 2
//               SCK  → GPIO 1
// ─────────────────────────────────────────────────────────────────────────────

#include <SPI.h>
#include <Arduino.h>


// ── Pin e costanti ────────────────────────────────────────────────────────────

static constexpr int  PIN_CS           = 4;
static constexpr int  PIN_MISO         = 3;
static constexpr int  PIN_MOSI         = 2;
static constexpr int  PIN_SCK          = 1;
static constexpr long SERIAL_BAUD      = 921600;
static constexpr long SAMPLE_PERIOD_US = 2500;   // 400 Hz → 2500 µs

// ADXL362 SPI frequency massima: 8 MHz
static constexpr long SPI_FREQ         = 8000000;

// ── Registri ADXL362 ─────────────────────────────────────────────────────────

static constexpr uint8_t CMD_WRITE     = 0x0A;
static constexpr uint8_t CMD_READ      = 0x0B;

static constexpr uint8_t REG_SOFT_RESET = 0x1F;
static constexpr uint8_t REG_FILTER_CTL = 0x2C;
static constexpr uint8_t REG_POWER_CTL  = 0x2D;
static constexpr uint8_t REG_XDATA_L    = 0x0E;   // burst: X_L X_H Y_L Y_H Z_L Z_H

// FILTER_CTL: RANGE=00 (±2g), HALF_BW=0, EXT_SAMPLE=0, ODR=101 (400Hz)
static constexpr uint8_t FILTER_CTL_400HZ = 0b00000101;

// POWER_CTL: LOW_NOISE=00, WAKEUP=0, AUTOSLEEP=0, MEASURE=10 (measurement)
static constexpr uint8_t POWER_CTL_MEASURE = 0x02;

// ── Helpers SPI ───────────────────────────────────────────────────────────────

static void adxl_write(uint8_t reg, uint8_t val) {
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(CMD_WRITE);
    SPI.transfer(reg);
    SPI.transfer(val);
    digitalWrite(PIN_CS, HIGH);
}

static void adxl_read_xyz(int16_t &x, int16_t &y, int16_t &z) {
    uint8_t buf[6];
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(CMD_READ);
    SPI.transfer(REG_XDATA_L);
    for (int i = 0; i < 6; ++i) buf[i] = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    x = (int16_t)((buf[1] << 8) | buf[0]);
    y = (int16_t)((buf[3] << 8) | buf[2]);
    z = (int16_t)((buf[5] << 8) | buf[4]);
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial) delay(10);

    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    SPI.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));

    // Reset software
    adxl_write(REG_SOFT_RESET, 0x52);   // 'R'
    delay(10);

    // 400 Hz ODR, ±2g
    adxl_write(REG_FILTER_CTL, FILTER_CTL_400HZ);
    delay(2);

    // Avvia misurazione
    adxl_write(REG_POWER_CTL, POWER_CTL_MEASURE);
    delay(10);

    // Header CSV — commentalo se vuoi un file pulito senza header
    Serial.println("millis,x,y,z");

    // Messaggio di avvio su stderr (non inquina il CSV)
    Serial.flush();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
    static uint32_t next_us = micros();

    uint32_t now = micros();
    if ((int32_t)(now - next_us) < 0) return;   // non ancora il momento
    next_us += SAMPLE_PERIOD_US;

    int16_t x, y, z;
    adxl_read_xyz(x, y, z);

    // Stampa CSV: millis,x,y,z
    // Usiamo print/println per evitare la lentezza di printf su Serial
    Serial.print(millis());
    Serial.print(',');
    Serial.print(x);
    Serial.print(',');
    Serial.print(y);
    Serial.print(',');
    Serial.println(z);
}
