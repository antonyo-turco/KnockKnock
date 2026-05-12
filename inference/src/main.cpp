#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "feature_extraction.h"
#include "tinyml_baseline_model.h"

#define SAMPLE_COUNT 500
#define SAMPLING_RATE_HZ 100.0f
#define SAMPLE_PERIOD_MS 10
#define SIGNAL_GAIN 1.0f

#define PIN_CS   4
#define PIN_MISO 3
#define PIN_MOSI 2
#define PIN_SCK  1
#define SPI_CLOCK_FREQ 1000000

#define ADXL362_CMD_READ_REG    0x0B
#define ADXL362_CMD_WRITE_REG   0x0A
#define ADXL362_REG_DEVID_AD    0x00
#define ADXL362_REG_DEVID_MST   0x01
#define ADXL362_REG_XDATA_L     0x0E
#define ADXL362_REG_YDATA_L     0x10
#define ADXL362_REG_ZDATA_L     0x12
#define ADXL362_REG_FILTER_CTL  0x2C
#define ADXL362_REG_POWER_CTL   0x2D

struct Sample {
    int16_t x;
    int16_t y;
    int16_t z;
};

static Sample window[SAMPLE_COUNT];
static int window_index = 0;
static uint32_t window_seq = 0;
static unsigned long last_sample_ms = 0;
static const uint8_t ALERT_LED_PIN = LED_BUILTIN;

static void set_alert_led(bool on) {
    digitalWrite(ALERT_LED_PIN, on ? HIGH : LOW);
}

static uint8_t adxl362_read_register(uint8_t reg_addr) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_READ_REG);
    SPI.transfer(reg_addr);
    uint8_t value = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
    return value;
}

static void adxl362_write_register(uint8_t reg_addr, uint8_t value) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_WRITE_REG);
    SPI.transfer(reg_addr);
    SPI.transfer(value);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
}

static int16_t adxl362_read_axis(uint8_t lsb_reg) {
    uint8_t lsb = adxl362_read_register(lsb_reg);
    uint8_t msb = adxl362_read_register(lsb_reg + 1);
    return (int16_t)(((int16_t)msb << 8) | lsb);
}

static void init_sensor() {
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    delay(100);

    uint8_t devid_ad = adxl362_read_register(ADXL362_REG_DEVID_AD);
    uint8_t devid_mst = adxl362_read_register(ADXL362_REG_DEVID_MST);
    Serial.printf("ADXL362 DevID: 0x%02X 0x%02X\n", devid_ad, devid_mst);
    if (devid_ad != 0xAD || devid_mst != 0x1D) {
        Serial.println("ERROR: ADXL362 not found!");
    }

    adxl362_write_register(ADXL362_REG_FILTER_CTL, 0x13);
    adxl362_write_register(ADXL362_REG_POWER_CTL, 0x02);
    delay(100);
}

static bool read_sample(Sample &sample) {
    int16_t x_raw = adxl362_read_axis(ADXL362_REG_XDATA_L);
    int16_t y_raw = adxl362_read_axis(ADXL362_REG_YDATA_L);
    int16_t z_raw = adxl362_read_axis(ADXL362_REG_ZDATA_L);

    sample.x = (int16_t)(x_raw * SIGNAL_GAIN);
    sample.y = (int16_t)(y_raw * SIGNAL_GAIN);
    sample.z = (int16_t)(z_raw * SIGNAL_GAIN);
    return true;
}

static void process_window() {
    static int16_t x_values[SAMPLE_COUNT];
    static int16_t y_values[SAMPLE_COUNT];
    static int16_t z_values[SAMPLE_COUNT];

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        x_values[i] = window[i].x;
        y_values[i] = window[i].y;
        z_values[i] = window[i].z;
    }

    InferenceFeatures features = compute_features(x_values, y_values, z_values, SAMPLE_COUNT, SAMPLING_RATE_HZ);
    bool is_baseline = tinyml_is_baseline_sample(
        features.impact_score,
        features.m_p99,
        features.m_jerk_max,
        features.m_band_20_40,
        features.m_spectral_flux
    );

    set_alert_led(!is_baseline);

    Serial.printf("Window #%lu | impact=%.4f | p99=%.2f | jerk=%.2f | band=%.2f | flux=%.6f | %s\n",
                  (unsigned long)window_seq,
                  features.impact_score,
                  features.m_p99,
                  features.m_jerk_max,
                  features.m_band_20_40,
                  features.m_spectral_flux,
                  is_baseline ? "BASELINE" : "DEVIATION");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== TinyML inference firmware ===");
    Serial.printf("Window size: %d samples\n", SAMPLE_COUNT);
    Serial.printf("Sampling rate: %.1f Hz\n", SAMPLING_RATE_HZ);

    pinMode(ALERT_LED_PIN, OUTPUT);
    set_alert_led(false);

    init_sensor();
    Serial.println("Ready for inference.");
}

void loop() {
    if (millis() - last_sample_ms < SAMPLE_PERIOD_MS) {
        return;
    }
    last_sample_ms = millis();

    Sample sample{};
    if (read_sample(sample)) {
        if (window_index < SAMPLE_COUNT) {
            window[window_index++] = sample;
        }

        if (window_index >= SAMPLE_COUNT) {
            process_window();
            window_index = 0;
            window_seq++;
        }
    }
}
