#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <stdarg.h>
#include <math.h>

#include "config.h"
#include "feature_extraction.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

bool sending_enabled = true;
unsigned long last_oled_update = 0;
unsigned long oled_update_interval = 1000;

#define PIN_CS 4
#define PIN_MISO 3
#define PIN_MOSI 2
#define PIN_SCK 1
#define SPI_CLOCK_FREQ 1000000

#define SAMPLING_RATE_HZ 100.0f
#define SAMPLE_PERIOD_MS 10
#define BUFFER_SIZE 6000
#define BATCH_SIZE 500
#define JSON_BUFFER_SIZE 120000
#define SIGNAL_GAIN 1.0f

struct Measurement {
    int16_t x;
    int16_t y;
    int16_t z;
};

static Measurement buffer[BUFFER_SIZE];
static int buffer_index = 0;
static uint32_t batch_seq = 0;
static unsigned long last_sample_ms = 0;

#define ADXL362_CMD_READ_REG 0x0B
#define ADXL362_CMD_WRITE_REG 0x0A
#define ADXL362_REG_DEVID_AD 0x00
#define ADXL362_REG_DEVID_MST 0x01
#define ADXL362_REG_XDATA_L 0x0E
#define ADXL362_REG_YDATA_L 0x10
#define ADXL362_REG_ZDATA_L 0x12
#define ADXL362_REG_FILTER_CTL 0x2C
#define ADXL362_REG_POWER_CTL 0x2D

static bool json_append(char *&pos, size_t &remaining, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(pos, remaining, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= remaining) {
        return false;
    }
    pos += written;
    remaining -= static_cast<size_t>(written);
    return true;
}

static void set_alert_led(bool on) {
    digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
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
    return static_cast<int16_t>((static_cast<int16_t>(msb) << 8) | lsb);
}

static void wifi_connect() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print('.');
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✓ WiFi connected!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n✗ WiFi connection failed!");
    }
}

static void compute_aggregates(const Measurement samples[], int sample_count,
                               float &min_x, float &max_x, float &avg_x,
                               float &min_y, float &max_y, float &avg_y,
                               float &min_z, float &max_z, float &avg_z) {
    min_x = max_x = static_cast<float>(samples[0].x);
    min_y = max_y = static_cast<float>(samples[0].y);
    min_z = max_z = static_cast<float>(samples[0].z);

    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_z = 0.0f;

    for (int i = 0; i < sample_count; ++i) {
        float x = static_cast<float>(samples[i].x);
        float y = static_cast<float>(samples[i].y);
        float z = static_cast<float>(samples[i].z);

        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
        if (z < min_z) min_z = z;
        if (z > max_z) max_z = z;

        sum_x += x;
        sum_y += y;
        sum_z += z;
    }

    avg_x = sum_x / sample_count;
    avg_y = sum_y / sample_count;
    avg_z = sum_z / sample_count;
}

static void reset_buffer_after_send() {
    buffer_index = 0;
}

static bool send_bulk_data() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("✗ WiFi not connected!");
        return false;
    }
    if (buffer_index < BATCH_SIZE) {
        Serial.printf("Buffer not full yet: %d/%d\n", buffer_index, BATCH_SIZE);
        return true;
    }

    Serial.printf("\n=== Sending batch #%lu ===\n", static_cast<unsigned long>(batch_seq));
    Serial.printf("Samples: %d\n", buffer_index);

    static int16_t x_values[BATCH_SIZE];
    static int16_t y_values[BATCH_SIZE];
    static int16_t z_values[BATCH_SIZE];

    for (int i = 0; i < BATCH_SIZE; ++i) {
        x_values[i] = buffer[i].x;
        y_values[i] = buffer[i].y;
        z_values[i] = buffer[i].z;
    }

    float min_x, max_x, avg_x;
    float min_y, max_y, avg_y;
    float min_z, max_z, avg_z;
    compute_aggregates(buffer, BATCH_SIZE, min_x, max_x, avg_x, min_y, max_y, avg_y, min_z, max_z, avg_z);

    InferenceFeatures features = compute_features(x_values, y_values, z_values, BATCH_SIZE, SAMPLING_RATE_HZ);

    static char json_buffer[JSON_BUFFER_SIZE];
    char *pos = json_buffer;
    size_t remaining = JSON_BUFFER_SIZE;

    if (!json_append(pos, remaining,
                     "{\"device_id\":\"esp32-heltec-v4\",\"batch_seq\":%lu,\"sampling_rate_hz\":%.1f,\"batch_size\":%d,\"measurements\":[",
                     static_cast<unsigned long>(batch_seq), SAMPLING_RATE_HZ, BATCH_SIZE)) {
        Serial.println("✗ JSON header overflow");
        return false;
    }

    for (int i = 0; i < BATCH_SIZE; ++i) {
        if (!json_append(pos, remaining, "%s{\"x\":%d,\"y\":%d,\"z\":%d}",
                         (i > 0) ? "," : "", buffer[i].x, buffer[i].y, buffer[i].z)) {
            Serial.println("✗ JSON body overflow");
            return false;
        }
    }

    if (!json_append(pos, remaining,
                     "],\"aggregates\":{\"batch_seq\":%lu,\"count\":%d,\"min_x\":%.3f,\"max_x\":%.3f,\"avg_x\":%.3f,\"min_y\":%.3f,\"max_y\":%.3f,\"avg_y\":%.3f,\"min_z\":%.3f,\"max_z\":%.3f,\"avg_z\":%.3f},",
                     static_cast<unsigned long>(batch_seq), BATCH_SIZE,
                     min_x, max_x, avg_x, min_y, max_y, avg_y, min_z, max_z, avg_z)) {
        Serial.println("✗ JSON aggregates overflow");
        return false;
    }

    if (!json_append(pos, remaining,
                     "\"features\":{\"impact_score\":%.6f,\"m_p99\":%.6f,\"m_jerk_max\":%.6f,\"m_band_20_40\":%.6f,\"m_spectral_flux\":%.6f,"
                     "\"x_p99\":%.6f,\"y_p99\":%.6f,\"z_p99\":%.6f,"
                     "\"x_jerk_max\":%.6f,\"y_jerk_max\":%.6f,\"z_jerk_max\":%.6f,"
                     "\"x_band_20_40\":%.6f,\"y_band_20_40\":%.6f,\"z_band_20_40\":%.6f}",
                     features.impact_score,
                     features.m_p99,
                     features.m_jerk_max,
                     features.m_band_20_40,
                     features.m_spectral_flux,
                     features.x_p99,
                     features.y_p99,
                     features.z_p99,
                     features.x_jerk_max,
                     features.y_jerk_max,
                     features.z_jerk_max,
                     features.x_band_20_40,
                     features.y_band_20_40,
                     features.z_band_20_40)) {
        Serial.println("✗ JSON features overflow");
        return false;
    }

    if (!json_append(pos, remaining, "}")) {
        Serial.println("✗ JSON footer overflow");
        return false;
    }

    int total_size = static_cast<int>(pos - json_buffer);
    Serial.printf("JSON size: %d bytes\n", total_size);

    HTTPClient http;
    String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/data/bulk";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(5000);
    http.setTimeout(20000);

    int httpResponseCode = http.POST(reinterpret_cast<uint8_t *>(json_buffer), total_size);
    if (httpResponseCode == 201) {
        Serial.printf("✓ Server OK: %d measurements received\n", buffer_index);
        String response = http.getString();
        Serial.println("Response: " + response);
        reset_buffer_after_send();
        batch_seq++;
        http.end();
        return true;
    }

    Serial.printf("✗ Server error: %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println("Response: " + response);
    http.end();
    return false;
}

static void updateOled() {
    if (millis() - last_oled_update < oled_update_interval) {
        return;
    }
    last_oled_update = millis();

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    char buf[64];
    String ip = WiFi.localIP().toString();
    snprintf(buf, sizeof(buf), "%s", ip.c_str());
    display.println(buf);
    snprintf(buf, sizeof(buf), "Batch: %lu", static_cast<unsigned long>(batch_seq));
    display.println(buf);
    snprintf(buf, sizeof(buf), "Samples: %d", buffer_index);
    display.println(buf);
    snprintf(buf, sizeof(buf), "Sending: %s", sending_enabled ? "ON" : "OFF");
    display.println(buf);
    display.display();
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ADXL362 + Bulk WiFi + Features ===");
    Serial.printf("Sampling: %d Hz\n", static_cast<int>(SAMPLING_RATE_HZ));
    Serial.printf("Batch size: %d samples\n", BATCH_SIZE);

    pinMode(LED_BUILTIN, OUTPUT);
    set_alert_led(false);

    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);

    Wire.begin(SDA_OLED, SCL_OLED);

    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, LOW);
    delay(50);
    digitalWrite(RST_OLED, HIGH);
    delay(50);

    bool dispOk = false;
    uint8_t addrs[] = {0x3C, 0x3D};
    for (uint8_t addr : addrs) {
        if (display.begin(SSD1306_SWITCHCAPVCC, addr)) {
            Serial.printf("SSD1306 found at 0x%02X\n", addr);
            dispOk = true;
            break;
        }
    }
    if (!dispOk) {
        Serial.println("SSD1306 allocation failed");
    } else {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("Booting...");
        display.display();
    }

    wifi_connect();

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
    Serial.println("Ready to accumulate data...");
}

void loop() {
    if (sending_enabled && buffer_index >= BATCH_SIZE) {
        send_bulk_data();
        updateOled();
        return;
    }

    if (millis() - last_sample_ms < SAMPLE_PERIOD_MS) {
        updateOled();
        return;
    }
    last_sample_ms = millis();

    int16_t x_raw = adxl362_read_axis(ADXL362_REG_XDATA_L);
    int16_t y_raw = adxl362_read_axis(ADXL362_REG_YDATA_L);
    int16_t z_raw = adxl362_read_axis(ADXL362_REG_ZDATA_L);

    int16_t x_scaled = static_cast<int16_t>(x_raw * SIGNAL_GAIN);
    int16_t y_scaled = static_cast<int16_t>(y_raw * SIGNAL_GAIN);
    int16_t z_scaled = static_cast<int16_t>(z_raw * SIGNAL_GAIN);

    if (buffer_index < BUFFER_SIZE) {
        buffer[buffer_index].x = x_scaled;
        buffer[buffer_index].y = y_scaled;
        buffer[buffer_index].z = z_scaled;
        buffer_index++;
    }

    if (sending_enabled && buffer_index >= BATCH_SIZE) {
        send_bulk_data();
    }

    updateOled();
}
